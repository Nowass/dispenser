/**
 * M1.6b e-mail delivery tests: run inside the Workers runtime against a local D1 database
 * and a scripted fake Google (OAuth token endpoint + Gmail users.messages.send). No real
 * Google credentials or network access are used.
 */
import { env } from "cloudflare:test";
import { exports } from "cloudflare:workers";
import { beforeEach, describe, expect, it } from "vitest";
import worker from "../src/index";
import { isValidAddress, loadConfig } from "../src/delivery/config";
import { formatEmail, formatPragueTime } from "../src/delivery/format";
import { buildMessage, encodeHeaderWords, messageIdFor } from "../src/delivery/mime";
import { claimNext } from "../src/delivery/queue";
import {
  AUTH_RETRY_SECONDS,
  BACKOFF_CAP_SECONDS,
  LEASE_SECONDS,
  MAX_SENDS_PER_RUN,
  backoffSeconds,
  runDelivery,
  type DeliveryDeps,
} from "../src/delivery/run";
import { classify400 } from "../src/delivery/gmail";
import type { NotificationEvent } from "../src/protocol";

const TOKEN = "test-token-0123456789abcdef0123456789abcdef";
const SENDER = "dispenser.sender@example.com";
const RECIPIENT = "owner@example.org";
const T0 = 1795158000; // 2026-11-20T07:00:00Z

const deliveryEnv = {
  ...env,
  GMAIL_CLIENT_ID: "test-client-id.apps.googleusercontent.com",
  GMAIL_CLIENT_SECRET: "test-client-secret",
  GMAIL_REFRESH_TOKEN: "test-refresh-token",
  NOTIFY_SENDER_ADDRESS: SENDER,
  NOTIFY_RECIPIENT_ADDRESS: RECIPIENT,
} as Env;

// ---- Fake Google ------------------------------------------------------------------------

type Handler = (init: RequestInit) => Response | Promise<Response>;

const json = (status: number, body: unknown, headers: Record<string, string> = {}) =>
  new Response(JSON.stringify(body), {
    status,
    headers: { "Content-Type": "application/json", ...headers },
  });

class FakeGoogle {
  tokenScript: Handler[] = [];
  sendScript: Handler[] = [];
  tokenCalls: URLSearchParams[] = [];
  sends: { raw: string; auth: string }[] = [];
  private issued = 0;

  readonly fetch = async (url: string, init: RequestInit): Promise<Response> => {
    if (url === "https://oauth2.googleapis.com/token") {
      this.tokenCalls.push(new URLSearchParams(String(init.body)));
      const h = this.tokenScript.shift();
      if (h) return h(init);
      return json(200, { access_token: `access-${++this.issued}`, expires_in: 3599 });
    }
    if (url === "https://gmail.googleapis.com/gmail/v1/users/me/messages/send") {
      const auth = new Headers(init.headers).get("Authorization") ?? "";
      const raw = (JSON.parse(String(init.body)) as { raw: string }).raw;
      const h = this.sendScript.shift();
      const res = h ? await h(init) : json(200, { id: `gmail-${this.sends.length + 1}` });
      if (res.ok) this.sends.push({ raw, auth });
      return res;
    }
    throw new Error(`unexpected URL ${url}`);
  };
}

let google: FakeGoogle;
let clock: number;
let ownerSeq = 0;

function deps(overrides: Partial<DeliveryDeps> = {}): DeliveryDeps {
  return {
    fetch: google.fetch,
    now: () => clock,
    leaseOwner: `run-${++ownerSeq}`,
    timeoutMs: 200,
    ...overrides,
  };
}

const run = (overrides: Partial<DeliveryDeps> = {}) => runDelivery(deliveryEnv, deps(overrides));

// ---- Fixtures ---------------------------------------------------------------------------

let seq = 1;
function event(overrides: Record<string, unknown> = {}): Record<string, unknown> {
  return {
    schema_version: 1,
    event_id: `dsp-0123456789ab-b0b0b0b0-${seq++}`,
    device_id: "0123456789ab",
    type: "RESERVOIR_LOW",
    severity: "WARNING",
    code: null,
    reminder_no: 0,
    created_at: T0,
    uncertain: false,
    manual: false,
    value: 8000,
    value2: 100000,
    attempt: 1,
    ...overrides,
  };
}

/** Ingests through the real M1.6a endpoint, so rows look exactly like production rows. */
async function ingest(body: Record<string, unknown>): Promise<string> {
  const res = await exports.default.fetch(
    new Request("https://dispenser-notify.test/api/v1/events", {
      method: "POST",
      headers: { Authorization: `Bearer ${TOKEN}`, "Content-Type": "application/json" },
      body: JSON.stringify(body),
    }),
  );
  expect(res.status).toBe(201);
  return body.event_id as string;
}

interface Row {
  event_id: string;
  payload: string;
  payload_hash: string;
  delivery_status: string;
  delivery_attempts: number;
  delivered_at: string | null;
  last_error: string | null;
  next_attempt_at: number | null;
  lease_owner: string | null;
  lease_expires_at: number | null;
  provider_message_id: string | null;
}

async function row(id: string): Promise<Row> {
  const r = await env.DB.prepare("SELECT * FROM notifications WHERE event_id = ?1")
    .bind(id)
    .first<Row>();
  if (r === null) throw new Error(`no row ${id}`);
  return r;
}

// ---- Message decoding -------------------------------------------------------------------

function fromBase64(b64: string): string {
  const bin = atob(b64.replace(/-/g, "+").replace(/_/g, "/"));
  return new TextDecoder().decode(Uint8Array.from(bin, (c) => c.charCodeAt(0)));
}

function decodeWords(header: string): string {
  return header
    .replace(/\r\n /g, "")
    .replace(/=\?UTF-8\?B\?([A-Za-z0-9+/=]+)\?=/g, (_m, b64: string) => fromBase64(b64));
}

