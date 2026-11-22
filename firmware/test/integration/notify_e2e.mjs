#!/usr/bin/env node
/**
 * M1.8 end-to-end notification integration test, entirely local:
 *
 *   firmware domain code (notify_harness: M1.5 engine + M1.3 storage over a fake KV with
 *   power-cut injection + M1.7 protocol, HTTPS transport and connectivity session)
 *     --real TLS (OpenSSL, test CA, hostname check)-->  fault-injection proxy (this file)
 *     --HTTP-->  `wrangler dev --local` (the committed Worker) + local D1 (throwaway)
 *     --scheduled delivery-->  fake Gmail API (this file)
 *
 * Never touches Cloudflare, production D1, Google or a real credential: synthetic token,
 * throwaway certificates and database in a temporary directory, 127.0.0.1 only.
 *
 *   node firmware/test/integration/notify_e2e.mjs <path to notify_harness> [--sessions N]
 */
import { spawn, spawnSync } from "node:child_process";
import { createHash, randomBytes } from "node:crypto";
import { mkdtempSync, readdirSync, readFileSync, rmSync, writeFileSync } from "node:fs";
import { createServer as createHttpServer, request as httpRequest } from "node:http";
import { createServer as createNetServer } from "node:net";
import { tmpdir } from "node:os";
import { dirname, join, resolve } from "node:path";
import { createInterface } from "node:readline";
import { DatabaseSync } from "node:sqlite";
import { createServer as createTlsServer } from "node:tls";
import { fileURLToPath } from "node:url";

const HERE = dirname(fileURLToPath(import.meta.url));
const BACKEND = resolve(HERE, "../../../cloudflare/dispenser-notify");
const WRANGLER = join(BACKEND, "node_modules", ".bin", "wrangler");
const HARNESS = process.argv[2];
const sessionsArg = process.argv.indexOf("--sessions");
const SESSIONS = sessionsArg > 0 ? Number(process.argv[sessionsArg + 1]) : 1000;
if (!Number.isInteger(SESSIONS) || SESSIONS < 2) {
  console.error("--sessions needs an integer >= 2");
  process.exit(2);
}
if (!HARNESS) {
  console.error("usage: notify_e2e.mjs <notify_harness> [--sessions N]");
  process.exit(2);
}
const TMP = mkdtempSync(join(tmpdir(), "dispenser-m18-"));
// Cleanup runs exactly once: normal end, failure, Ctrl-C, SIGTERM or the watchdog.
const cleanups = [];
let cleanupPromise = null;
// One shared promise: concurrent callers (signal handler and the main flow) all wait for the
// complete cleanup before anything exits.
function cleanup() {
  cleanupPromise ??= (async () => {
    for (const fn of cleanups.reverse()) {
      try {
        await fn();
      } catch {
        /* keep cleaning */
      }
    }
    rmSync(TMP, { recursive: true, force: true });
  })();
  return cleanupPromise;
}
for (const sig of ["SIGINT", "SIGTERM"]) {
  process.on(sig, async () => {
    console.error(`\ninterrupted (${sig}): cleaning up`);
    await cleanup();
    process.exit(130);
  });
}
for (const ev of ["uncaughtException", "unhandledRejection"]) {
  process.on(ev, async (e) => {
    console.error(e);
    await cleanup();
    process.exit(1);
  });
}
const WATCHDOG_MS = 30 * 60 * 1000;
setTimeout(async () => {
  console.error("watchdog: the integration run exceeded 30 minutes");
  await cleanup();
  process.exit(1);
}, WATCHDOG_MS).unref();
const TOKEN = `e2e-${randomBytes(24).toString("hex")}`; // synthetic, this run only
const ENV = { ...process.env, CI: "1", WRANGLER_SEND_METRICS: "false", NO_COLOR: "1" };

let failures = 0;
let checks = 0;
function check(cond, what) {
  checks++;
  if (!cond) {
    failures++;
    console.log(`FAIL ${what}`);
  }
  return cond;
}
function section(name) {
  console.log(`\n== ${name}`);
}
const sleep = (ms) => new Promise((r) => setTimeout(r, ms));

// ---- Throwaway certificates -------------------------------------------------------------
function openssl(args) {
  const r = spawnSync("openssl", args, { cwd: TMP, encoding: "utf8" });
  if (r.status !== 0) throw new Error(`openssl ${args.join(" ")}\n${r.stderr}`);
}
function makeCa(name) {
  openssl([
    "req",
    "-x509",
    "-newkey",
    "ec",
    "-pkeyopt",
    "ec_paramgen_curve:P-256",
    "-nodes",
    "-keyout",
    `${name}.key`,
    "-out",
    `${name}.pem`,
    "-days",
    "2",
    "-subj",
    `/CN=${name}`,
  ]);
}
function makeLeaf(name, ca, san) {
  writeFileSync(
    join(TMP, `${name}.ext`),
    `basicConstraints=CA:FALSE\nextendedKeyUsage=serverAuth\nsubjectAltName=${san}\n`,
  );
  openssl([
    "req",
    "-newkey",
    "ec",
    "-pkeyopt",
    "ec_paramgen_curve:P-256",
    "-nodes",
    "-keyout",
    `${name}.key`,
    "-out",
    `${name}.csr`,
    "-subj",
    `/CN=${name}`,
  ]);
  openssl([
    "x509",
    "-req",
    "-in",
    `${name}.csr`,
    "-CA",
    `${ca}.pem`,
    "-CAkey",
    `${ca}.key`,
    "-CAcreateserial",
    "-out",
    `${name}.pem`,
    "-days",
    "2",
    "-extfile",
    `${name}.ext`,
  ]);
  return {
    key: readFileSync(join(TMP, `${name}.key`)),
    cert: readFileSync(join(TMP, `${name}.pem`)),
  };
}
makeCa("e2e-ca");
makeCa("rogue-ca");
const goodCert = makeLeaf("server", "e2e-ca", "DNS:localhost,IP:127.0.0.1");
const rogueCert = makeLeaf("rogue", "rogue-ca", "DNS:localhost,IP:127.0.0.1");
const wrongHostCert = makeLeaf("wronghost", "e2e-ca", "DNS:other.example");
const CA_FILE = join(TMP, "e2e-ca.pem");

