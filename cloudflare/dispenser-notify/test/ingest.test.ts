/**
 * M1.6a ingestion tests, run inside the Workers runtime against a local D1 database.
 * Numbers in test names refer to the required scenarios of the M1.6a brief.
 */
import { env } from "cloudflare:test";
import { exports } from "cloudflare:workers";
import { describe, expect, it } from "vitest";
import worker, { handle } from "../src/index";
import { canonicalPayload, payloadHash, validateRequest } from "../src/protocol";

const TOKEN = "test-token-0123456789abcdef0123456789abcdef";
const BASE = "https://dispenser-notify.test";
const DEVICE = "0123456789ab";

let seqCounter = 1;
/** A fresh, unique event ID per call (tests share one database). */
function nextId(): string {
  return `dsp-${DEVICE}-a5a5a5a5-${seqCounter++}`;
}

type Body = Record<string, unknown>;

function lowEvent(overrides: Body = {}): Body {
  return {
    schema_version: 1,
    event_id: nextId(),
    device_id: DEVICE,
    type: "RESERVOIR_LOW",
    severity: "WARNING",
    code: null,
    reminder_no: 0,
    created_at: 1795158000,
    uncertain: false,
    manual: false,
    value: 8000,
    value2: 100000,
    attempt: 1,
    ...overrides,
  };
}

function post(body: unknown, init: { token?: string | null; raw?: string; headers?: Body } = {}) {
  const headers = new Headers({ "Content-Type": "application/json" });
  const token = init.token === undefined ? TOKEN : init.token;
  if (token !== null) headers.set("Authorization", `Bearer ${token}`);
  for (const [k, v] of Object.entries(init.headers ?? {})) headers.set(k, String(v));
  return new Request(`${BASE}/api/v1/events`, {
    method: "POST",
    headers,
    body: init.raw ?? JSON.stringify(body),
  });
}

function send(req: Request): Promise<Response> {
  return exports.default.fetch(req);
}

async function rowCount(eventId: string): Promise<number> {
  const r = await env.DB.prepare("SELECT COUNT(*) AS n FROM notifications WHERE event_id = ?1")
    .bind(eventId)
    .first<{ n: number }>();
  return r?.n ?? -1;
}

async function storedRow(eventId: string) {
  return env.DB.prepare("SELECT * FROM notifications WHERE event_id = ?1")
    .bind(eventId)
    .first<Record<string, unknown>>();
}

/** Calls the Worker with a replaced DB binding (failure injection). */
function sendWithDb(req: Request, db: unknown): Promise<Response> {
  return worker.fetch(req, { ...env, DB: db as D1Database });
}

/** A D1Database stand-in delegating to the real one, with selected operations failing. */
function faultyDb(mode: "insert-throws" | "insert-unsuccessful" | "select-throws" | "select-null") {
  return {
    prepare(sql: string) {
      const real = env.DB.prepare(sql);
      return {
        bind(...args: unknown[]) {
          const stmt = real.bind(...args);
          return {
            async run() {
              if (mode === "insert-throws") throw new Error("D1_ERROR: simulated");
              const r = await stmt.run();
              return mode === "insert-unsuccessful" ? { ...r, success: false } : r;
            },
            async first() {
              if (mode === "select-throws") throw new Error("D1_ERROR: simulated");
              if (mode === "select-null") return null;
              return stmt.first();
            },
          };
        },
      };
    },
  };
}

describe("health", () => {
  it("1. answers without credentials and discloses nothing sensitive", async () => {
    const res = await send(new Request(`${BASE}/health`));
    expect(res.status).toBe(200);
    const text = await res.text();
    expect(JSON.parse(text)).toEqual({ status: "ok", service: "dispenser-notify", protocol: 1 });
    expect(text).not.toContain(TOKEN);
    expect(res.headers.get("Cache-Control")).toBe("no-store");
  });

  it("rejects other methods", async () => {
    const res = await send(new Request(`${BASE}/health`, { method: "POST" }));
    expect(res.status).toBe(405);
  });
});