interface Decoded {
  rawText: string;
  headers: Record<string, string>;
  subject: string;
  body: string;
}

function decode(raw: string): Decoded {
  const rawText = fromBase64(raw);
  const [head, bodyB64] = rawText.split("\r\n\r\n") as [string, string];
  const headers: Record<string, string> = {};
  for (const line of head.replace(/\r\n /g, " ").split("\r\n")) {
    const i = line.indexOf(": ");
    headers[line.slice(0, i)] = line.slice(i + 2);
  }
  return {
    rawText,
    headers,
    subject: decodeWords(head.match(/^Subject: ((?:.*)(?:\r\n .*)*)/m)![1]!),
    body: fromBase64(bodyB64.replace(/\r\n/g, "")),
  };
}

beforeEach(async () => {
  await env.DB.exec("DELETE FROM notifications");
  await env.DB.exec(
    "UPDATE delivery_state SET consecutive_failures = 0, last_error = NULL, last_error_at = NULL, last_success_at = NULL, auth_attention = 0, updated_at = NULL",
  );
  google = new FakeGoogle();
  // D1 stamps received_at with the real clock, so the test clock starts at real time.
  clock = Math.floor(Date.now() / 1000);
});

// ---- Tests ------------------------------------------------------------------------------

describe("successful delivery", () => {
  it("sends one Czech e-mail, records SENT and never touches the immutable event", async () => {
    const id = await ingest(event());
    const before = await row(id);
    const report = await run();
    expect(report).toMatchObject({ status: "done", sent: 1, retried: 0, failed: 0, released: 0 });

    // OAuth: refresh-token grant with the configured client, then the Bearer token on send.
    expect(google.tokenCalls).toHaveLength(1);
    expect(google.tokenCalls[0]!.get("grant_type")).toBe("refresh_token");
    expect(google.tokenCalls[0]!.get("client_id")).toBe(deliveryEnv.GMAIL_CLIENT_ID);
    expect(google.tokenCalls[0]!.get("refresh_token")).toBe("test-refresh-token");
    expect(google.sends).toHaveLength(1);
    expect(google.sends[0]!.auth).toBe("Bearer access-1");

    const m = decode(google.sends[0]!.raw);
    expect(m.headers.From).toBe(
      `=?UTF-8?B?${btoa(String.fromCharCode(...new TextEncoder().encode("Dávkovač V2")))}?= <${SENDER}>`,
    );
    expect(m.headers.To).toBe(`<${RECIPIENT}>`);
    expect(m.headers["Message-ID"]).toBe(`<${id}@dispenser-notify.invalid>`);
    expect(m.headers["X-Dispenser-Event-Id"]).toBe(id);
    expect(m.headers["Content-Type"]).toBe("text/plain; charset=UTF-8");
    expect(m.headers["Auto-Submitted"]).toBe("auto-generated");
    expect(m.subject).toBe("[Dávkovač V2] VAROVÁNÍ: Nízká zásoba v zásobníku (8 %)");
    expect(m.body).toContain(`ID události: ${id}`);

    const after = await row(id);
    expect(after.delivery_status).toBe("SENT");
    expect(after.delivery_attempts).toBe(1);
    expect(after.delivered_at).toMatch(/^\d{4}-\d{2}-\d{2} \d{2}:\d{2}:\d{2}$/);
    expect(after.provider_message_id).toBe("gmail-1");
    expect(after.lease_owner).toBeNull();
    expect(after.lease_expires_at).toBeNull();
    expect(after.last_error).toBeNull();
    expect(after.event_id).toBe(before.event_id);
    expect(after.payload).toBe(before.payload);
    expect(after.payload_hash).toBe(before.payload_hash);

    // Nothing due afterwards: no Google traffic at all.
    expect((await run()).status).toBe("idle");
    expect(google.tokenCalls).toHaveLength(1);
  });

  it("sends critical events first, then the oldest, at most MAX_SENDS_PER_RUN per run", async () => {
    const warn = await ingest(event());
    const crit = await ingest(
      event({ type: "DEVICE_ERROR", severity: "CRITICAL", code: "STORAGE", value: 0, value2: 0 }),
    );
    for (let i = 0; i < MAX_SENDS_PER_RUN; i++) await ingest(event());
    const report = await run();
    expect(report.sent).toBe(MAX_SENDS_PER_RUN);
    expect(decode(google.sends[0]!.raw).headers["X-Dispenser-Event-Id"]).toBe(crit);
    expect(decode(google.sends[1]!.raw).headers["X-Dispenser-Event-Id"]).toBe(warn);
    expect((await run()).sent).toBe(2); // the rest in the next run
  });

  it("does nothing without configuration (fails closed, no Google traffic)", async () => {
    await ingest(event());
    const report = await runDelivery(env, deps());
    expect(report.status).toBe("disabled");
    expect(report.detail).toContain("GMAIL_REFRESH_TOKEN missing");
    expect(google.tokenCalls).toHaveLength(0);
    // The real scheduled handler without configuration completes without throwing.
    await expect(
      worker.scheduled({ cron: "*/5 * * * *", scheduledTime: 0, noRetry() {} }, env),
    ).resolves.toBeUndefined();
  });

  it("rejects addresses that could inject headers; endpoint overrides only for loopback", () => {
    for (const bad of [
      "owner@example.org\r\nBcc: x@y.z",
      "Owner <owner@example.org>",
      "a@b",
      "@x.cz",
      "",
    ]) {
      expect(isValidAddress(bad), bad).toBe(false);
    }
    expect(isValidAddress("first.last+tag@sub.example.cz")).toBe(true);
    const bad = loadConfig({ ...deliveryEnv, NOTIFY_RECIPIENT_ADDRESS: "x@y.z\nBcc: evil@z.z" });
    expect(bad.ok).toBe(false);
    const remote = loadConfig({ ...deliveryEnv, GMAIL_API_BASE_OVERRIDE: "https://evil.example" });
    expect(remote.ok && remote.config.gmailApiBase).toBe("https://gmail.googleapis.com");
    const local = loadConfig({
      ...deliveryEnv,
      GOOGLE_TOKEN_URL_OVERRIDE: "http://127.0.0.1:9911/token",
    });
    expect(local.ok && local.config.tokenUrl).toBe("http://127.0.0.1:9911/token");
  });
});