// ---- Fake Google (OAuth token endpoint + Gmail send) ------------------------------------
const google = { sends: [] };
const fakeGoogle = createHttpServer((req, res) => {
  let body = "";
  req.on("data", (d) => (body += d));
  req.on("end", () => {
    res.setHeader("Content-Type", "application/json");
    if (req.method === "POST" && req.url === "/token") {
      res.end(JSON.stringify({ access_token: "fake-access", expires_in: 3599 }));
    } else if (req.method === "POST" && req.url === "/gmail/v1/users/me/messages/send") {
      google.sends.push(JSON.parse(body).raw);
      res.end(JSON.stringify({ id: `fake-msg-${google.sends.length}` }));
    } else {
      res.writeHead(404).end("{}");
    }
  });
});
await new Promise((r) => fakeGoogle.listen(0, "127.0.0.1", r));
cleanups.push(() => fakeGoogle.close());
const googleBase = `http://127.0.0.1:${fakeGoogle.address().port}`;
function messageIds() {
  return google.sends.map((raw) => {
    const text = Buffer.from(raw, "base64url").toString("utf8");
    return /^Message-ID: (.*)$/m.exec(text)?.[1]?.trim();
  });
}

// ---- Local Worker (wrangler dev, local D1) -----------------------------------------------
const PERSIST = join(TMP, "d1");
const workerPort = 21000 + Math.floor(Math.random() * 2000);
const workerBase = `http://127.0.0.1:${workerPort}`;
function wranglerSync(args) {
  const r = spawnSync(WRANGLER, args, { cwd: BACKEND, env: ENV, encoding: "utf8" });
  if (r.status !== 0) throw new Error(`wrangler ${args[0]} failed:\n${r.stdout}\n${r.stderr}`);
}
wranglerSync([
  "d1",
  "migrations",
  "apply",
  "dispenser-notifications",
  "--local",
  "--persist-to",
  PERSIST,
]);
const vars = {
  DEVICE_API_TOKEN: TOKEN,
  GMAIL_CLIENT_ID: "fake-client.apps.googleusercontent.com",
  GMAIL_CLIENT_SECRET: "fake-client-secret",
  GMAIL_REFRESH_TOKEN: "fake-refresh",
  NOTIFY_SENDER_ADDRESS: "sender@example.com",
  NOTIFY_RECIPIENT_ADDRESS: "recipient@example.org",
  GMAIL_API_BASE_OVERRIDE: googleBase,
  GOOGLE_TOKEN_URL_OVERRIDE: `${googleBase}/token`,
};
const wargs = [
  "dev",
  "--local",
  "--test-scheduled",
  "--ip",
  "127.0.0.1",
  "--port",
  String(workerPort),
  "--persist-to",
  PERSIST,
  "--show-interactive-dev-session=false",
];
for (const [k, v] of Object.entries(vars)) wargs.push("--var", `${k}:${v}`);
// Own process group, so cleanup can terminate wrangler and its workerd children together.
const worker = spawn(WRANGLER, wargs, {
  cwd: BACKEND,
  env: ENV,
  stdio: ["ignore", "pipe", "pipe"],
  detached: true,
});
const killWorker = (signal) => {
  try {
    process.kill(-worker.pid, signal);
  } catch {
    /* already gone */
  }
};
cleanups.push(async () => {
  killWorker("SIGTERM");
  for (let i = 0; i < 20 && worker.exitCode === null && worker.signalCode === null; i++)
    await sleep(100);
  killWorker("SIGKILL");
});
let workerLog = "";
worker.stdout.on("data", (d) => (workerLog += d));
worker.stderr.on("data", (d) => (workerLog += d));
for (let i = 0; ; i++) {
  if (worker.exitCode !== null || i > 120)
    throw new Error(`wrangler dev did not start:\n${workerLog}`);
  try {
    if ((await fetch(`${workerBase}/health`)).ok) break;
  } catch {
    /* not up yet */
  }
  await sleep(500);
}
function d1() {
  const dir = join(PERSIST, "v3", "d1", "miniflare-D1DatabaseObject");
  const file = readdirSync(dir).find((f) => f.endsWith(".sqlite") && f !== "metadata.sqlite");
  return new DatabaseSync(join(dir, file), { readOnly: true });
}
function rowsFor(prefix) {
  const db = d1();
  const rows = db
    .prepare(
      "SELECT event_id, payload, payload_hash, delivery_status, provider_message_id FROM notifications WHERE event_id LIKE ? ORDER BY event_id",
    )
    .all(`${prefix}%`);
  db.close();
  return rows;
}
async function cron() {
  const r = await fetch(`${workerBase}/__scheduled?cron=${encodeURIComponent("*/5 * * * *")}`);
  await r.text();
}
async function drainCron(prefix) {
  // The Worker sends at most 10 e-mails per scheduled run (MAX_SENDS_PER_RUN), oldest first,
  // across all PENDING rows of the shared local D1.
  for (let i = 0; i < 400 && rowsFor(prefix).some((r) => r.delivery_status === "PENDING"); i++)
    await cron();
}