describe("authentication", () => {
  it("2. missing token is rejected and nothing is stored", async () => {
    const body = lowEvent();
    const res = await send(post(body, { token: null }));
    expect(res.status).toBe(401);
    expect(res.headers.get("WWW-Authenticate")).toContain("Bearer");
    expect(await res.json()).toEqual({ status: "unauthorized" });
    expect(await rowCount(body.event_id as string)).toBe(0);
  });

  it("3. invalid tokens are rejected", async () => {
    const body = lowEvent();
    for (const header of [
      `Bearer ${TOKEN}x`,
      `Bearer ${TOKEN.slice(0, -1)}`,
      `bearer ${TOKEN}`,
      `Bearer  ${TOKEN}`,
      `Basic ${TOKEN}`,
      TOKEN,
      "Bearer ",
      `Bearer ${"a".repeat(5000)}`,
    ]) {
      const req = post(body, { token: null, headers: { Authorization: header } });
      expect((await send(req)).status).toBe(401);
    }
    expect(await rowCount(body.event_id as string)).toBe(0);
  });

  it("4. valid token is accepted", async () => {
    const res = await send(post(lowEvent()));
    expect(res.status).toBe(201);
  });

  it("missing or weak secret configuration fails closed (no acceptance, retryable)", async () => {
    for (const secret of [undefined, "", "short-secret"]) {
      const body = lowEvent();
      const e = { ...env, DEVICE_API_TOKEN: secret } as unknown as Env;
      const res = await worker.fetch(post(body), e);
      expect(res.status).toBe(503);
      expect(await rowCount(body.event_id as string)).toBe(0);
      // Not even with a matching (weak) token.
      if (secret) {
        const res2 = await worker.fetch(post(body, { token: secret }), e);
        expect(res2.status).toBe(503);
      }
    }
  });

  it("authentication is checked before the body is parsed", async () => {
    const res = await send(post(null, { token: "wrong", raw: "{not json" }));
    expect(res.status).toBe(401);
  });
});

