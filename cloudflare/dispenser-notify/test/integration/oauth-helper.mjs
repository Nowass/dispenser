#!/usr/bin/env node
/**
 * Tests scripts/gmail-authorize.mjs against a fake Google OAuth server on 127.0.0.1:
 * PKCE, state check, scope check, output file permissions, and that no token is printed.
 * No Google account or credential is involved.
 */
import { spawn } from "node:child_process";
import { createHash } from "node:crypto";
import { existsSync, mkdtempSync, readFileSync, rmSync, statSync, writeFileSync } from "node:fs";
import { createServer } from "node:http";
import { tmpdir } from "node:os";
import { dirname, join } from "node:path";
import { fileURLToPath } from "node:url";

const root = join(dirname(fileURLToPath(import.meta.url)), "..", "..");
const helper = join(root, "scripts", "gmail-authorize.mjs");
const SCOPE = "https://www.googleapis.com/auth/gmail.send";
const REFRESH = "fake-refresh-token-must-not-be-printed";

let failures = 0;
function check(cond, what) {
  console.log(`${cond ? "ok  " : "FAIL"} ${what}`);
  if (!cond) failures++;
}

const work = mkdtempSync(join(tmpdir(), "dispenser-oauth-it-"));
const clientFile = join(work, "client_secret.json");
writeFileSync(
  clientFile,
  JSON.stringify({
    installed: { client_id: "fake-id.apps.googleusercontent.com", client_secret: "fake-secret" },
  }),
);

let tokenRequests = [];
let grantedScope = SCOPE;
const fake = createServer((req, res) => {
  let body = "";
  req.on("data", (d) => (body += d));
  req.on("end", () => {
    tokenRequests.push(new URLSearchParams(body));
    res.setHeader("Content-Type", "application/json");
    res.end(
      JSON.stringify({
        access_token: "fake-access",
        refresh_token: REFRESH,
        scope: grantedScope,
        token_type: "Bearer",
        expires_in: 3599,
      }),
    );
  });
});
await new Promise((r) => fake.listen(0, "127.0.0.1", r));
const fakeBase = `http://127.0.0.1:${fake.address().port}`;

/** Runs the helper; `browser(authUrl)` plays the user's browser. */
async function runHelper(out, browser, extra = []) {
  const child = spawn(
    process.execPath,
    [
      helper,
      "--client-file",
      clientFile,
      "--out",
      out,
      "--auth-url",
      `${fakeBase}/auth`,
      "--token-url",
      `${fakeBase}/token`,
      ...extra,
    ],
    { stdio: ["ignore", "pipe", "pipe"] },
  );
  let stdout = "";
  let stderr = "";
  let opened = false;
  child.stderr.on("data", (d) => (stderr += d));
  const done = new Promise((r) => child.on("exit", (code) => r(code)));
  child.stdout.on("data", async (d) => {
    stdout += d;
    const m = stdout.match(/^(http:\/\/127\.0\.0\.1:\d+\/auth\?\S+)$/m);
    if (m && !opened) {
      opened = true;
      await browser(new URL(m[1]));
    }
  });
  const code = await done;
  return { code, stdout, stderr };
}

async function follow(authUrl, params) {
  const cb = new URL(authUrl.searchParams.get("redirect_uri"));
  for (const [k, v] of Object.entries(params)) cb.searchParams.set(k, v);
  const r = await fetch(cb);
  await r.text();
  return r.status;
}