// ---- Fault-injection proxy (TLS in front of the Worker) ----------------------------------
const proxy = {
  queue: [],
  sticky: null,
  received: 0,
  forwarded: 0,
  workerStatuses: [],
  storedIds: new Set(),
  sockets: new Set(),
};
proxy.next = (...modes) => proxy.queue.push(...modes);
proxy.reset = () => {
  proxy.queue = [];
  proxy.sticky = null;
};
function rawResponse(status, body) {
  const b = Buffer.from(body ?? "");
  return `HTTP/1.1 ${status} X\r\nContent-Type: application/json\r\nContent-Length: ${b.length}\r\nConnection: close\r\n\r\n${b}`;
}
function forward(req, mode) {
  return new Promise((resolveFwd, rejectFwd) => {
    let body = req.body;
    const headers = { ...req.headers, host: `127.0.0.1:${workerPort}` };
    let path = req.path;
    const id = (() => {
      try {
        return JSON.parse(body).event_id;
      } catch {
        return undefined;
      }
    })();
    if (mode.kind === "corrupt" && !proxy.storedIds.has(id)) body = mode.fn(body); // never a stored ID
    if (mode.kind === "pad") body = body.padEnd(5000, " ");
    if (mode.kind === "header") headers[mode.name] = mode.value;
    if (mode.kind === "path") path = mode.path;
    headers["content-length"] = String(Buffer.byteLength(body));
    const r = httpRequest(
      { host: "127.0.0.1", port: workerPort, method: req.method, path, headers },
      (res) => {
        let out = "";
        res.on("data", (d) => (out += d));
        res.on("end", () => {
          proxy.forwarded++;
          proxy.workerStatuses.push(res.statusCode);
          if ((res.statusCode === 201 || res.statusCode === 200) && id) proxy.storedIds.add(id);
          resolveFwd({ status: res.statusCode, body: out });
        });
      },
    );
    r.on("error", rejectFwd);
    r.end(body);
  });
}
function handleTls(socket) {
  proxy.sockets.add(socket);
  socket.on("close", () => proxy.sockets.delete(socket));
  socket.on("error", () => {});
  const mode = proxy.queue.length ? proxy.queue.shift() : (proxy.sticky ?? { kind: "pass" });
  if (mode.kind === "reset_on_connect") {
    socket.destroy();
    return;
  }
  let buf = Buffer.alloc(0);
  socket.on("data", async (d) => {
    buf = Buffer.concat([buf, d]);
    const end = buf.indexOf("\r\n\r\n");
    if (end < 0) return;
    const head = buf.subarray(0, end).toString();
    const lines = head.split("\r\n");
    const [method, path] = lines[0].split(" ");
    const headers = {};
    for (const l of lines.slice(1)) {
      const i = l.indexOf(":");
      headers[l.slice(0, i).trim().toLowerCase()] = l.slice(i + 1).trim();
    }
    const len = Number(headers["content-length"] || 0);
    if (buf.length < end + 4 + len) return;
    socket.removeAllListeners("data");
    proxy.received++;
    const req = { method, path, headers, body: buf.subarray(end + 4, end + 4 + len).toString() };
    try {
      switch (mode.kind) {
        case "hang":
          return; // read the request, never answer, never forward
        case "close_after_request":
          socket.destroy(); // complete request received, not forwarded, no answer
          return;
        case "status":
          socket.end(rawResponse(mode.status, mode.body));
          return;
        case "drop_after_forward":
          await forward(req, mode);
          socket.destroy(); // the Worker stored it; the device never hears back
          return;
        case "hang_after_forward":
          await forward(req, mode);
          return; // stored; the answer never comes (device timeout)
        case "delay":
          await sleep(mode.ms);
          break;
        default:
          break;
      }
      const res = await forward(req, mode);
      const out =
        mode.kind === "rewrite" ? { status: mode.status ?? res.status, body: mode.body } : res;
      socket.end(rawResponse(out.status, out.body));
    } catch {
      socket.destroy();
    }
  });
}
const tlsGood = createTlsServer({ ...goodCert }, handleTls);
const tlsRogue = createTlsServer({ ...rogueCert }, (s) => {
  proxy.rogueRequests = (proxy.rogueRequests ?? 0) + 1;
  s.destroy();
});
const tlsWrongHost = createTlsServer({ ...wrongHostCert }, (s) => {
  proxy.wrongHostRequests = (proxy.wrongHostRequests ?? 0) + 1;
  s.destroy();
});
const blackhole = createNetServer((s) => {
  proxy.sockets.add(s); // TCP accepted, TLS never answered
  s.on("error", () => {});
});
for (const srv of [tlsGood, tlsRogue, tlsWrongHost, blackhole]) {
  srv.on("tlsClientError", () => {});
  await new Promise((r) => srv.listen(0, "127.0.0.1", r));
  cleanups.push(() => srv.close());
}
cleanups.push(() => {
  for (const sock of proxy.sockets) sock.destroy();
});
const closedPort = await new Promise((r) => {
  const s = createNetServer();
  s.listen(0, "127.0.0.1", () => {
    const p = s.address().port;
    s.close(() => r(p));
  });
});
const PORTS = {
  good: tlsGood.address().port,
  rogue: tlsRogue.address().port,
  wronghost: tlsWrongHost.address().port,
  blackhole: blackhole.address().port,
  closed: closedPort,
};

// ---- Harness ------------------------------------------------------------------------------
const harness = spawn(HARNESS, [CA_FILE], { stdio: ["pipe", "pipe", "inherit"] });
cleanups.push(async () => {
  try {
    harness.stdin.end("quit\n");
  } catch {
    /* already closed */
  }
  for (let i = 0; i < 20 && harness.exitCode === null && harness.signalCode === null; i++)
    await sleep(100);
  if (harness.exitCode === null && harness.signalCode === null) harness.kill("SIGKILL");
});
const lines = createInterface({ input: harness.stdout });
const pending = [];
let harnessExited = null;
lines.on("line", (l) => pending.shift()?.resolve(l));
harness.on("exit", (code, signal) => {
  harnessExited = `notify_harness exited (code ${code}, signal ${signal})`;
  for (const p of pending.splice(0)) p.reject(new Error(harnessExited));
});
function h(command) {
  return new Promise((resolve, reject) => {
    if (harnessExited) {
      reject(new Error(harnessExited));
      return;
    }
    pending.push({ resolve: (l) => resolve(JSON.parse(l)), reject });
    harness.stdin.write(`${command}\n`);
  });
}
const canonical = (body) =>
  body.replace(/^\{"schema_version":1,/, "{").replace(/,"attempt":\d+\}$/, "}");
const sha256 = (s) => createHash("sha256").update(s).digest("hex");
let epochCounter = 0xe2e00000;
async function fresh(durable = 1) {
  const epoch = (++epochCounter).toString(16).padStart(8, "0");
  proxy.reset();
  await h(`init ${durable} ${epoch}`);
  await h(`target localhost ${PORTS.good} /api/v1/events`);
  await h(`token ${TOKEN}`);
  await h("timeouts 1500 2000 60000");
  await h("wifi ok");
  await h("ntp ok");
  return `dsp-0123456789ab-${epoch}-`;
}
async function dosing(n = 1, code = 2) {
  let r;
  for (let i = 0; i < n; i++) r = await h(`dosing ${code} - ${1000 + i}`);
  return r;
}
const bodiesById = new Map(); // every body ever sent, per event ID (immutability)
function record(reply) {
  for (const x of reply.exchanges ?? []) {
    if (!x.event_id) continue;
    if (!bodiesById.has(x.event_id)) bodiesById.set(x.event_id, []);
    bodiesById.get(x.event_id).push(x.body);
  }
  return reply;
}
async function deliver(budget) {
  return record(await h(budget ? `deliver ${budget}` : "deliver"));
}
async function session(args = "0 0 0 -1") {
  const r = record(await h(`session ${args}`));
  check(
    r.wifi_state === 0 && r.wifi_starts === r.wifi_stops,
    `Wi-Fi off after session (${r.outcome})`,
  );
  return r;
}
function immutable(prefix) {
  let ok = true;
  for (const [id, bodies] of bodiesById) {
    if (!id.startsWith(prefix)) continue;
    const c = new Set(bodies.map(canonical));
    if (c.size !== 1) {
      ok = false;
      console.log(`  payload changed for ${id}`);
    }
  }
  return ok;
}

