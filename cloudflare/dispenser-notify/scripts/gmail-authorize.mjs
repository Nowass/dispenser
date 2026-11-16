#!/usr/bin/env node
/**
 * One-time Gmail OAuth authorization for the Dispenser V2 notification sender (M1.6b).
 *
 * Run it yourself, interactively, on your own machine (never in CI, never by an agent):
 *
 *   node scripts/gmail-authorize.mjs \
 *     --client-file ~/Downloads/client_secret_XXXX.json \
 *     --out ~/.config/dispenser-notify-deploy/gmail-secrets.json \
 *     --sender <dedicated sender address> --recipient <your personal address>
 *
 * - Uses the OAuth client of type "Desktop app" (installed-app flow) with a loopback
 *   redirect on 127.0.0.1, PKCE (S256) and a random state. --sender is also passed to
 *   Google as login hint, so the consent screen preselects the dedicated account.
 * - Requests ONLY https://www.googleapis.com/auth/gmail.send, with access_type=offline and
 *   prompt=consent so that Google issues a refresh token.
 * - Prints only the authorization URL (it contains no secret). The refresh token and the
 *   client secret are written to --out (mode 0600, outside the repository) as
 *   {"GMAIL_CLIENT_ID", "GMAIL_CLIENT_SECRET", "GMAIL_REFRESH_TOKEN"} plus, when given,
 *   "NOTIFY_SENDER_ADDRESS" and "NOTIFY_RECIPIENT_ADDRESS", ready for
 *   `wrangler secret bulk <file>` or `wrangler deploy --secrets-file <file>`. Delete the file
 *   afterwards. The access token is discarded.
 */
import { createHash, randomBytes } from "node:crypto";
import { chmodSync, existsSync, readFileSync, realpathSync, writeFileSync } from "node:fs";
import { createServer } from "node:http";
import { dirname, resolve } from "node:path";
import { fileURLToPath } from "node:url";

const SCOPE = "https://www.googleapis.com/auth/gmail.send";
const AUTH_URL = "https://accounts.google.com/o/oauth2/v2/auth";
const TOKEN_URL = "https://oauth2.googleapis.com/token";
const TIMEOUT_MS = 5 * 60 * 1000;
const repoRoot = resolve(dirname(fileURLToPath(import.meta.url)), "..", "..", "..");

function fail(message) {
  console.error(`error: ${message}`);
  process.exit(1);
}

function args() {
  const a = process.argv.slice(2);
  const o = {};
  for (let i = 0; i < a.length; i++) {
    const k = a[i];
    if (!k.startsWith("--")) fail(`unexpected argument ${k}`);
    if (k === "--force") {
      o.force = true;
      continue;
    }
    if (i + 1 >= a.length) fail(`${k} needs a value`);
    o[k.slice(2)] = a[++i];
  }
  return o;
}

