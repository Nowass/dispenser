#!/usr/bin/env node
/**
 * Local integration test (M1.6b): the real Worker under `wrangler dev --test-scheduled`
 * with a LOCAL persisted D1 database and a fake Google (OAuth token endpoint + Gmail send)
 * on 127.0.0.1. Ingests an event, fires the Cron Trigger, checks the e-mail and the D1
 * state, restarts the Worker and checks that nothing is sent twice, then checks retry
 * scheduling after a Gmail 503.
 *
 * Never touches Cloudflare or Google: --local only, fake credentials, throwaway database.
 */
import { spawn, spawnSync } from "node:child_process";
import { randomBytes } from "node:crypto";
import { mkdtempSync, rmSync } from "node:fs";
import { createServer } from "node:http";
import { tmpdir } from "node:os";
import { dirname, join } from "node:path";
import { fileURLToPath } from "node:url";

const root = join(dirname(fileURLToPath(import.meta.url)), "..", "..");
const wrangler = join(root, "node_modules", ".bin", "wrangler");
const persist = mkdtempSync(join(tmpdir(), "dispenser-delivery-it-"));
const token = randomBytes(24).toString("hex");
const port = 19787 + Math.floor(Math.random() * 1000);
const base = `http://127.0.0.1:${port}`;
const env = { ...process.env, CI: "1", WRANGLER_SEND_METRICS: "false", NO_COLOR: "1" };

let failures = 0;
function check(cond, what) {
  console.log(`${cond ? "ok  " : "FAIL"} ${what}`);
  if (!cond) failures++;
}

// ---- Fake Google ----
const google = { mode: "ok", tokens: 0, sends: [] };
const fake = createServer((req, res) => {
  let body = "";
  req.on("data", (d) => (body += d));
  req.on("end", () => {
    res.setHeader("Content-Type", "application/json");
    if (req.method === "POST" && req.url === "/token") {
      const p = new URLSearchParams(body);
      if (p.get("grant_type") !== "refresh_token" || p.get("refresh_token") !== "fake-refresh") {
        res.writeHead(400).end(JSON.stringify({ error: "invalid_grant" }));
        return;
      }
      google.tokens++;
      res.end(JSON.stringify({ access_token: `fake-access-${google.tokens}`, expires_in: 3599 }));
      return;
    }
    if (req.method === "POST" && req.url === "/gmail/v1/users/me/messages/send") {
      if (!String(req.headers.authorization).startsWith("Bearer fake-access-")) {
        res.writeHead(401).end("{}");
        return;
      }
      if (google.mode === "unavailable") {
        res.writeHead(503).end("{}");
        return;
      }
      google.sends.push(JSON.parse(body).raw);
      res.end(JSON.stringify({ id: `fake-msg-${google.sends.length}` }));
      return;
    }
    res.writeHead(404).end("{}");
  });
});
await new Promise((r) => fake.listen(0, "127.0.0.1", r));
const fakeBase = `http://127.0.0.1:${fake.address().port}`;

function wranglerSync(args) {
  const r = spawnSync(wrangler, args, { cwd: root, env, encoding: "utf8" });
  if (r.status !== 0) throw new Error(`wrangler ${args[0]} failed:\n${r.stdout}\n${r.stderr}`);
  return r.stdout;
}

function sql(command) {
  const out = wranglerSync([
    "d1",
    "execute",
    "dispenser-notifications",
    "--local",
    "--persist-to",
    persist,
    "--json",
    "--command",
    command,
  ]);
  return JSON.parse(out)[0].results;
}

const vars = {
  DEVICE_API_TOKEN: token,
  GMAIL_CLIENT_ID: "fake-client.apps.googleusercontent.com",
  GMAIL_CLIENT_SECRET: "fake-client-secret",
  GMAIL_REFRESH_TOKEN: "fake-refresh",
  NOTIFY_SENDER_ADDRESS: "sender@example.com",
  NOTIFY_RECIPIENT_ADDRESS: "recipient@example.org",
  GMAIL_API_BASE_OVERRIDE: fakeBase,
  GOOGLE_TOKEN_URL_OVERRIDE: `${fakeBase}/token`,
};

async function startWorker() {
  const args = ["dev", "--local", "--test-scheduled", "--ip", "127.0.0.1", "--port", String(port)];
  args.push("--persist-to", persist, "--show-interactive-dev-session=false");
  for (const [k, v] of Object.entries(vars)) args.push("--var", `${k}:${v}`);
  const child = spawn(wrangler, args, { cwd: root, env, stdio: ["ignore", "pipe", "pipe"] });
  let output = "";
  child.stdout.on("data", (d) => (output += d));
  child.stderr.on("data", (d) => (output += d));
  for (let i = 0; i < 120; i++) {
    if (child.exitCode !== null) throw new Error(`wrangler dev exited:\n${output}`);
    try {
      if ((await fetch(`${base}/health`)).ok) return child;
    } catch {
      /* not up yet */
    }
    await new Promise((r) => setTimeout(r, 500));
  }
  child.kill("SIGKILL");
  throw new Error(`wrangler dev did not start:\n${output}`);
}