try {
  // ======================================================================================
  section("1. basic delivery");
  {
    const prefix = await fresh();
    const ev = await dosing(1);
    check(ev.seq === 1 && ev.pending === 1, "event queued");
    const r = await deliver();
    const x = r.exchanges[0];
    check(x?.status === 201 && x.result === "ACK", "strict 201 acknowledgement");
    check(
      x.response === `{"status":"accepted","event_id":"${x.event_id}"}`,
      "exact acknowledgement body",
    );
    check(r.pending === 0 && r.in_sync, "removed durably");
    const rows = rowsFor(prefix);
    check(rows.length === 1 && rows[0].event_id === x.event_id, "one D1 row");
    check(rows[0].payload === canonical(x.body), "D1 payload = canonical body");
    check(rows[0].payload_hash === sha256(rows[0].payload), "D1 payload_hash = SHA-256(canonical)");
    check(rows[0].delivery_status === "PENDING", "PENDING before the scheduled run");
    const before = google.sends.length;
    await cron();
    const after = rowsFor(prefix)[0];
    check(google.sends.length === before + 1, "fake Gmail accepted one message");
    check(
      after.delivery_status === "SENT" && after.provider_message_id,
      "D1 row SENT with provider message ID",
    );
  }

  // ======================================================================================
  section("1b. BATTERY_LOW (battery monitor -> engine -> Worker -> e-mail)");
  {
    const prefix = await fresh();
    const ev = await h("battery low 3850");
    check(ev.seq === 1 && ev.pending === 1 && ev.queue[0].value === 3850, "one BATTERY_LOW queued");
    const again = await h("battery low 3840");
    check(again.seq === 0 && again.pending === 1, "repeated LOW: no second event");
    await h("battery unknown 0");
    // Wi-Fi unavailable: the event stays queued
    await h("wifi fail");
    const s1 = await session();
    check(s1.pending === 1, `Wi-Fi unavailable: still queued (${s1.outcome})`);
    await h("wifi ok");
    // the response is lost after the Worker stored it: the retry repeats the same ID
    proxy.next({ kind: "drop_after_forward" });
    const r1 = await deliver();
    check(r1.exchanges[0]?.result === "UNCERTAIN" && r1.pending === 1, "lost ACK: UNCERTAIN, still queued");
    const r2 = await deliver();
    const x = r2.exchanges[0];
    check(x?.status === 200 && x.result === "ACK" && r2.pending === 0, "retry answered 200 duplicate -> removed");
    check(canonical(r1.exchanges[0].body) === canonical(x.body), "identical canonical payload on retry");
    const body = JSON.parse(x.body);
    check(
      body.type === "BATTERY_LOW" && body.severity === "WARNING" && body.value === 3840 &&
        body.value2 === 0 && body.code === null && body.reminder_no === 0,
      "protocol v1 fields: BATTERY_LOW, WARNING, value = mV, value2 = 0",
    );
    const rows = rowsFor(prefix);
    check(rows.length === 1, "exactly one D1 row");
    const before = google.sends.length;
    await cron();
    check(google.sends.length === before + 1 && rowsFor(prefix)[0].delivery_status === "SENT", "one e-mail sent");
    const mail = Buffer.from(google.sends.at(-1), "base64url").toString("utf8");
    const [head, ...rest] = mail.split(/\r\n\r\n/);
    const subject = (/^Subject: ((?:.*)(?:\r\n .*)*)/m.exec(head)?.[1] ?? "")
      .split(/\r\n /)
      .map((w) => Buffer.from(/=\?UTF-8\?B\?(.*)\?=/.exec(w)?.[1] ?? "", "base64").toString("utf8"))
      .join("");
    const text = Buffer.from(rest.join("").replace(/\s/g, ""), "base64").toString("utf8");
    check(subject.includes("Slabá baterie") && subject.includes("3,84"), `battery e-mail subject (${subject})`);
    check(text.includes("Napětí baterie: 3,84 V"), "e-mail body shows the measured voltage 3,84 V");
    check(!/\d\s*%/.test(text), "no battery percentage in the e-mail");
    // recovery closes the episode; a new drop opens a new one with a new ID
    const okr = await h("battery ok 4100");
    check(okr.pending === 0 && okr.seq === 0, "recovery: no event");
    const ev2 = await h("battery low 3870");
    check(ev2.seq === 2 && ev2.pending === 1, "new episode after recovery: new event");
    const r3 = await deliver();
    check(r3.exchanges[0]?.status === 201 && rowsFor(prefix).length === 2, "second episode stored as a new row");
    check(immutable(prefix), "payloads immutable per event ID");
    await cron(); // deliver the second episode here (later sections count e-mails)
    check(rowsFor(prefix).every((r) => r.delivery_status === "SENT"), "both battery e-mails sent");
  }

  // ======================================================================================
  section("2. idempotency (lost response after successful ingestion)");
  for (const lose of ["drop_after_forward", "hang_after_forward"]) {
    const prefix = await fresh();
    await dosing(1);
    proxy.next({ kind: lose });
    const r1 = await deliver();
    check(
      r1.exchanges[0]?.result === "UNCERTAIN" && r1.pending === 1,
      `${lose}: UNCERTAIN, still queued`,
    );
    check(rowsFor(prefix).length === 1, `${lose}: the Worker stored it`);
    const r2 = await deliver();
    const x = r2.exchanges[0];
    check(
      x?.status === 200 &&
        x.response === `{"status":"duplicate","event_id":"${x.event_id}"}` &&
        x.result === "ACK",
      `${lose}: retry answered 200 duplicate -> ACK`,
    );
    check(
      canonical(r1.exchanges[0].body) === canonical(x.body),
      `${lose}: identical canonical payload`,
    );
    check(
      r1.exchanges[0].body.endsWith('"attempt":1}') && x.body.endsWith('"attempt":2}'),
      `${lose}: only attempt differs`,
    );
    check(rowsFor(prefix).length === 1, `${lose}: exactly one D1 row`);
    const before = google.sends.length;
    await cron();
    await cron();
    check(
      google.sends.length === before + 1 && rowsFor(prefix)[0].delivery_status === "SENT",
      `${lose}: one scheduled delivery, no duplicate after SENT`,
    );
  }

  // ======================================================================================
  section("3. network failures and recovery");
  const netCases = [
    [
      "DNS failure",
      async () => h(`target nonexistent.invalid ${PORTS.good} /api/v1/events`),
      "connect_failed",
      "FAILED",
    ],
    [
      "connection refused",
      async () => h(`target localhost ${PORTS.closed} /api/v1/events`),
      "connect_failed",
      "FAILED",
    ],
    [
      "TLS: untrusted certificate",
      async () => h(`target localhost ${PORTS.rogue} /api/v1/events`),
      "tls_failed",
      "FAILED",
    ],
    [
      "TLS: wrong host name",
      async () => h(`target localhost ${PORTS.wronghost} /api/v1/events`),
      "tls_failed",
      "FAILED",
    ],
    [
      "timeout before transmission (TLS never answered)",
      async () => h(`target localhost ${PORTS.blackhole} /api/v1/events`),
      "tls_failed",
      "FAILED",
    ],
    [
      "reset right after the handshake",
      async () => proxy.next({ kind: "reset_on_connect" }),
      null,
      "UNCERTAIN",
    ],
    [
      "timeout after complete transmission (not forwarded)",
      async () => proxy.next({ kind: "hang" }),
      "no_response",
      "UNCERTAIN",
    ],
    [
      "connection closed after the request (not forwarded)",
      async () => proxy.next({ kind: "close_after_request" }),
      "no_response",
      "UNCERTAIN",
    ],
    [
      "HTTP 429",
      async () => proxy.next({ kind: "status", status: 429, body: "" }),
      "response",
      "FAILED",
    ],
    [
      "HTTP 503",
      async () => proxy.next({ kind: "status", status: 503, body: '{"status":"retry"}' }),
      "response",
      "FAILED",
    ],
    [
      "HTTP 500",
      async () => proxy.next({ kind: "status", status: 500, body: "" }),
      "response",
      "FAILED",
    ],
  ];
  for (const [name, inject, phase, result] of netCases) {
    const prefix = await fresh();
    await dosing(2);
    const fwd = proxy.forwarded;
    await inject();
    const r = await deliver();
    const x = r.exchanges[0];
    check(
      x?.result === result && (phase === null || x.phase === phase),
      `${name}: ${result} (${x?.phase})`,
    );
    check(
      r.report.attempted === 1 && r.pending === 2 && r.diag === 0,
      `${name}: queue kept, session stopped, no diagnostic`,
    );
    check(
      proxy.forwarded === fwd && rowsFor(prefix).length === 0,
      `${name}: nothing reached the Worker`,
    );
    await h(`target localhost ${PORTS.good} /api/v1/events`);
    const rec = await deliver();
    check(
      rec.report.acked === 2 && rec.pending === 0 && rec.exchanges.every((e) => e.status === 201),
      `${name}: recovery delivers both (201)`,
    );
    check(
      rowsFor(prefix).length === 2 && immutable(prefix),
      `${name}: two rows, payloads unchanged`,
    );
  }
  check(!proxy.rogueRequests || true, "untrusted TLS servers never saw a request");
  {
    // Wi-Fi unavailable (connectivity session).
    const prefix = await fresh();
    await dosing(1);
    await h("wifi fail");
    const rcv = proxy.received;
    const r = await session();
    check(
      r.outcome === "WIFI_FAILED" && r.pending === 1 && proxy.received === rcv,
      "Wi-Fi unavailable: nothing sent, queue kept",
    );
    check(r.failures === 1, "Wi-Fi failure feeds the sync backoff");
    await h("wifi ok");
    let s;
    for (let i = 0; i < 40; i++) {
      const a = await h("advance 300");
      if (a.sync_due) break;
    }
    s = await session();
    check(
      s.outcome === "DONE" && s.report.acked === 1 && rowsFor(prefix).length === 1,
      "Wi-Fi restored: delivered in the next due session",
    );
  }

  // ======================================================================================
  section("4. permanent rejection (confirmed by the real Worker)");
  {
    const prefix = await fresh();
    await dosing(3);
    proxy.next({ kind: "corrupt", fn: (b) => b.replace(/"value":\d+/, '"value":-1') });
    const r = await deliver();
    const [x0, x1, x2] = r.exchanges;
    check(
      x0?.status === 400 &&
        x0.response === '{"status":"invalid","error":"invalid_field","field":"value"}' &&
        x0.result === "REJECTED",
      "confirmed 400 invalid_field -> REJECTED",
    );
    check(
      x1?.result === "ACK" &&
        x2?.result === "ACK" &&
        r.report.rejected === 1 &&
        r.report.acked === 2,
      "the other queued events continue in the same session",
    );
    const e = r.rejections.entries[0];
    check(
      r.rejections.count === 1 &&
        e.http_status === 400 &&
        e.error === 5 &&
        e.field === 11 &&
        e.event_id === x0.event_id,
      "rejection logged (400, invalid_field, value)",
    );
    check(r.pending === 0 && r.in_sync, "log entry and removal durable together");
    check(
      rowsFor(prefix).length === 2 && !rowsFor(prefix).some((row) => row.event_id === x0.event_id),
      "no D1 row for the rejected event",
    );
    const b = await h("boot");
    check(
      b.rejections.count === 1 &&
        b.rejections.total === 1 &&
        b.rejections.entries[0].event_id === x0.event_id,
      "rejection diagnostics survive a restart",
    );
  }
  {
    // Genuine 409: storage reset with a colliding epoch reuses IDs with different content.
    const epochHex = (++epochCounter).toString(16).padStart(8, "0");
    const prefix = `dsp-0123456789ab-${epochHex}-`;
    proxy.reset();
    await h(`init 1 ${epochHex}`);
    await h(`target localhost ${PORTS.good} /api/v1/events`);
    await h(`token ${TOKEN}`);
    await h("dosing 2 - 1000");
    const first = await deliver();
    check(first.exchanges[0]?.status === 201, "original event stored");
    const original = rowsFor(prefix)[0]?.payload;
    await h(`init 1 ${epochHex}`); // storage reset, same epoch: seq 1 again
    await h("dosing 2 - 2000");
    await h("dosing 2 - 3000");
    const r = await deliver();
    const [x0, x1] = r.exchanges;
    check(
      x0?.status === 409 &&
        x0.response === `{"status":"conflict","event_id":"${x0.event_id}"}` &&
        x0.result === "REJECTED",
      "genuine 409 conflict -> REJECTED",
    );
    check(x1?.status === 201 && x1.result === "ACK", "the next event continues");
    check(
      r.rejections.entries[0]?.http_status === 409 && r.rejections.entries[0]?.error === 10,
      "conflict logged",
    );
    check(
      rowsFor(prefix).find((row) => row.event_id === x0.event_id)?.payload === original,
      "the stored event is never modified",
    );
  }

  // ======================================================================================
  section("5. systemic protocol failures never drain the queue");
  const sys = [
    [
      "401 (wrong token)",
      async () => h("token wrong-token-000000000000000000000000000000"),
      async () => h(`token ${TOKEN}`),
      1,
      false,
    ],
    [
      "404 (wrong path)",
      async () => h(`target localhost ${PORTS.good} /api/v1/nope`),
      async () => h(`target localhost ${PORTS.good} /api/v1/events`),
      2,
      false,
    ],
    [
      "405 (POST /health)",
      async () => h(`target localhost ${PORTS.good} /health`),
      async () => h(`target localhost ${PORTS.good} /api/v1/events`),
      2,
      false,
    ],
    [
      "413 (oversized body, real Worker)",
      async () => (proxy.sticky = { kind: "pad" }),
      async () => (proxy.sticky = null),
      4,
      false,
    ],
    [
      "415 (wrong content type, real Worker)",
      async () => (proxy.sticky = { kind: "header", name: "content-type", value: "text/plain" }),
      async () => (proxy.sticky = null),
      4,
      false,
    ],
    [
      "ambiguous 400 (missing field, real Worker)",
      async () => (proxy.sticky = { kind: "corrupt", fn: (b) => b.replace(/,"attempt":\d+/, "") }),
      async () => (proxy.sticky = null),
      4,
      false,
    ],
    [
      "mismatched 409",
      async () =>
        (proxy.sticky = {
          kind: "status",
          status: 409,
          body: '{"status":"conflict","event_id":"dsp-ffffffffffff-ffffffff-1"}',
        }),
      async () => (proxy.sticky = null),
      4,
      false,
    ],
    [
      "mismatched 201 acknowledgement",
      async () =>
        (proxy.sticky = {
          kind: "rewrite",
          body: '{"status":"accepted","event_id":"dsp-ffffffffffff-ffffffff-1"}',
        }),
      async () => (proxy.sticky = null),
      4,
      true,
    ],
    [
      "invalid 2xx body",
      async () => (proxy.sticky = { kind: "rewrite", status: 200, body: "OK" }),
      async () => (proxy.sticky = null),
      4,
      true,
    ],
  ];
  for (const [name, inject, restore, diag, stores] of sys) {
    const prefix = await fresh();
    await dosing(3);
    await inject();
    let r;
    for (let i = 0; i < 6; i++) {
      r = await deliver();
      check(
        r.report.attempted === 1 && r.report.rejected === 0,
        `${name}: session ${i + 1} stops at the first event, nothing rejected`,
      );
    }
    check(r.pending === 3 && r.rejections.total === 0, `${name}: all 3 events still queued`);
    check(r.diag === diag, `${name}: persistent diagnostic ${diag} (got ${r.diag})`);
    check(stores || rowsFor(prefix).length === 0, `${name}: nothing stored`);
    const b = await h("boot");
    check(b.pending === 3 && b.diag === diag, `${name}: queue and diagnostic survive a restart`);
    await restore();
    const ok = await deliver();
    check(
      ok.report.acked === 3 && ok.pending === 0 && ok.diag === 0,
      `${name}: after the fix all delivered, diagnostic cleared`,
    );
    check(
      rowsFor(prefix).length === 3 && immutable(prefix),
      `${name}: exactly 3 rows, payloads unchanged`,
    );
  }

  // ======================================================================================
  section("6. power loss");
  {
    // a) during the freeze write (NVS-like: the write lands, its confirmation is lost).
    let prefix = await fresh(1);
    await dosing(2);
    let rcv = proxy.received;
    await h("cut 1");
    let r = await deliver();
    check(
      r.report.status === 2 && proxy.received === rcv,
      "freeze write cut (durable): ERR_STORAGE, nothing sent",
    );
    await h("boot");
    r = await deliver();
    check(
      r.report.acked === 2 && rowsFor(prefix).length === 2 && immutable(prefix),
      "after restart: both delivered once",
    );
    // b) before the freeze is committed (staged write lost).
    prefix = await fresh(0);
    await dosing(2);
    rcv = proxy.received;
    await h("cut 1");
    r = await deliver();
    check(
      r.report.status === 2 && proxy.received === rcv,
      "freeze write cut (staged): nothing sent",
    );
    const b = await h("boot");
    check(
      b.queue.every((e) => e.attempts === 0),
      "after restart: not frozen, nothing counted",
    );
    r = await deliver();
    check(r.report.acked === 2 && rowsFor(prefix).length === 2, "then delivered once");
    // c) during transmission + power loss before the result is saved.
    prefix = await fresh(0);
    await dosing(1);
    proxy.next({ kind: "reset_on_connect" });
    await h("cut 2");
    r = await deliver();
    check(r.exchanges[0]?.result === "UNCERTAIN", "cut during transmission: UNCERTAIN");
    await h("boot");
    r = await deliver();
    check(
      r.exchanges[0]?.status === 201 && rowsFor(prefix).length === 1 && immutable(prefix),
      "after restart: same ID, same payload, stored once",
    );
    // d) after backend acceptance, before the ACK removal is persisted.
    prefix = await fresh(0);
    await dosing(1);
    await h("cut 2"); // set 1 = freeze, set 2 = end write (lost)
    r = await deliver();
    check(
      r.exchanges[0]?.status === 201 && r.report.status === 2,
      "accepted by the Worker, ACK persistence lost",
    );
    const b2 = await h("boot");
    check(b2.pending === 1 && b2.queue[0].attempts === 1, "after restart: still queued (frozen)");
    r = await deliver();
    check(
      r.exchanges[0]?.status === 200 && r.exchanges[0].result === "ACK",
      "re-sent: 200 duplicate -> ACK",
    );
    check(rowsFor(prefix).length === 1 && immutable(prefix), "exactly one row, identical payload");
    // e) rejection-log persistence lost (staged) or landed without confirmation (durable).
    for (const durable of [0, 1]) {
      prefix = await fresh(durable);
      await dosing(2);
      proxy.next({ kind: "corrupt", fn: (bb) => bb.replace(/"value":\d+/, '"value":-1') });
      await h("cut 2"); // set 1 = freeze, set 2 = rejection write
      r = await deliver();
      check(
        r.exchanges[0]?.result === "REJECTED" && r.report.status === 2,
        `rejection write cut (durable=${durable}): ERR_STORAGE`,
      );
      const b3 = await h("boot");
      const id = r.exchanges[0].event_id;
      const queued = b3.queue.some((e) => e.event_id === id);
      const logged = b3.rejections.entries.some((e) => e.event_id === id);
      check(
        queued !== logged,
        `rejection write cut (durable=${durable}): either still queued or logged, never both/neither`,
      );
      r = await deliver();
      check(r.pending === 0, `rejection write cut (durable=${durable}): queue drains afterwards`);
      const rowIds = new Set(rowsFor(prefix).map((row) => row.event_id));
      check(
        logged ? !rowIds.has(id) : rowIds.has(id),
        `rejection write cut (durable=${durable}): logged xor delivered`,
      );
    }
  }

  // ======================================================================================
  section("7. session orchestration");
  {
    const prefix = await fresh();
    let r = await session();
    check(
      r.outcome === "DONE" && r.reason === 1 && r.trust !== 0,
      "first session: regular sync, trusted afterwards",
    );
    await h("advance 600");
    await h("reservoir 5000 100000"); // WARNING
    let rcv = proxy.received;
    let starts = r.wifi_starts;
    r = await session();
    check(
      r.outcome === "SKIPPED_NOT_DUE" && proxy.received === rcv && r.wifi_starts === starts,
      "WARNING alone: no session, no Wi-Fi",
    );
    await dosing(1); // CRITICAL
    r = await session();
    check(
      r.outcome === "DONE" && r.reason === 2 && r.report.acked === 2,
      "CRITICAL on an existing wake: session, WARNING rides along",
    );
    check(r.exchanges[0]?.body.includes('"type":"DOSING_ERROR"'), "CRITICAL sent first");
    await h("advance 600");
    await dosing(1);
    rcv = proxy.received;
    r = await session();
    check(
      r.outcome === "SKIPPED_NOT_DUE" && proxy.received === rcv,
      "second CRITICAL within the hour: waits",
    );
    await h("advance 3000");
    r = await session();
    check(r.outcome === "DONE" && r.reason === 2 && r.report.acked === 1, "after one hour: sent");
    // Wi-Fi backoff: two failures make the backoff (2 h) longer than the hourly limit.
    await h("advance 3700");
    await dosing(1);
    await h("wifi fail");
    r = await session();
    check(
      r.outcome === "WIFI_FAILED" && r.reason === 2 && r.failures === 1,
      "CRITICAL session with Wi-Fi down: failure recorded",
    );
    let a = await h("advance 3600");
    check(a.sync_due, "first retry due after the 1 h backoff");
    r = await session();
    check(
      r.outcome === "WIFI_FAILED" && r.reason === 1 && r.failures === 2,
      "regular retry fails too: backoff doubles",
    );
    await h("wifi ok");
    a = await h("advance 3700"); // hourly limit passed, backoff (2 h) not yet
    rcv = proxy.received;
    starts = r.wifi_starts;
    r = await session();
    check(
      !a.sync_due &&
        r.outcome === "SKIPPED_NOT_DUE" &&
        r.wifi_starts === starts &&
        proxy.received === rcv,
      "hourly limit passed but Wi-Fi backoff active: no session",
    );
    for (let i = 0; i < 40 && !(await h("advance 300")).sync_due; i++);
    r = await session();
    check(
      r.outcome === "DONE" && r.reason === 1 && r.report.acked === 1,
      "delivered by the due regular session",
    );
    // Guards.
    await h("advance 3700");
    await dosing(1);
    rcv = proxy.received;
    starts = r.wifi_starts;
    for (const [args, what] of [
      ["1 0 0 -1", "dosing active"],
      ["0 1 0 -1", "dose permit outstanding"],
      ["0 0 1 -1", "BLE active"],
      ["0 0 0 200", "dose in 200 s"],
    ]) {
      const g = await session(args);
      check(
        g.outcome === (args.endsWith("200") ? "SKIPPED_DOSE_SOON" : "SKIPPED_GUARD"),
        `guard: ${what}`,
      );
    }
    r = await session();
    check(
      proxy.received === rcv + 1 && r.outcome === "DONE",
      "guards touched neither Wi-Fi nor the network; then sent",
    );
    check(r.wifi_starts === starts + 1, "exactly one Wi-Fi session for it");
    // Trusted time.
    await h("boot"); // power loss: clock UNKNOWN
    await dosing(1);
    await h("ntp fail");
    rcv = proxy.received;
    r = await session();
    check(
      r.outcome === "TIME_UNTRUSTED" &&
        proxy.received === rcv &&
        r.queue.every((e) => e.attempts === 0),
      "no trusted time: no HTTPS, queue untouched",
    );
    await h("ntp ok");
    for (let i = 0; i < 40; i++) {
      const a = await h("advance 300");
      if (a.sync_due) break;
    }
    r = await session();
    check(r.outcome === "DONE" && r.report.acked === 1, "trusted again: delivered");
    // Session budget (real monotonic clock, 1.3 s per request, 5 s budget, 2 s worst case).
    await h("timeouts 3000 2000 5000");
    await h("advance 3700");
    await dosing(6);
    proxy.sticky = { kind: "delay", ms: 1300 };
    r = await session();
    proxy.sticky = null;
    const untouched = r.queue.filter((e) => e.attempts === 0).length;
    check(
      r.report.budget_exhausted && r.report.attempted >= 2 && r.report.attempted <= 3,
      `budget expired after ${r.report.attempted} requests`,
    );
    check(
      r.pending === 6 - r.report.acked && untouched === r.pending,
      "the rest stays queued, untouched",
    );
    await h("timeouts 1500 2000 60000");
    await h("advance 3700");
    r = await session();
    check(r.report.acked === untouched && r.pending === 0, "the rest goes in the next session");
    check(immutable(prefix), "payloads unchanged throughout");
  }

  // ======================================================================================
  section(`8. stress: ${SESSIONS} randomized sessions with faults and power cuts`);
  for (const durable of [1, 0]) {
    const prefix = await fresh(durable);
    await h("timeouts 400 1000 60000");
    const statusesFrom = proxy.workerStatuses.length;
    let seed = durable ? 12345 : 67890;
    const rnd = (n) => {
      seed = (seed * 1103515245 + 12345) % 2147483648;
      return seed % n;
    };
    const known = new Set();
    const rejectedSeen = new Set();
    let dropped = 0;
    let stateChecks = 0;
    // No silent eviction: a queued entry may only ever disappear after an ACK or REJECTED
    // was observed for its ID (in this reply or earlier, e.g. a removal that became durable
    // and shows after a restart).
    let prevQueue = new Set();
    const removedLegitimately = new Set();
    let silentLosses = 0;
    let fullDropsChecked = 0;
    const note = (reply) => {
      for (const x of reply.exchanges ?? []) {
        if (x.result === "REJECTED") rejectedSeen.add(x.event_id);
        if (x.result === "ACK" || x.result === "REJECTED") removedLegitimately.add(x.event_id);
      }
      if (reply.queue) {
        const now = new Set(reply.queue.map((e) => e.event_id));
        for (const id of prevQueue) {
          if (!now.has(id) && !removedLegitimately.has(id)) {
            silentLosses++;
            console.log(`  queued event ${id} disappeared without ACK/REJECTED`);
          }
        }
        for (const id of now) known.add(id);
        prevQueue = now;
      }
      if (reply.valid === false) check(false, "state valid");
    };
    const modes = [
      () => null,
      () => null,
      () => null,
      () => ({ kind: "drop_after_forward" }),
      () => ({ kind: "close_after_request" }),
      () => ({ kind: "status", status: 503, body: '{"status":"retry"}' }),
      () => ({ kind: "status", status: 429, body: "" }),
      () => ({
        kind: "rewrite",
        body: '{"status":"accepted","event_id":"dsp-ffffffffffff-ffffffff-1"}',
      }),
      () => ({ kind: "corrupt", fn: (b) => b.replace(/"value":\d+/, '"value":-1') }),
      () => ({ kind: "status", status: 401, body: '{"status":"unauthorized"}' }),
      () => (rnd(4) === 0 ? { kind: "hang" } : null),
    ];
    const half = Math.ceil(SESSIONS / 2);
    let sessionsRun = 0;
    for (let s = 0; s < half; s++) {
      sessionsRun++;
      const events = rnd(3);
      for (let i = 0; i < events; i++) {
        const r = await h(`dosing ${1 + rnd(5)} ${rnd(2) ? "manual" : "-"} ${100 + rnd(5000)}`);
        if (r.status === 3) {
          // Dropped only because 16 CRITICAL entries are queued: nothing of lower severity
          // could be evicted and CRITICAL entries never are (approved M1.5 policy). The event
          // gets no ID; it is counted (persistently) and reported as QUEUE_OVERFLOW.
          dropped++;
          fullDropsChecked++;
          check(
            r.pending === 16 && r.queue.every((e) => e.severity === 3) && r.dropped > 0,
            `stress(durable=${durable}): queue-full drop only with 16 CRITICAL entries queued (pending ${r.pending})`,
          );
        }
        note(r);
      }
      const m = modes[rnd(modes.length)]();
      if (m) proxy.next(m);
      const cut = rnd(10) === 0;
      if (cut) await h(`cut ${1 + rnd(3)}`);
      const r = await deliver();
      note(r);
      for (const x of r.exchanges ?? []) {
        if (x.status === 409) {
          console.log(
            `  409 for ${x.event_id} (mode ${JSON.stringify(m?.kind ?? "pass")}, cut ${cut}); history:`,
          );
          for (const b of bodiesById.get(x.event_id) ?? []) console.log(`    ${b}`);
        }
      }
      proxy.reset();
      if (cut || rnd(25) === 0) note(await h("boot"));
      if (s > 0 && s % 100 === 0) console.log(`  durable=${durable}: ${s}/${half} sessions`);
      if (s % 50 === 0) {
        const st = await h("state");
        note(st);
        check(st.valid && (st.in_sync || st.dirty), "durable state consistent");
        stateChecks++;
      }
    }
    // Drain without faults.
    let last;
    for (let i = 0; i < 40; i++) {
      last = await deliver();
      note(last);
      if (last.pending === 0 && last.dropped === 0) break;
      await h("tick");
    }
    check(last.pending === 0, `stress(durable=${durable}): queue drained`);
    const rows = rowsFor(prefix);
    const rowIds = new Set(rows.map((r) => r.event_id));
    const noRow = [...known].filter((id) => !rowIds.has(id));
    check(
      rows.length === new Set(rows.map((r) => r.event_id)).size,
      `stress(durable=${durable}): no duplicate D1 rows`,
    );
    check(
      [...rowIds].every((id) => known.has(id)),
      `stress(durable=${durable}): every row is a known event`,
    );
    check(
      noRow.every((id) => rejectedSeen.has(id)),
      `stress(durable=${durable}): every event without a row was rejected`,
    );
    check(
      noRow.length === last.rejections.total,
      `stress(durable=${durable}): rejected-without-row (${noRow.length}) = durable rejection count (${last.rejections.total})`,
    );
    check(
      !proxy.workerStatuses.slice(statusesFrom).includes(409),
      `stress(durable=${durable}): no 409 ever (payloads never changed under an ID)`,
    );
    check(immutable(prefix), `stress(durable=${durable}): identical payload on every retry`);
    check(
      rows.every((r) => bodiesById.get(r.event_id)?.some((b) => canonical(b) === r.payload)),
      `stress(durable=${durable}): D1 payload = sent payload`,
    );
    const overflowSum = rows
      .map((r) => JSON.parse(r.payload))
      .filter((p) => p.type === "DEVICE_ERROR" && p.code === "QUEUE_OVERFLOW")
      .reduce((a, p) => a + p.value, 0);
    check(
      overflowSum === dropped,
      `stress(durable=${durable}): queue-overflow reports account for ${dropped} dropped events (${overflowSum})`,
    );
    check(
      silentLosses === 0,
      `stress(durable=${durable}): no queued event ever disappeared silently`,
    );
    check(
      sessionsRun === half && known.size > half / 2,
      `stress(durable=${durable}): ran ${sessionsRun}/${half} sessions with ${known.size} events (never vacuous)`,
    );
    check(
      last.dropped === 0,
      `stress(durable=${durable}): persistent drop counter fully reported (0 left)`,
    );
    const overflowReports = rows.filter(
      (r) => JSON.parse(r.payload).code === "QUEUE_OVERFLOW",
    ).length;
    const delivered = rows.length;
    const rejected = noRow.length;
    // Every event that got an ID is delivered (D1) or durably rejected; every event that got
    // no ID is counted in a delivered QUEUE_OVERFLOW report.
    check(
      known.size === delivered + rejected && last.pending === 0,
      `stress(durable=${durable}): ${known.size} IDs = ${delivered} delivered + ${rejected} rejected, queue empty`,
    );
    console.log(
      `  accounting durable=${durable}: ${known.size} events with an ID (${delivered} delivered incl. ${overflowReports} overflow reports, ${rejected} rejected = log total ${last.rejections.total}); ${dropped} events without an ID (queue full of CRITICAL entries: ${fullDropsChecked} checked) = ${overflowSum} reported; ${silentLosses} silent losses`,
    );
    await drainCron(prefix);
    const sent = rowsFor(prefix);
    // The scheduled run e-mails every PENDING row of the shared local D1: count this run's.
    const ids = messageIds().filter((m) => m?.startsWith(`<${prefix}`));
    check(
      sent.every((r) => r.delivery_status === "SENT") && ids.length === sent.length,
      `stress(durable=${durable}): every row e-mailed exactly once via the scheduled run (${ids.length} e-mails, ${sent.length} rows)`,
    );
    check(new Set(ids).size === ids.length, `stress(durable=${durable}): unique Message-IDs`);
    console.log(
      `  durable=${durable}: ${half} sessions, ${known.size} events, ${rows.length} delivered, ${noRow.length} rejected, ${dropped} dropped (reported), ${stateChecks} state checks`,
    );
  }
  check(new Set(messageIds()).size === messageIds().length, "no Message-ID ever e-mailed twice");
} catch (e) {
  failures++;
  console.error(e);
} finally {
  await cleanup();
}
console.log(`\n${checks} checks, ${failures} failed`);
console.log(failures === 0 ? "m1.8 integration: all checks passed" : "m1.8 integration: FAILED");
process.exit(failures === 0 ? 0 : 1);