describe("authentication", () => {
  it("expired access token: refreshes once and retries the send", async () => {
    const id = await ingest(event());
    google.sendScript.push(() => json(401, { error: { status: "UNAUTHENTICATED" } }));
    const report = await run();
    expect(report.sent).toBe(1);
    expect(google.tokenCalls).toHaveLength(2);
    expect(google.sends[0]!.auth).toBe("Bearer access-2");
    expect((await row(id)).delivery_status).toBe("SENT");
    expect(report.health?.auth_attention).toBe(false);
  });

  it("authentication failure: no attempt consumed, event stays PENDING", async () => {
    const id = await ingest(event());
    google.sendScript.push(
      () => json(401, { error: { status: "UNAUTHENTICATED" } }),
      () => json(401, { error: { status: "UNAUTHENTICATED" } }),
    );
    const report = await run();
    expect(report.status).toBe("config_error");
    expect(report.released).toBe(1);
    expect(report.health).toMatchObject({ auth_attention: true, attention: true, pending: 1 });
    const r = await row(id);
    expect(r.delivery_status).toBe("PENDING");
    expect(r.delivery_attempts).toBe(0);
    expect(r.last_error).toBe("gmail_unauthorized");
    expect(r.lease_owner).toBeNull();
    // Missing scope, disabled account, unknown user, Gmail not enabled: configuration too.
    const accountErrors: [number, unknown, string][] = [
      [
        403,
        { error: { errors: [{ reason: "insufficientPermissions" }] } },
        "gmail_403_insufficientPermissions",
      ],
      [404, { error: { errors: [{ reason: "notFound" }] } }, "gmail_404_notFound"],
      [
        400,
        { error: { errors: [{ reason: "failedPrecondition" }] } },
        "gmail_400_failedPrecondition",
      ],
      [
        400,
        { error: { message: "Invalid To header", errors: [{ reason: "invalidArgument" }] } },
        "gmail_400_invalidArgument",
      ],
    ];
    for (const [status, body, detail] of accountErrors) {
      clock += AUTH_RETRY_SECONDS;
      google.sendScript.push(() => json(status, body));
      expect((await run()).status, detail).toBe("config_error");
      expect((await row(id)).delivery_attempts).toBe(0);
      expect((await row(id)).last_error).toBe(detail);
      expect((await row(id)).delivery_status).toBe("PENDING");
    }
  });

  it(
    "revoked or expired refresh token: events preserved, Google retried every 30 min, recovery after repair",
    { timeout: 120_000 },
    async () => {
      const ids = [await ingest(event()), await ingest(event())];
      // The refresh token is revoked (or the 7-day Testing limit hit) for a long time.
      const revoked = () =>
        json(400, {
          error: "invalid_grant",
          error_description: "Token has been expired or revoked.",
        });
      google.tokenScript.push(revoked);
      expect(await run()).toMatchObject({ status: "config_error", detail: "token_invalid_grant" });
      // Every 5 minutes for 3 days: at most one token request per 30 minutes, nothing claimed.
      for (let k = 1; k <= 3 * 288; k++) {
        clock += 300;
        google.tokenScript.push(revoked);
        const r = await run();
        expect(["auth_wait", "config_error"]).toContain(r.status);
      }
      expect(google.tokenCalls.length).toBeLessThanOrEqual(1 + (3 * 86400) / AUTH_RETRY_SECONDS);
      expect(google.tokenCalls.length).toBeGreaterThan(100);
      expect(google.sends).toHaveLength(0);
      for (const id of ids) {
        const r = await row(id);
        expect(r.delivery_status).toBe("PENDING");
        expect(r.delivery_attempts).toBe(0);
        expect(r.lease_owner).toBeNull();
      }
      const h = (await run()).health!;
      expect(h).toMatchObject({ auth_attention: true, attention: true, pending: 2, failed: 0 });
      expect(h.last_error).toBe("token_invalid_grant");
      expect(h.oldest_pending_age_s).toBeGreaterThan(3 * 86400 - 1000);
      expect(h.consecutive_failures).toBeGreaterThan(100);
      // The administrator re-authorizes (new refresh token): delivered within 30 minutes.
      google.tokenScript = [];
      clock += AUTH_RETRY_SECONDS;
      const fixed = await run();
      expect(fixed.sent).toBe(2);
      expect(fixed.health).toMatchObject({
        auth_attention: false,
        attention: false,
        pending: 0,
        consecutive_failures: 0,
        last_success_at: clock,
      });
      for (const id of ids) expect((await row(id)).delivery_status).toBe("SENT");
    },
  );

  it("a temporary token-endpoint failure claims nothing and needs no administrator", async () => {
    const id = await ingest(event());
    google.tokenScript.push(() => json(503, {}));
    const r = await run();
    expect(r).toMatchObject({ status: "stopped", detail: "token_http_503" });
    expect(r.health?.auth_attention).toBe(false);
    expect((await row(id)).delivery_attempts).toBe(0);
    expect((await run()).sent).toBe(1); // no auth wait for temporary problems
  });

  it("missing configuration: disabled, flagged for attention, state written only on change", async () => {
    await ingest(event());
    const r = await runDelivery({ ...env, DB: env.DB }, deps());
    expect(r.status).toBe("disabled");
    expect(r.health).toMatchObject({ auth_attention: true, last_error: "config_incomplete" });
    const before = await env.DB.prepare("SELECT updated_at FROM delivery_state").first<{
      updated_at: number;
    }>();
    clock += 300;
    await runDelivery({ ...env, DB: env.DB }, deps());
    const after = await env.DB.prepare("SELECT updated_at FROM delivery_state").first<{
      updated_at: number;
    }>();
    expect(after?.updated_at).toBe(before?.updated_at);
    // Once configured, delivery starts immediately (no auth wait for configuration gaps).
    expect((await run()).sent).toBe(1);
    expect((await run()).health?.auth_attention).toBe(false);
  });
});