describe("request validation", () => {
  it("5. malformed JSON is rejected", async () => {
    for (const raw of ["{", "", "[1,", '{"a":}', "ÿþ", "null", "[]", "42", '"x"']) {
      const res = await send(post(null, { raw }));
      expect(res.status).toBe(400);
      const j = (await res.json()) as Body;
      expect(["malformed_json", "invalid_field"]).toContain(j.error);
    }
    // Invalid UTF-8 bytes.
    const req = new Request(`${BASE}/api/v1/events`, {
      method: "POST",
      headers: { Authorization: `Bearer ${TOKEN}`, "Content-Type": "application/json" },
      body: new Uint8Array([0x7b, 0xff, 0x7d]),
    });
    expect(((await (await send(req)).json()) as Body).error).toBe("malformed_json");
  });

  it("6. unsupported schema versions are rejected", async () => {
    for (const v of [0, 2, "1", 1.5, null]) {
      const res = await send(post(lowEvent({ schema_version: v })));
      expect(res.status).toBe(400);
      expect(((await res.json()) as Body).error).toBe("unsupported_schema_version");
    }
    const noVersion = lowEvent();
    delete noVersion.schema_version;
    const res = await send(post(noVersion));
    expect(await res.json()).toEqual({
      status: "invalid",
      error: "missing_field",
      field: "schema_version",
    });
  });

  it("7. every required field is required; unknown fields are rejected", async () => {
    for (const field of Object.keys(lowEvent())) {
      if (field === "schema_version") continue;
      const body = lowEvent();
      delete body[field];
      const res = await send(post(body));
      expect(res.status).toBe(400);
      expect(await res.json()).toEqual({ status: "invalid", error: "missing_field", field });
    }
    const res = await send(post(lowEvent({ extra: 1 })));
    expect(await res.json()).toEqual({ status: "invalid", error: "unknown_field", field: "extra" });
  });

  it("8. invalid event IDs are rejected", async () => {
    const bad = [
      "",
      "dsp-0123456789ab-a5a5a5a5-0", // seq 0 = empty slot
      "dsp-0123456789ab-a5a5a5a5-01", // leading zero
      "dsp-0123456789ab-a5a5a5a5-4294967295", // UINT32_MAX never assigned
      "dsp-0123456789ab-a5a5a5a5-99999999999",
      "dsp-0123456789ab-00000000-1", // epoch 0 = unassigned
      "dsp-0123456789AB-a5a5a5a5-1", // uppercase hex
      "dsp-0123456789-a5a5a5a5-1",
      "dsp-0123456789ab-a5a5a5-1",
      "xyz-0123456789ab-a5a5a5a5-1",
      "dsp-0123456789ab-a5a5a5a5-1 ",
      "dsp-ffffffffffff-a5a5a5a5-1", // device part != device_id
      42,
      null,
    ];
    for (const event_id of bad) {
      const res = await send(post(lowEvent({ event_id })));
      expect(res.status).toBe(400);
      const j = (await res.json()) as Body;
      expect(["event_id", "device_id"]).toContain(j.field);
    }
    // Largest valid sequence number.
    const max = await send(post(lowEvent({ event_id: `dsp-${DEVICE}-a5a5a5a6-4294967294` })));
    expect(max.status).toBe(201);
  });

  it("9. oversized bodies are rejected (declared or streamed)", async () => {
    const big = lowEvent({ pad: "x".repeat(5000) });
    const res = await send(post(big));
    expect(res.status).toBe(413);
    // Chunked body without Content-Length.
    const stream = new ReadableStream({
      start(c) {
        for (let i = 0; i < 10; i++) c.enqueue(new TextEncoder().encode("x".repeat(1000)));
        c.close();
      },
    });
    const req = new Request(`${BASE}/api/v1/events`, {
      method: "POST",
      headers: { Authorization: `Bearer ${TOKEN}`, "Content-Type": "application/json" },
      body: stream,
    });
    expect((await send(req)).status).toBe(413);
  });

  it("rejects wrong content types and methods; unknown paths are 404", async () => {
    const res = await send(post(lowEvent(), { headers: { "Content-Type": "text/plain" } }));
    expect(res.status).toBe(415);
    const get = await send(
      new Request(`${BASE}/api/v1/events`, { headers: { Authorization: `Bearer ${TOKEN}` } }),
    );
    expect(get.status).toBe(405);
    expect(get.headers.get("Allow")).toBe("POST");
    expect((await send(new Request(`${BASE}/`))).status).toBe(404);
  });

  it("validates types, severities, codes, ranges and firmware structural rules", async () => {
    const cases: [Body, string][] = [
      [{ type: "RESERVOIR_EMPTY" }, "type"],
      [{ type: "toString" }, "type"],
      [{ severity: "CRITICAL" }, "severity"], // LOW is WARNING
      [{ severity: "INFO" }, "severity"],
      [{ code: "STORAGE" }, "code"], // reservoir events carry no code
      [{ type: "DOSING_ERROR", severity: "CRITICAL", code: null }, "code"],
      [{ type: "DOSING_ERROR", severity: "CRITICAL", code: "CLOCK" }, "code"],
      [{ type: "DEVICE_ERROR", severity: "CRITICAL", code: "PUMP_FAILED" }, "code"],
      [
        { type: "DOSING_ERROR", severity: "CRITICAL", code: "PUMP_FAILED", reminder_no: 1 },
        "reminder_no",
      ],
      [
        { type: "DEVICE_ERROR", severity: "CRITICAL", code: "QUEUE_OVERFLOW", reminder_no: 2 },
        "reminder_no",
      ],
      [{ reminder_no: 256 }, "reminder_no"],
      [{ reminder_no: -1 }, "reminder_no"],
      [{ reminder_no: 1.5 }, "reminder_no"],
      [{ manual: true }, "manual"], // only dosing errors are manual
      [{ manual: "false" }, "manual"],
      [{ uncertain: 0 }, "uncertain"],
      [{ created_at: 1735689599 }, "created_at"],
      [{ created_at: 4133980800 }, "created_at"],
      [{ created_at: "2026-11-20T07:00:00Z" }, "created_at"],
      [{ created_at: 0 }, "created_at"], // unknown clock is null, not 0
      [{ value: -1 }, "value"],
      [{ value: 4294967296 }, "value"],
      [{ value: 1e400 }, "value"], // serialized as null
      [{ value2: "1" }, "value2"],
      [{ attempt: 0 }, "attempt"],
      [{ attempt: 65536 }, "attempt"],
      [{ device_id: "0123456789AB" }, "device_id"],
    ];
    // A literal that parses to Infinity.
    const inf = JSON.stringify(lowEvent()).replace('"value":8000', '"value":1e400');
    expect(((await (await send(post(null, { raw: inf }))).json()) as Body).field).toBe("value");
    for (const [override, field] of cases) {
      const res = await send(post(lowEvent(override)));
      expect(res.status, JSON.stringify(override)).toBe(400);
      expect(((await res.json()) as Body).field, JSON.stringify(override)).toBe(field);
    }
  });

  it("rejects prototype-pollution keys and keeps duplicate-key handling deterministic", async () => {
    for (const key of ["__proto__", "constructor", "prototype"]) {
      const raw = JSON.stringify(lowEvent()).replace("{", `{"${key}":{"x":1},`);
      const res = await send(post(null, { raw }));
      expect(res.status, key).toBe(400);
      expect(((await res.json()) as Body).error).toBe("unknown_field");
    }
    // Duplicate keys: JSON.parse keeps the last value; validation and the canonical
    // payload then use that value only (the firmware never sends duplicate keys).
    const body = lowEvent();
    const raw = JSON.stringify(body).replace('"value":8000', '"value":1,"value":8000');
    expect((await send(post(null, { raw }))).status).toBe(201);
    const row = await storedRow(body.event_id as string);
    expect(JSON.parse(row!.payload as string).value).toBe(8000);
  });

  it("accepts every firmware event type with its valid codes", async () => {
    const valid: Body[] = [
      { type: "RESERVOIR_LOW", severity: "WARNING", code: null, reminder_no: 3, uncertain: true },
      { type: "RESERVOIR_BLOCKED", severity: "CRITICAL", code: null, value: 0 },
      { type: "BATTERY_LOW", severity: "WARNING", code: null, value: 3300, value2: 0 },
      ...["START_FAILED", "PUMP_FAILED", "INTERRUPTED", "STORAGE", "BLOCKED"].map((code) => ({
        type: "DOSING_ERROR",
        severity: "CRITICAL",
        code,
        manual: true,
        value: 2000,
        value2: 0,
      })),
      ...["STORAGE", "TIME_SYNC", "CLOCK"].map((code) => ({
        type: "DEVICE_ERROR",
        severity: "CRITICAL",
        code,
        reminder_no: 1,
      })),
      { type: "DEVICE_ERROR", severity: "CRITICAL", code: "QUEUE_OVERFLOW", value: 2 },
      { created_at: null }, // clock unknown at creation
      { created_at: 1735689600 },
      { created_at: 4133980799, value: 4294967295, value2: 4294967295, attempt: 65535 },
    ];
    for (const override of valid) {
      const res = await send(post(lowEvent(override)));
      expect(res.status, JSON.stringify(override)).toBe(201);
    }
  });
});

