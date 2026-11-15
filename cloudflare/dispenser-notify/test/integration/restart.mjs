#!/usr/bin/env node
/**
 * Local integration test (M1.6a): runs the real Worker with `wrangler dev` against a LOCAL,
 * persisted D1 database built from ./migrations, restarts the Worker process between
 * submissions and checks that accepted events survive and keep their idempotency.
 *
 * Never touches Cloudflare: --local only, a throwaway database directory and a random
 * per-run token. Usage: npm run test:integration
 */
import { spawn, spawnSync } from "node:child_process";
import { randomBytes } from "node:crypto";
import { mkdtempSync, rmSync } from "node:fs";
import { tmpdir } from "node:os";
import { dirname, join } from "node:path";
import { fileURLToPath } from "node:url";

const root = join(dirname(fileURLToPath(import.meta.url)), "..", "..");
const wrangler = join(root, "node_modules", ".bin", "wrangler");
const persist = mkdtempSync(join(tmpdir(), "dispenser-notify-it-"));
const token = randomBytes(24).toString("hex"); // 48 chars, per run, never a real secret
const port = 18787 + Math.floor(Math.random() * 1000);
const base = `http://127.0.0.1:${port}`;
const env = { ...process.env, CI: "1", WRANGLER_SEND_METRICS: "false", NO_COLOR: "1" };

let failures = 0;
function check(cond, what) {
  console.log(`${cond ? "ok  " : "FAIL"} ${what}`);
  if (!cond) failures++;
}

function wranglerSync(args) {
  const r = spawnSync(wrangler, args, { cwd: root, env, encoding: "utf8" });
  if (r.status !== 0) {
    throw new Error(`wrangler ${args[0]} failed:\n${r.stdout}\n${r.stderr}`);
  }
  return r.stdout;
}

async function startWorker() {
  const child = spawn(
    wrangler,
    [
      "dev",
      "--local",
      "--ip",
      "127.0.0.1",
      "--port",
      String(port),
      "--persist-to",
      persist,
      "--var",
      `DEVICE_API_TOKEN:${token}`,
      "--show-interactive-dev-session=false",
    ],
    { cwd: root, env, stdio: ["ignore", "pipe", "pipe"] },
  );
  let output = "";
  child.stdout.on("data", (d) => (output += d));
  child.stderr.on("data", (d) => (output += d));
  for (let i = 0; i < 120; i++) {
    if (child.exitCode !== null) throw new Error(`wrangler dev exited:\n${output}`);
    try {
      const res = await fetch(`${base}/health`);
      if (res.ok) return child;
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

function post(body) {
  return fetch(`${base}/api/v1/events`, {
    method: "POST",
    headers: { Authorization: `Bearer ${token}`, "Content-Type": "application/json" },
    body: JSON.stringify(body),
  });
}

const event = {
  schema_version: 1,
  event_id: "dsp-0123456789ab-a5a5a5a5-42",
  device_id: "0123456789ab",
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
};

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
  check((await fetch(`${base}/health`)).status === 200, "health check");
  check((await post(event)).status === 201, "first submission accepted (201)");
  check(
    (await fetch(`${base}/api/v1/events`, { headers: { Authorization: `Bearer ${token}` } }))
      .status === 405,
    "no listing endpoint",
  );
  await stopWorker(worker);
  worker = undefined;

  // Restart: a new workerd process, same local D1 files.
  worker = await startWorker();
  const dup = await post({ ...event, attempt: 2 });
  check(dup.status === 200, "after restart: identical retry is a duplicate (200)");
  const conflict = await post({ ...event, value: 5000, attempt: 3 });
  check(conflict.status === 409, "after restart: modified payload is a conflict (409)");
  const unauth = await fetch(`${base}/api/v1/events`, {
    method: "POST",
    headers: { "Content-Type": "application/json" },
    body: JSON.stringify(event),
  });
  check(unauth.status === 401, "after restart: missing token rejected (401)");
  await stopWorker(worker);
  worker = undefined;

  const out = wranglerSync([
    "d1",
    "execute",
    "dispenser-notifications",
    "--local",
    "--persist-to",
    persist,
    "--json",
    "--command",
    "SELECT event_id, delivery_status, payload FROM notifications",
  ]);
  const rows = JSON.parse(out)[0].results;
  check(rows.length === 1, "exactly one stored row");
  check(rows[0]?.delivery_status === "PENDING", "stored as PENDING");
  check(
    JSON.parse(rows[0]?.payload ?? "{}").value === 8000,
    "stored payload is the original (8000)",
  );
} catch (e) {
  failures++;
  console.error(String(e));
} finally {
  if (worker) await stopWorker(worker);
  rmSync(persist, { recursive: true, force: true });
}

console.log(failures === 0 ? "integration: all checks passed" : `integration: ${failures} failed`);
process.exit(failures === 0 ? 0 : 1);