describe("retries", () => {
  it("rate limiting (429 + Retry-After) postpones and stops the run", async () => {
    const a = await ingest(event());
    const b = await ingest(event());
    google.sendScript.push(() =>
      json(
        429,
        { error: { errors: [{ reason: "rateLimitExceeded" }] } },
        { "Retry-After": "3600" },
      ),
    );
    const report = await run();
    expect(report).toMatchObject({ status: "stopped", retried: 1, sent: 0 });
    const ra = await row(a);
    expect(ra.next_attempt_at).toBe(clock + 3600); // Retry-After beats the 60 s backoff
    expect(ra.last_error).toBe("gmail_429_rateLimitExceeded");
    expect(ra.delivery_attempts).toBe(1);
    expect((await row(b)).delivery_attempts).toBe(0); // the run stopped
    // 403 rateLimitExceeded is a rate limit as well (exponential backoff).
    google.sendScript.push(() =>
      json(403, { error: { errors: [{ reason: "userRateLimitExceeded" }] } }),
    );
    expect((await run()).retried).toBe(1);
    expect((await row(b)).next_attempt_at).toBe(clock + 60);
  });

  it("temporary Gmail errors retry with exponential backoff until delivered", async () => {
    const id = await ingest(event());
    google.sendScript.push(
      () => json(503, {}),
      () => json(500, {}),
    );
    expect((await run()).retried).toBe(1);
    expect((await row(id)).next_attempt_at).toBe(clock + 60);
    clock += 59;
    expect((await run()).status).toBe("idle"); // not due yet
    clock += 1;
    expect((await run()).retried).toBe(1);
    const r = await row(id);
    expect(r.delivery_attempts).toBe(2);
    expect(r.next_attempt_at).toBe(clock + 120);
    expect(r.last_error).toBe("gmail_500");
    clock += 120;
    expect((await run()).sent).toBe(1);
    expect((await row(id)).delivery_attempts).toBe(3);
  });

  it("backoff doubles from 1 minute and is capped at 6 hours", () => {
    expect([1, 2, 3, 4, 5].map(backoffSeconds)).toEqual([60, 120, 240, 480, 960]);
    expect(backoffSeconds(9)).toBe(15360);
    expect(backoffSeconds(10)).toBe(BACKOFF_CAP_SECONDS);
    expect(backoffSeconds(1000)).toBe(BACKOFF_CAP_SECONDS);
  });

  it("more than 16 temporary failures (every kind) are followed by successful delivery", async () => {
    const id = await ingest(event());
    const kinds: (() => Response | Promise<Response>)[] = [
      () => json(429, { error: { errors: [{ reason: "rateLimitExceeded" }] } }),
      () => json(500, {}),
      () => json(502, {}),
      () => json(503, {}),
      () => json(504, {}),
      () => json(408, {}),
      () => json(418, {}), // unexpected status: temporary, never abandoned
      () => {
        throw new TypeError("Network connection lost.");
      },
    ];
    for (let k = 0; k < 24; k++) {
      google.sendScript.push(kinds[k % kinds.length]!);
      const r = await run();
      expect(r.status, `attempt ${k + 1}`).toBe("stopped");
      const rr = await row(id);
      expect(rr.delivery_status).toBe("PENDING");
      expect(rr.delivery_attempts).toBe(k + 1);
      expect(rr.next_attempt_at! - clock).toBe(backoffSeconds(k + 1));
      clock = rr.next_attempt_at!;
    }
    expect((await run()).sent).toBe(1);
    const r = await row(id);
    expect(r.delivery_status).toBe("SENT");
    expect(r.delivery_attempts).toBe(25);
  });

  it("a long Gmail outage (two weeks) never abandons notifications", async () => {
    const ids = [
      await ingest(event()),
      await ingest(event({ type: "BATTERY_LOW", value: 3300, value2: 0 })),
    ];
    const start = clock;
    let runs = 0;
    // Gmail answers 503 to everything for 14 days; a cron run every 15 minutes (the real
    // cron runs every 5; the backoff makes the extra idle runs irrelevant).
    while (clock < start + 14 * 86400) {
      google.sendScript.push(() => json(503, {}));
      await run();
      google.sendScript = [];
      clock += 900;
      runs++;
    }
    expect(runs).toBe(14 * 96);
    expect(google.sends).toHaveLength(0);
    for (const id of ids) {
      const r = await row(id);
      expect(r.delivery_status).toBe("PENDING"); // never FAILED, never dropped
      expect(r.next_attempt_at! - clock).toBeLessThanOrEqual(BACKOFF_CAP_SECONDS);
    }
    google.sendScript.push(() => json(503, {})); // still down for the health check
    const h = (await run()).health!;
    google.sendScript = [];
    expect(h).toMatchObject({ pending: 2, failed: 0, auth_attention: false, attention: true });
    expect(h.consecutive_failures).toBeGreaterThanOrEqual(3);
    expect(h.last_error).toBe("gmail_503");
    expect(h.oldest_pending_age_s).toBeGreaterThanOrEqual(14 * 86400 - 900);
    // Gmail is back: everything is delivered once due.
    clock += BACKOFF_CAP_SECONDS;
    const after = await run();
    expect(after.sent).toBe(2);
    expect(after.health).toMatchObject({ pending: 0, consecutive_failures: 0, attention: false });
  }, 120_000);

  it("permanent invalid-message failures mark the event FAILED and are not retried", async () => {
    const bad = await ingest(event());
    const other = await ingest(event());
    // Gmail's response when the raw RFC 822 payload is missing or undecodable.
    google.sendScript.push(() =>
      json(400, {
        error: {
          code: 400,
          message:
            "'raw' RFC822 payload message string or uploading message via /upload/* URL required",
          errors: [{ reason: "invalidArgument", domain: "global" }],
          status: "INVALID_ARGUMENT",
        },
      }),
    );
    const r = await run();
    // The rejection does not block the unrelated event: it is delivered in the same run.
    expect(r).toMatchObject({ status: "done", failed: 1, sent: 1 });
    expect(r.health).toMatchObject({ failed: 1, consecutive_failures: 0, auth_attention: false });
    expect(r.health?.attention).toBe(true); // a FAILED event is an operational problem
    expect(r.health?.last_error).toBe("gmail_400_invalid_payload");
    const rr = await row(bad);
    expect(rr.delivery_status).toBe("FAILED");
    expect(rr.last_error).toBe("gmail_400_invalid_payload");
    expect((await row(other)).delivery_status).toBe("SENT");
    clock += 86400;
    expect((await run()).status).toBe("idle");
    // Too large (413) is permanent as well; other events are unaffected.
    const big = await ingest(event());
    const ok = await ingest(event());
    google.sendScript.push(() => json(413, { error: { status: "PAYLOAD_TOO_LARGE" } }));
    const r2 = await run();
    expect(r2).toMatchObject({ failed: 1, sent: 1 });
    expect((await row(big)).delivery_status).toBe("FAILED");
    expect((await row(ok)).delivery_status).toBe("SENT");
    expect(google.sends).toHaveLength(2);
  });

  it("network timeout and network errors are retried (the send may have happened)", async () => {
    const id = await ingest(event());
    google.sendScript.push(
      (init) =>
        new Promise<Response>((_resolve, reject) => {
          init.signal?.addEventListener("abort", () => reject(init.signal?.reason));
        }),
    );
    expect((await run()).retried).toBe(1);
    expect((await row(id)).last_error).toBe("gmail_timeout");
    clock += 60;
    google.sendScript.push(() => {
      throw new TypeError("Network connection lost.");
    });
    expect((await run()).retried).toBe(1);
    expect((await row(id)).last_error).toBe("gmail_network");
  });
});