describe("durable storage and idempotency", () => {
  it("10. a first submission is stored as PENDING with its canonical payload, then acknowledged", async () => {
    const body = lowEvent({
      type: "DOSING_ERROR",
      severity: "CRITICAL",
      code: "INTERRUPTED",
      manual: true,
    });
    const res = await send(post(body));
    expect(res.status).toBe(201);
    expect(await res.json()).toEqual({ status: "accepted", event_id: body.event_id });
    const row = await storedRow(body.event_id as string);
    expect(row).not.toBeNull();
    expect(row!.device_id).toBe(DEVICE);
    expect(row!.event_type).toBe("DOSING_ERROR");
    expect(row!.severity).toBe("CRITICAL");
    expect(row!.delivery_status).toBe("PENDING");
    expect(row!.delivery_attempts).toBe(0);
    expect(row!.delivered_at).toBeNull();
    expect(row!.last_error).toBeNull();
    expect(typeof row!.received_at).toBe("string");
    const parsed = validateRequest(body);
    if (!parsed.ok) throw new Error("fixture invalid");
    const canonical = canonicalPayload(parsed.value.event);
    expect(row!.payload).toBe(canonical);
    expect(row!.payload_hash).toBe(await payloadHash(canonical));
    expect(row!.payload_hash).toMatch(/^[0-9a-f]{64}$/);
    // The attempt counter and schema version are transport metadata, not stored payload.
    expect(row!.payload).not.toContain("attempt");
    expect(row!.payload).not.toContain("schema_version");
  });

  it("11. an identical retry is acknowledged as duplicate without modifying the row", async () => {
    const body = lowEvent();
    expect((await send(post(body))).status).toBe(201);
    const before = await storedRow(body.event_id as string);
    // Same event, different attempt number, key order and number spelling.
    const reordered = Object.fromEntries(Object.entries({ ...body, attempt: 7 }).reverse());
    const raw = JSON.stringify(reordered).replace('"value":8000', '"value":8.0e3');
    const res = await send(post(null, { raw }));
    expect(res.status).toBe(200);
    expect(await res.json()).toEqual({ status: "duplicate", event_id: body.event_id });
    expect(await storedRow(body.event_id as string)).toEqual(before);
    expect(await rowCount(body.event_id as string)).toBe(1);
  });

  it("12. the same ID with a modified payload is a conflict; the stored payload is unchanged", async () => {
    const body = lowEvent();
    expect((await send(post(body))).status).toBe(201);
    const before = await storedRow(body.event_id as string);
    for (const change of [
      { value: 5000 },
      { uncertain: true },
      { created_at: null },
      { reminder_no: 1 },
    ]) {
      const res = await send(post({ ...body, ...change }));
      expect(res.status).toBe(409);
      expect(await res.json()).toEqual({ status: "conflict", event_id: body.event_id });
    }
    expect(await storedRow(body.event_id as string)).toEqual(before);
  });

  it("13. concurrent identical submissions: exactly one accepted, the rest duplicates", async () => {
    const body = lowEvent();
    const results = await Promise.all(
      Array.from({ length: 12 }, (_, i) => send(post({ ...body, attempt: i + 1 }))),
    );
    const statuses = results.map((r) => r.status).sort();
    expect(statuses.filter((s) => s === 201)).toHaveLength(1);
    expect(statuses.filter((s) => s === 200)).toHaveLength(11);
    expect(await rowCount(body.event_id as string)).toBe(1);
  });

  it("14. concurrent conflicting submissions: one stored, the others conflict", async () => {
    const body = lowEvent();
    const variants = Array.from({ length: 8 }, (_, i) => ({ ...body, value: 1000 + i }));
    const results = await Promise.all(variants.map((v) => send(post(v))));
    const accepted = results.map((r, i) => [r.status, i] as const).filter(([s]) => s === 201);
    expect(accepted).toHaveLength(1);
    expect(results.filter((r) => r.status === 409)).toHaveLength(7);
    const row = await storedRow(body.event_id as string);
    const winner = variants[accepted[0]![1]]!;
    expect(JSON.parse(row!.payload as string).value).toBe(winner.value);
  });

  it("15./16. storage failures return a retryable error and never a false acknowledgement", async () => {
    for (const mode of ["insert-throws", "insert-unsuccessful"] as const) {
      const body = lowEvent();
      const res = await sendWithDb(post(body), faultyDb(mode));
      expect(res.status, mode).toBe(503);
      expect(res.headers.get("Retry-After")).toBe("60");
      expect(await res.json()).toEqual({ status: "retry" });
    }
    // Existing row, but the duplicate check cannot read it: no "duplicate" ACK either.
    const body = lowEvent();
    expect((await send(post(body))).status).toBe(201);
    for (const mode of ["select-throws", "select-null"] as const) {
      const res = await sendWithDb(post(body), faultyDb(mode));
      expect(res.status, mode).toBe(503);
    }
    // A binding that throws on anything at all.
    const broken = {
      prepare() {
        throw new Error("binding unavailable");
      },
    };
    const res = await sendWithDb(post(lowEvent()), broken);
    expect(res.status).toBe(503);
    // After the failure the device retries the same event: stored exactly once.
    const retried = lowEvent();
    expect((await sendWithDb(post(retried), faultyDb("insert-throws"))).status).toBe(503);
    expect(await rowCount(retried.event_id as string)).toBe(0);
    expect((await send(post(retried))).status).toBe(201);
    expect((await send(post(retried))).status).toBe(200);
  });

  it("17. stored events survive a Worker restart (state lives only in D1)", async () => {
    const body = lowEvent();
    expect((await send(post(body))).status).toBe(201);
    // A fresh handler invocation with a fresh env object shares no in-memory state;
    // only D1 knows the event. (The real process restart is test/integration/restart.mjs.)
    const fresh = { DB: env.DB, DEVICE_API_TOKEN: TOKEN } as Env;
    expect((await handle(post(body), fresh)).status).toBe(200);
    expect((await handle(post({ ...body, value: 1 }), fresh)).status).toBe(409);
  });

  it("18. stored events are not readable through the API", async () => {
    const body = lowEvent({ value: 777777 });
    expect((await send(post(body))).status).toBe(201);
    const auth = { Authorization: `Bearer ${TOKEN}` };
    const probes = [
      new Request(`${BASE}/api/v1/events`, { headers: auth }),
      new Request(`${BASE}/api/v1/events/${body.event_id as string}`, { headers: auth }),
      new Request(`${BASE}/api/v1/events?event_id=${body.event_id as string}`, { headers: auth }),
      new Request(`${BASE}/api/v1/notifications`, { headers: auth }),
      new Request(`${BASE}/health?debug=1`),
    ];
    for (const req of probes) {
      const res = await send(req);
      expect(res.status === 404 || res.status === 405 || res.status === 200).toBe(true);
      const text = await res.text();
      expect(text).not.toContain("777777");
      expect(text).not.toContain("payload");
    }
    // Neither a duplicate nor a conflict response echoes the stored payload.
    for (const variant of [body, { ...body, value: 1 }]) {
      const text = await (await send(post(variant))).text();
      expect(text).not.toContain("777777");
      expect(text).not.toContain("payload_hash");
    }
    // Unauthenticated duplicate probing reveals nothing (401 before any lookup).
    expect((await send(post(body, { token: null }))).status).toBe(401);
  });
});