/** Test-only endpoint overrides are accepted for loopback HTTP URLs only. */
function endpoint(value, fallback) {
  if (value === undefined) return fallback;
  if (!/^http:\/\/127\.0\.0\.1:\d{1,5}\//.test(value))
    fail("endpoint overrides must be http://127.0.0.1:<port>/...");
  return value;
}

const opt = args();
if (!opt["client-file"] || !opt.out) {
  fail(
    "usage: gmail-authorize.mjs --client-file <client_secret.json> --out <secrets.json> [--sender <address>] [--recipient <address>]",
  );
}
const ADDRESS_RE = /^[A-Za-z0-9.!#$%&'*+/=?^_`{|}~-]+@[A-Za-z0-9-]+(\.[A-Za-z0-9-]+)+$/;
for (const k of ["sender", "recipient"]) {
  if (opt[k] !== undefined && !(opt[k].length <= 254 && ADDRESS_RE.test(opt[k])))
    fail(`--${k} is not a plain e-mail address`);
}
const outPath = resolve(opt.out);
const outDir = dirname(outPath);
if (!existsSync(outDir))
  fail(`output directory ${outDir} does not exist (create it with mode 700)`);
const realOutDir = realpathSync(outDir);
if (realOutDir === repoRoot || realOutDir.startsWith(`${repoRoot}/`)) {
  fail("refusing to write credentials inside the repository; choose a path outside it");
}
if (existsSync(outPath) && !opt.force) fail(`${outPath} exists (use --force to overwrite)`);

let client;
try {
  const j = JSON.parse(readFileSync(resolve(opt["client-file"]), "utf8"));
  client = j.installed;
  if (!client) fail('client file is not a "Desktop app" OAuth client (no "installed" section)');
} catch (e) {
  fail(`cannot read client file: ${e instanceof Error ? e.message : e}`);
}
if (typeof client.client_id !== "string" || typeof client.client_secret !== "string") {
  fail("client file lacks client_id / client_secret");
}
const authUrl = endpoint(opt["auth-url"], AUTH_URL);
const tokenUrl = endpoint(opt["token-url"], TOKEN_URL);

const verifier = randomBytes(48).toString("base64url");
const challenge = createHash("sha256").update(verifier).digest("base64url");
const state = randomBytes(24).toString("base64url");

const server = createServer();
await new Promise((r) => server.listen(0, "127.0.0.1", r));
const redirectUri = `http://127.0.0.1:${server.address().port}/callback`;

const url = new URL(authUrl);
url.search = new URLSearchParams({
  response_type: "code",
  client_id: client.client_id,
  redirect_uri: redirectUri,
  scope: SCOPE,
  access_type: "offline",
  prompt: "consent",
  code_challenge: challenge,
  code_challenge_method: "S256",
  state,
  ...(opt.sender ? { login_hint: opt.sender } : {}),
}).toString();

console.log("Open this URL in a browser and sign in with the DEDICATED sender account");
console.log(
  `(${opt.sender ?? "the Dispenser notification account"}), NOT your personal account. Review the permission`,
);
console.log('("Send email on your behalf") and approve it:\n');
console.log(url.toString());
console.log(`\nWaiting for the redirect to ${redirectUri} (5 minutes) ...`);

const timer = setTimeout(() => {
  server.close();
  fail("timed out waiting for the authorization");
}, TIMEOUT_MS);

server.on("request", async (req, res) => {
  const u = new URL(req.url ?? "/", redirectUri);
  if (u.pathname !== "/callback") {
    res.writeHead(404).end();
    return;
  }
  const finish = (status, text, exitCode) => {
    res.writeHead(status, { "Content-Type": "text/plain; charset=utf-8" }).end(text);
    clearTimeout(timer);
    server.close();
    if (exitCode !== 0) console.error(`error: ${text}`);
    process.exitCode = exitCode;
  };
  if (u.searchParams.get("state") !== state) {
    finish(400, "State mismatch: authorization rejected. Start again.", 1);
    return;
  }
  if (u.searchParams.has("error")) {
    finish(400, `Authorization not granted (${u.searchParams.get("error")}).`, 1);
    return;
  }
  const code = u.searchParams.get("code");
  if (!code) {
    finish(400, "No authorization code received.", 1);
    return;
  }
  let body;
  try {
    const r = await fetch(tokenUrl, {
      method: "POST",
      headers: { "Content-Type": "application/x-www-form-urlencoded" },
      body: new URLSearchParams({
        code,
        client_id: client.client_id,
        client_secret: client.client_secret,
        redirect_uri: redirectUri,
        grant_type: "authorization_code",
        code_verifier: verifier,
      }),
    });
    body = await r.json();
    if (!r.ok) {
      finish(502, `Token exchange failed (${r.status} ${body?.error ?? "unknown"}).`, 1);
      return;
    }
  } catch {
    finish(502, "Token exchange failed (network).", 1);
    return;
  }
  const granted = String(body.scope ?? "")
    .split(" ")
    .filter(Boolean);
  if (granted.length !== 1 || granted[0] !== SCOPE) {
    finish(400, `Unexpected scopes granted (${granted.join(", ") || "none"}); nothing written.`, 1);
    return;
  }
  if (typeof body.refresh_token !== "string" || body.refresh_token === "") {
    finish(
      400,
      "Google returned no refresh token. Remove the app's access at https://myaccount.google.com/permissions and try again.",
      1,
    );
    return;
  }
  const secrets = {
    GMAIL_CLIENT_ID: client.client_id,
    GMAIL_CLIENT_SECRET: client.client_secret,
    GMAIL_REFRESH_TOKEN: body.refresh_token,
    ...(opt.sender ? { NOTIFY_SENDER_ADDRESS: opt.sender } : {}),
    ...(opt.recipient ? { NOTIFY_RECIPIENT_ADDRESS: opt.recipient } : {}),
  };
  writeFileSync(outPath, `${JSON.stringify(secrets, null, 2)}\n`, {
    mode: 0o600,
    flag: opt.force ? "w" : "wx",
  });
  chmodSync(outPath, 0o600); // also when --force overwrote an existing file
  finish(200, "Authorization complete. You can close this window.", 0);
  console.log(`\nDone. Granted scope: ${SCOPE}`);
  console.log(`Secrets written to ${outPath} (mode 0600). Tokens were not printed.`);
  console.log("Next: wrangler secret bulk <that file> (see GMAIL_SETUP.md), then delete the file.");
});