describe("Gmail HTTP 400 classification (conservative)", () => {
  const e400 = (message: string | undefined, reason = "invalidArgument", status?: string) => ({
    error: {
      code: 400,
      ...(message === undefined ? {} : { message }),
      errors: [{ reason, domain: "global", ...(message === undefined ? {} : { message }) }],
      ...(status === undefined ? {} : { status }),
    },
  });

  it("representative Gmail 400 responses", () => {
    const cases: [Record<string, unknown> | null, "config" | "permanent" | "unclear"][] = [
      // Configuration / account: never FAILED, no attempt consumed.
      [e400("Precondition check failed.", "failedPrecondition", "FAILED_PRECONDITION"), "config"],
      [e400("Mail service not enabled", "failedPrecondition"), "config"],
      [e400("Invalid To header"), "config"],
      [e400("Invalid From header"), "config"],
      [e400("Recipient address required"), "config"],
      [e400("Delegation denied for sender@example.com", "delegationDenied"), "config"],
      [e400("Request violates domain policy"), "config"],
      // Clearly an invalid, non-recoverable payload: FAILED.
      [
        e400("'raw' RFC822 payload message string or uploading message via /upload/* URL required"),
        "permanent",
      ],
      [e400("Invalid raw field"), "permanent"],
      [e400("Message too large"), "permanent"],
      [e400("Message exceeds maximum message size"), "permanent"],
      // Ambiguous: PENDING plus a health warning.
      [e400("Bad Request", "badRequest"), "unclear"],
      [e400("Invalid argument"), "unclear"],
      [e400(undefined, "invalidArgument", "INVALID_ARGUMENT"), "unclear"],
      [{}, "unclear"],
      [null, "unclear"],
    ];
    for (const [body, expected] of cases) {
      expect(classify400(body), JSON.stringify(body)).toBe(expected);
    }
  });

  it("an unclear 400 keeps the event PENDING, warns, and does not block other events", async () => {
    const unclear = await ingest(event());
    const other = await ingest(event());
    google.sendScript.push(() => json(400, e400("Bad Request", "badRequest")));
    const lines: Record<string, string | number>[] = [];
    const r = await run({ log: (f) => lines.push(f) });
    expect(r).toMatchObject({ status: "done", sent: 1, retried: 1, failed: 0 });
    expect((await row(other)).delivery_status).toBe("SENT");
    const u = await row(unclear);
    expect(u.delivery_status).toBe("PENDING");
    expect(u.last_error).toBe("gmail_400_unclear_badRequest");
    expect(u.next_attempt_at).toBe(clock + backoffSeconds(1));
    expect(r.health).toMatchObject({
      pending: 1,
      failed: 0,
      unclear_pending: 1,
      auth_attention: false,
      attention: true,
    });
    expect(lines.find((l) => l.outcome === "held")).toMatchObject({ level: "warn" });
    expect(lines.find((l) => l.evt === "delivery_health")).toMatchObject({
      level: "warn",
      unclear_pending: 1,
    });
    // Still unclear on the next attempts: never FAILED, backoff grows.
    for (let k = 2; k <= 5; k++) {
      clock += backoffSeconds(k - 1);
      google.sendScript.push(() => json(400, e400("Bad Request", "badRequest")));
      const rk = await run();
      expect(rk.retried).toBe(1);
      expect((await row(unclear)).delivery_status).toBe("PENDING");
      expect((await row(unclear)).next_attempt_at).toBe(clock + backoffSeconds(k));
    }
    // Whatever caused it is fixed: delivered, and the warning clears by itself.
    clock += backoffSeconds(5);
    const fixed = await run();
    expect(fixed.sent).toBe(1);
    expect((await row(unclear)).delivery_status).toBe("SENT");
    expect(fixed.health).toMatchObject({ pending: 0, unclear_pending: 0, attention: false });
  });

  it("a configuration 400 consumes no attempt and recovers once the configuration is fixed", async () => {
    const a = await ingest(event());
    const b = await ingest(event());
    // E.g. a malformed NOTIFY_RECIPIENT_ADDRESS accepted by us but rejected by Gmail.
    google.sendScript.push(() => json(400, e400("Invalid To header")));
    const r = await run();
    expect(r).toMatchObject({ status: "config_error", released: 1, sent: 0 });
    expect(r.health).toMatchObject({ auth_attention: true, attention: true, pending: 2 });
    for (const id of [a, b]) {
      expect((await row(id)).delivery_status).toBe("PENDING");
      expect((await row(id)).delivery_attempts).toBe(0);
    }
    // Repeated for a day (configuration still wrong): attempts never consumed.
    for (let k = 0; k < 48; k++) {
      clock += AUTH_RETRY_SECONDS;
      google.sendScript.push(() => json(400, e400("Invalid To header")));
      expect((await run()).status).toBe("config_error");
    }
    for (const id of [a, b]) {
      expect((await row(id)).delivery_attempts).toBe(0);
      expect((await row(id)).delivery_status).toBe("PENDING");
    }
    // The administrator fixes the secret: delivered after the authorization cooldown.
    clock += AUTH_RETRY_SECONDS;
    const fixed = await run();
    expect(fixed.sent).toBe(2);
    expect(fixed.health).toMatchObject({
      pending: 0,
      failed: 0,
      unclear_pending: 0,
      auth_attention: false,
      consecutive_failures: 0,
      attention: false,
    });
  });
});