async function stopWorker(child) {
  const exited = new Promise((r) => child.once("exit", r));
  child.kill("SIGTERM");
  const timer = setTimeout(() => child.kill("SIGKILL"), 10000);
  await exited;
  clearTimeout(timer);
}

/** Fires the Cron Trigger once (wrangler dev --test-scheduled). */
async function cron() {
  const res = await fetch(`${base}/__scheduled?cron=${encodeURIComponent("*/5 * * * *")}`);
  await res.text();
  return res.status;
}

function post(event) {
  return fetch(`${base}/api/v1/events`, {
    method: "POST",
    headers: { Authorization: `Bearer ${token}`, "Content-Type": "application/json" },
    body: JSON.stringify(event),
  });
}

function event(seq, overrides = {}) {
  return {
    schema_version: 1,
    event_id: `dsp-0123456789ab-c0ffee00-${seq}`,
    device_id: "0123456789ab",
    type: "RESERVOIR_LOW",
    severity: "WARNING",
    code: null,
    reminder_no: 0,
    created_at: 1795158000,
    uncertain: true,
    manual: false,
    value: 8000,
    value2: 100000,
    attempt: 1,
    ...overrides,
  };
}

function decodeRaw(raw) {
  const text = Buffer.from(raw, "base64url").toString("utf8");
  const [head, body] = text.split("\r\n\r\n");
  const subject = head
    .match(/^Subject: ((?:.*)(?:\r\n .*)*)/m)[1]
    .replace(/\r\n /g, "")
    .replace(/=\?UTF-8\?B\?([A-Za-z0-9+/=]+)\?=/g, (_m, b) =>
      Buffer.from(b, "base64").toString("utf8"),
    );
  return { head, subject, body: Buffer.from(body.replace(/\r\n/g, ""), "base64").toString("utf8") };
}

let worker;
try {
  wranglerSync([
    "d1",
    "migrations",
    "apply",
    "dispenser-notifications",
    "--local",
    "--persist-to",
    persist,
  ]);
  worker = await startWorker();

  check((await post(event(1))).status === 201, "event ingested (201)");
  check((await cron()) === 200, "cron trigger fired");
  check(google.sends.length === 1, "exactly one e-mail sent via the fake Gmail API");
  if (google.sends[0]) {
    const m = decodeRaw(google.sends[0]);
    check(
      m.subject === "[Dávkovač V2] VAROVÁNÍ: Nízká zásoba v zásobníku (8 %)",
      "Czech subject decoded",
    );
    check(m.body.includes("Odhad zásoby je nejistý: ano"), "uncertainty shown in the body");
    check(
      m.head.includes("Message-ID: <dsp-0123456789ab-c0ffee00-1@dispenser-notify.invalid>"),
      "stable Message-ID",
    );
    check(m.head.includes("To: <recipient@example.org>"), "configured recipient");
  }
  let rows = sql(
    "SELECT delivery_status, delivery_attempts, lease_owner, provider_message_id FROM notifications",
  );
  check(
    rows[0]?.delivery_status === "SENT" && rows[0]?.delivery_attempts === 1,
    "row SENT after 1 attempt",
  );
  check(
    rows[0]?.lease_owner === null && rows[0]?.provider_message_id === "fake-msg-1",
    "lease released, Gmail ID stored",
  );

  await cron();
  check(google.sends.length === 1, "second cron run sends nothing");
  await stopWorker(worker);
  worker = undefined;

  worker = await startWorker(); // restart: new process, same local D1
  await cron();
  check(google.sends.length === 1, "after restart: still exactly one e-mail");

  google.mode = "unavailable";
  check((await post(event(2))).status === 201, "second event ingested");
  await cron();
  rows = sql(
    "SELECT delivery_status, delivery_attempts, last_error, next_attempt_at FROM notifications WHERE event_id = 'dsp-0123456789ab-c0ffee00-2'",
  );
  check(
    rows[0]?.delivery_status === "PENDING" &&
      rows[0]?.delivery_attempts === 1 &&
      rows[0]?.last_error === "gmail_503",
    "Gmail 503: still PENDING, attempt counted, error recorded",
  );
  check(typeof rows[0]?.next_attempt_at === "number", "retry scheduled (next_attempt_at set)");
  await cron();
  check(google.sends.length === 1, "not retried before the backoff expires");
} catch (e) {
  failures++;
  console.error(String(e));
} finally {
  if (worker) await stopWorker(worker);
  fake.close();
  rmSync(persist, { recursive: true, force: true });
}

console.log(
  failures === 0
    ? "delivery integration: all checks passed"
    : `delivery integration: ${failures} failed`,
);
process.exit(failures === 0 ? 0 : 1);