try {
  // 1. Happy path.
  const out = join(work, "gmail-secrets.json");
  let auth;
  const r = await runHelper(
    out,
    async (u) => {
      auth = u;
      await follow(u, { code: "fake-code", state: u.searchParams.get("state") });
    },
    ["--sender", "sender@example.com", "--recipient", "owner@example.org"],
  );
  check(r.code === 0, "helper exits 0");
  check(auth.searchParams.get("scope") === SCOPE, "requests only the gmail.send scope");
  check(auth.searchParams.get("access_type") === "offline", "offline access (refresh token)");
  check(auth.searchParams.get("prompt") === "consent", "prompt=consent");
  check(auth.searchParams.get("code_challenge_method") === "S256", "PKCE S256");
  check(auth.searchParams.get("login_hint") === "sender@example.com", "login hint passed");
  check(
    /^http:\/\/127\.0\.0\.1:\d+\/callback$/.test(auth.searchParams.get("redirect_uri")),
    "loopback redirect",
  );
  const tr = tokenRequests[0];
  check(
    tr?.get("grant_type") === "authorization_code" && tr?.get("code") === "fake-code",
    "code exchanged",
  );
  check(
    createHash("sha256")
      .update(tr?.get("code_verifier") ?? "")
      .digest("base64url") === auth.searchParams.get("code_challenge"),
    "code_verifier matches the challenge",
  );
  check(existsSync(out) && (statSync(out).mode & 0o777) === 0o600, "secrets file has mode 0600");
  const secrets = JSON.parse(readFileSync(out, "utf8"));
  check(
    secrets.GMAIL_REFRESH_TOKEN === REFRESH &&
      secrets.GMAIL_CLIENT_ID === "fake-id.apps.googleusercontent.com" &&
      secrets.GMAIL_CLIENT_SECRET === "fake-secret" &&
      secrets.NOTIFY_SENDER_ADDRESS === "sender@example.com" &&
      secrets.NOTIFY_RECIPIENT_ADDRESS === "owner@example.org" &&
      Object.keys(secrets).length === 5,
    "secrets file holds exactly the five delivery secrets",
  );
  check(
    !r.stdout.includes(REFRESH) && !r.stderr.includes(REFRESH) && !r.stdout.includes("fake-secret"),
    "no secret printed",
  );

  // 2. Existing output file is not overwritten.
  const again = await runHelper(out, async () => {});
  check(
    again.code === 1 && again.stderr.includes("exists"),
    "refuses to overwrite without --force",
  );

  // 3. State mismatch: rejected, nothing written.
  const out2 = join(work, "mismatch.json");
  const mm = await runHelper(out2, (u) => follow(u, { code: "x", state: "wrong" }));
  check(mm.code === 1 && !existsSync(out2), "state mismatch rejected, no file");

  // 4. Broader scope than requested: rejected, nothing written.
  grantedScope = `${SCOPE} https://mail.google.com/`;
  const out3 = join(work, "scope.json");
  const sc = await runHelper(out3, (u) =>
    follow(u, { code: "x", state: u.searchParams.get("state") }),
  );
  check(sc.code === 1 && !existsSync(out3), "unexpected scopes rejected, no file");
  grantedScope = SCOPE;

  // 5. User denied consent.
  const out4 = join(work, "denied.json");
  const dn = await runHelper(out4, (u) =>
    follow(u, { error: "access_denied", state: u.searchParams.get("state") }),
  );
  check(dn.code === 1 && !existsSync(out4), "denied consent: no file");

  // 6. An address with a header-injection attempt is refused before anything starts.
  const badAddr = await runHelper(join(work, "bad.json"), async () => {}, [
    "--recipient",
    "a@b.cz\r\nBcc: x@y.z",
  ]);
  check(
    badAddr.code === 1 && badAddr.stderr.includes("not a plain e-mail address"),
    "invalid address refused",
  );

  // 7. Output inside the repository is refused before anything starts.
  const inRepo = await runHelper(join(root, "gmail-secrets.json"), async () => {});
  check(
    inRepo.code === 1 && inRepo.stderr.includes("inside the repository"),
    "refuses to write inside the repo",
  );
  check(!existsSync(join(root, "gmail-secrets.json")), "no file created in the repo");
} catch (e) {
  failures++;
  console.error(String(e));
} finally {
  fake.close();
  rmSync(work, { recursive: true, force: true });
}

console.log(
  failures === 0 ? "oauth helper: all checks passed" : `oauth helper: ${failures} failed`,
);
process.exit(failures === 0 ? 0 : 1);