describe("diagnostics", () => {
  it("a FAILED event keeps the health warning after later successful runs", async () => {
    const bad = await ingest(event());
    google.sendScript.push(() => json(413, { error: { status: "PAYLOAD_TOO_LARGE" } }));
    expect((await run()).failed).toBe(1);
    for (let k = 0; k < 3; k++) {
      clock += 3600;
      await ingest(event());
      const lines: Record<string, string | number>[] = [];
      const r = await run({ log: (f) => lines.push(f) });
      expect(r.sent).toBe(1);
      expect(r.health).toMatchObject({
        failed: 1,
        pending: 0,
        consecutive_failures: 0,
        auth_attention: false,
        attention: true,
      });
      expect(lines.find((l) => l.evt === "delivery_health")).toMatchObject({
        level: "warn",
        failed: 1,
      });
    }
    // Idle runs keep warning too.
    clock += 86400;
    const idle = await run();
    expect(idle.status).toBe("idle");
    expect(idle.health?.attention).toBe(true);
    // The administrator handles the event (documented: requeue or delete): warning clears.
    await env.DB.prepare(
      "DELETE FROM notifications WHERE event_id = ?1 AND delivery_status = 'FAILED'",
    )
      .bind(bad)
      .run();
    expect((await run()).health).toMatchObject({ failed: 0, attention: false });
  });

  it("health snapshot: pending, failed, oldest age, consecutive failures, last error, auth", async () => {
    const idle = await run();
    expect(idle.status).toBe("idle");
    expect(idle.health).toEqual({
      pending: 0,
      failed: 0,
      unclear_pending: 0,
      oldest_pending_age_s: null,
      consecutive_failures: 0,
      last_error: null,
      last_error_at: null,
      last_success_at: null,
      auth_attention: false,
      attention: false,
    });
    await ingest(event());
    for (let k = 1; k <= 3; k++) {
      google.sendScript.push(() => json(503, {}));
      const r = await run();
      expect(r.health?.consecutive_failures).toBe(k);
      expect(r.health?.attention).toBe(k >= 3); // 3 failed runs in a row need a look
      clock += backoffSeconds(k);
    }
    const h = (await run()).health!; // sent now
    expect(h).toMatchObject({ pending: 0, consecutive_failures: 0, attention: false });
    expect(h.last_error).toBe("gmail_503"); // the last error stays visible after recovery
    expect(h.last_success_at).toBe(clock);
  });

  it("an old pending event raises attention even without errors", async () => {
    await ingest(event());
    await env.DB.prepare("UPDATE notifications SET next_attempt_at = ?1")
      .bind(clock + 7200)
      .run();
    expect((await run()).health?.attention).toBe(false);
    clock += 3600 + 60;
    const h = (await run()).health!;
    expect(h.oldest_pending_age_s).toBeGreaterThanOrEqual(3600);
    expect(h.attention).toBe(true);
  });

  it("logs one structured delivery_health line per run, without secrets or content", async () => {
    await ingest(event({ value: 7777 }));
    const lines: Record<string, string | number>[] = [];
    google.sendScript.push(() => json(503, {}));
    await run({ log: (f) => lines.push(f) });
    const health = lines.find((l) => l.evt === "delivery_health");
    expect(health).toMatchObject({
      level: "info", // one failed run is not yet "attention" (threshold: 3)
      status: "stopped",
      pending: 1,
      failed: 0,
      consecutive_failures: 1,
      auth_attention: 0,
      last_error: "gmail_503",
    });
    const text = JSON.stringify(lines);
    for (const secret of [
      "test-refresh-token",
      "test-client-secret",
      "access-1",
      SENDER,
      RECIPIENT,
      "7777",
      "Nízká",
    ]) {
      expect(text).not.toContain(secret);
    }
  });
});

describe("leases and concurrency", () => {
  it("an active lease prevents duplicate processing; an expired lease is recovered", async () => {
    const id = await ingest(event());
    // A run claims the event and dies (Worker interruption) before sending.
    const claimed = await claimNext(env.DB, "crashed-run", clock, LEASE_SECONDS);
    expect(claimed?.event_id).toBe(id);
    expect((await run()).status).toBe("idle");
    clock += LEASE_SECONDS - 1;
    expect((await run()).status).toBe("idle");
    expect(google.sends).toHaveLength(0);
    clock += 1; // lease expired
    expect((await run()).sent).toBe(1);
    const r = await row(id);
    expect(r.delivery_status).toBe("SENT");
    expect(r.delivery_attempts).toBe(2); // the interrupted attempt is counted
  });

  it("concurrent runs never deliver an event whose lease is active", async () => {
    const leased = await ingest(event());
    const free = await ingest(event());
    expect((await claimNext(env.DB, "slow-run", clock, LEASE_SECONDS))?.event_id).toBe(leased);
    const reports = await Promise.all([run(), run(), run(), run()]);
    expect(reports.reduce((n, r) => n + r.sent, 0)).toBe(1);
    const sentIds = google.sends.map((s) => decode(s.raw).headers["X-Dispenser-Event-Id"]);
    expect(sentIds).toEqual([free]);
    const r = await row(leased);
    expect(r.delivery_status).toBe("PENDING");
    expect(r.lease_owner).toBe("slow-run");
  });

  it("interruption after Gmail accepted: re-sent later with the same Message-ID (at-least-once)", async () => {
    const id = await ingest(event());
    // The D1 write that records SENT fails (Worker dies right after Gmail accepted).
    const failingSent = {
      prepare(sql: string) {
        if (sql.includes("delivery_status = 'SENT'")) throw new Error("worker killed");
        return env.DB.prepare(sql);
      },
    };
    const report = await runDelivery(
      { ...deliveryEnv, DB: failingSent as unknown as D1Database },
      deps(),
    );
    expect(report.status).toBe("storage_error");
    expect(google.sends).toHaveLength(1);
    expect((await row(id)).delivery_status).toBe("PENDING");
    clock += LEASE_SECONDS;
    expect((await run()).sent).toBe(1);
    expect(google.sends).toHaveLength(2);
    const ids = google.sends.map((s) => decode(s.raw).headers["Message-ID"]);
    expect(ids[0]).toBe(messageIdFor(id));
    expect(ids[1]).toBe(ids[0]);
  });

  it("concurrent scheduled invocations send every event exactly once", async () => {
    const ids: string[] = [];
    for (let i = 0; i < 8; i++) ids.push(await ingest(event()));
    google.sendScript = Array.from({ length: 8 }, (_v, i) => async () => {
      await new Promise((r) => setTimeout(r, 5 + (i % 3) * 5));
      return json(200, { id: `gmail-c${i}` });
    });
    const reports = await Promise.all([run(), run(), run()]);
    expect(reports.reduce((n, r) => n + r.sent, 0)).toBe(8);
    const sentIds = google.sends.map((s) => decode(s.raw).headers["X-Dispenser-Event-Id"]);
    expect(new Set(sentIds).size).toBe(8);
    expect(sentIds.sort()).toEqual([...ids].sort());
    for (const id of ids) expect((await row(id)).delivery_status).toBe("SENT");
  });

  it("an unreadable stored payload is marked FAILED instead of blocking the queue", async () => {
    await env.DB.prepare(
      "INSERT INTO notifications (event_id, device_id, event_type, severity, payload, payload_hash) " +
        "VALUES ('dsp-0123456789ab-b0b0b0b0-999999', '0123456789ab', 'RESERVOIR_LOW', 'WARNING', '{broken', 'x')",
    ).run();
    const ok = await ingest(event());
    const report = await run();
    expect(report).toMatchObject({ failed: 1, sent: 1 });
    expect((await row("dsp-0123456789ab-b0b0b0b0-999999")).last_error).toBe(
      "invalid_stored_payload",
    );
    expect((await row(ok)).delivery_status).toBe("SENT");
  });
});

describe("Czech formatting", () => {
  const base: NotificationEvent = {
    event_id: "dsp-0123456789ab-a5a5a5a5-42",
    device_id: "0123456789ab",
    type: "RESERVOIR_LOW",
    severity: "WARNING",
    code: null,
    reminder_no: 0,
    created_at: T0,
    uncertain: false,
    manual: false,
    value: 8000,
    value2: 100000,
  };
  const fmt = (o: Partial<NotificationEvent>, received: string | null = "2026-11-20 07:00:05") =>
    formatEmail({ ...base, ...o }, received);

  it("reservoir LOW: percentage, volumes, uncertainty, times and event ID", () => {
    const { subject, body } = fmt({});
    expect(subject).toBe("[Dávkovač V2] VAROVÁNÍ: Nízká zásoba v zásobníku (8 %)");
    expect(body).toContain("Zařízení: 01:23:45:67:89:AB");
    expect(body).toContain("Závažnost: VAROVÁNÍ");
    expect(body).toContain("Zásoba (odhad): 8 % (8,0 ml z 100,0 ml)");
    expect(body).toContain("Odhad zásoby je nejistý: ne");
    expect(body).toContain("Čas události: 20. 11. 2026 08:00 SEČ");
    expect(body).toContain("Přijato serverem: 20. 11. 2026 08:00 SEČ");
    expect(body).toContain("ID události: dsp-0123456789ab-a5a5a5a5-42");
    // Percent is rounded down: 19 999 of 100 000 µl is 19 %, never "20 %".
    expect(fmt({ value: 19999 }).subject).toContain("(19 %)");
    expect(fmt({ uncertain: true }).body).toContain("Odhad zásoby je nejistý: ano");
  });

  it("reservoir BLOCKED and reminders", () => {
    const { subject, body } = fmt({
      type: "RESERVOIR_BLOCKED",
      severity: "CRITICAL",
      value: 0,
      reminder_no: 2,
    });
    expect(subject).toBe(
      "[Dávkovač V2] KRITICKÉ: Připomínka – Automatické dávkování je zablokováno (0 %)",
    );
    expect(body).toContain("Připomínka č. 2: stav stále trvá.");
    expect(body).toContain("Zásoba (odhad): 0 % (0,0 ml z 100,0 ml)");
  });

  it("battery LOW with and without a measurement", () => {
    const withV = fmt({ type: "BATTERY_LOW", value: 3300, value2: 0 });
    expect(withV.subject).toBe("[Dávkovač V2] VAROVÁNÍ: Slabá baterie (3,30 V)");
    expect(withV.body).toContain("Napětí baterie: 3,30 V");
    const without = fmt({ type: "BATTERY_LOW", value: 0, value2: 0 });
    expect(without.subject).toBe("[Dávkovač V2] VAROVÁNÍ: Slabá baterie");
    expect(without.body).toContain("Napětí baterie: neuvedeno");
    expect(without.body).not.toMatch(/\d,\d\d V/);
  });

  it("dosing errors: every code, manual vs automatic, consumption note", () => {
    const titles: Record<string, string> = {
      START_FAILED: "Dávku se nepodařilo spustit",
      PUMP_FAILED: "Selhání čerpadla",
      INTERRUPTED: "Dávkování bylo přerušeno",
      STORAGE: "Dávka neproběhla – chyba úložiště",
      BLOCKED: "Dávka neproběhla – blokováno",
    };
    for (const [code, title] of Object.entries(titles)) {
      const { subject, body } = fmt({
        type: "DOSING_ERROR",
        severity: "CRITICAL",
        code,
        manual: code === "INTERRUPTED",
        value: 2000,
        value2: 0,
      });
      expect(subject).toBe(`[Dávkovač V2] KRITICKÉ: ${title}`);
      expect(body).toContain("Požadované množství: 2,0 ml");
      expect(body).toContain(
        code === "INTERRUPTED" ? "Druh dávky: ruční" : "Druh dávky: plánovaná (automatická)",
      );
      expect(body).toContain(
        code === "STORAGE" || code === "BLOCKED"
          ? "Dávka nebyla spuštěna."
          : "odhad je do příštího doplnění nejistý",
      );
    }
  });

  it("device errors and missing optional measurements", () => {
    const sync = fmt({
      type: "DEVICE_ERROR",
      severity: "CRITICAL",
      code: "TIME_SYNC",
      value: 7,
      value2: 0,
    });
    expect(sync.subject).toBe("[Dávkovač V2] KRITICKÉ: Synchronizace času je opožděná");
    expect(sync.body).toContain("Neúspěšné pokusy o synchronizaci: 7");
    const clockErr = fmt({
      type: "DEVICE_ERROR",
      severity: "CRITICAL",
      code: "CLOCK",
      value: 0,
      value2: 0,
      created_at: null,
    });
    expect(clockErr.body).toContain("Čas zařízení není znám");
    expect(clockErr.body).not.toContain("Neúspěšné pokusy");
    expect(clockErr.body).toContain(
      "Čas události: neznámý (hodiny zařízení nebyly synchronizovány)",
    );
    const overflow = fmt({
      type: "DEVICE_ERROR",
      severity: "CRITICAL",
      code: "QUEUE_OVERFLOW",
      value: 3,
      value2: 0,
    });
    expect(overflow.body).toContain("Počet ztracených upozornění: 3");
    const storage = fmt({
      type: "DEVICE_ERROR",
      severity: "CRITICAL",
      code: "STORAGE",
      value: 0,
      value2: 0,
    });
    expect(storage.subject).toBe("[Dávkovač V2] KRITICKÉ: Chyba úložiště zařízení");
    const noCap = fmt({ value: 0, value2: 0 }, null);
    expect(noCap.subject).toBe("[Dávkovač V2] VAROVÁNÍ: Nízká zásoba v zásobníku");
    expect(noCap.body).toContain("Zásoba (odhad): neuvedena");
    expect(noCap.body).toContain("Přijato serverem: neznámo");
  });

  it("times use Europe/Prague with daylight saving", () => {
    expect(formatPragueTime(T0)).toBe("20. 11. 2026 08:00 SEČ");
    expect(formatPragueTime(1782900000)).toBe("1. 7. 2026 12:00 SELČ");
  });

  it("encoding: ASCII-only message, RFC 2047 subject, UTF-8 base64 body", () => {
    const subject =
      "[Dávkovač V2] KRITICKÉ: Připomínka – Automatické dávkování je zablokováno (0 %)";
    const words = encodeHeaderWords(subject);
    for (const w of words.split("\r\n ")) expect(w.length).toBeLessThanOrEqual(75);
    expect(decodeWords(words)).toBe(subject);
    expect(encodeHeaderWords("plain ascii")).toBe("plain ascii");
    const body = "Příliš žluťoučký kůň úpěl ďábelské ódy.\nŘádek 2";
    const text = buildMessage({
      fromName: "Dávkovač V2",
      from: SENDER,
      to: RECIPIENT,
      subject,
      body,
      messageId: messageIdFor("dsp-0123456789ab-a5a5a5a5-1"),
      eventId: "dsp-0123456789ab-a5a5a5a5-1",
      date: new Date(T0 * 1000),
    });
    expect(/^[\x00-\x7f]*$/.test(text)).toBe(true);
    expect(text).toContain("Date: Fri, 20 Nov 2026 07:00:00 +0000");
    const raw = btoa(text).replace(/\+/g, "-").replace(/\//g, "_").replace(/=+$/, "");
    const m = decode(raw);
    expect(m.subject).toBe(subject);
    expect(m.body).toBe(body.replace(/\n/g, "\r\n"));
    for (const line of text.split("\r\n")) expect(line.length).toBeLessThanOrEqual(998);
  });
});
