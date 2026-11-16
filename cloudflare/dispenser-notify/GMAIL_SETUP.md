# Gmail delivery setup (M1.6b)

How to prepare the **dedicated Dispenser V2 Google account** and its OAuth access, so that
the Worker can send notification e-mails through the Gmail API.

You do every step yourself, in your own browser and terminal. Never paste a password,
client secret or refresh token into a chat, an issue, a commit or a log.

**Accounts:**

- **Sender:** a new Google account used only for Dispenser V2 notifications (any name you
  choose, `<name>@gmail.com`), with two-step verification on.
- **Recipient:** your personal Gmail address. It is never authorized and receives only
  e-mails.

**What the Worker can do:** send e-mail as the sender account, nothing else. The scope is
`https://www.googleapis.com/auth/gmail.send`. It cannot read, list or delete mail.

## 1. Create the dedicated account

1. Create the Google account in a private browser window, so it doesn't mix with your
   personal session.
2. Turn on **two-step verification**: <https://myaccount.google.com/security>. Store the
   password and backup codes in your password manager.
3. Optionally set a forwarding or recovery address. The account needs no mail client and
   no app password. Do **not** create an app password: the Worker uses OAuth only.

## 2. Google Cloud project and Gmail API

Use the dedicated account for everything below.

1. <https://console.cloud.google.com/>: **create a new project**, for example
   `dispenser-notify`. No billing account is needed for the Gmail API at this volume.
2. **APIs & Services → Library → Gmail API → Enable.**

## 3. OAuth consent screen (Google Auth Platform)

**Google Auth Platform → Branding / Audience / Data access:**

1. **Branding:** app name `Dispenser V2 notify`, support e-mail = the dedicated account,
   developer contact = the dedicated account. No logo (a logo would trigger brand
   verification).
2. **Audience → User type: External.** "Internal" exists only for Google Workspace
   organizations, not for personal Gmail accounts.
3. **Data access → Add or remove scopes:** add only `.../auth/gmail.send` ("Send email on
   your behalf"). Google classifies it as a _sensitive_ scope (not _restricted_).
4. **Audience → Test users:** add the dedicated account while the app is in Testing.

### Publishing status: Testing vs In production

|                            | Testing                                                                                                           | In production (unverified, personal use)                                                                                           |
| -------------------------- | ----------------------------------------------------------------------------------------------------------------- | ---------------------------------------------------------------------------------------------------------------------------------- |
| Who can authorize          | only listed test users (up to 100)                                                                                | any Google account, after the "Google hasn't verified this app" warning                                                            |
| **Refresh token lifetime** | **expires after 7 days** (Google's rule for External apps in Testing that request more than basic profile scopes) | no 7-day expiry; remains valid until revoked, unused for 6 months, the password changes, or the per-client token limit is exceeded |
| Verification               | not needed                                                                                                        | **not needed for personal use** (the app is used only by its owner; there is a cap of 100 users)                                   |

**Consequence:** in Testing, delivery stops every 7 days with `token_invalid_grant`
(visible in the logs as `delivery_auth_error`), and you would have to re-authorize weekly.
For permanent operation:

1. **Publish the app** (Audience → Publish app → In production). Do not submit it for
   verification: this is a personal, single-user app.
2. Authorize once (step 5). On the consent screen Google shows _"Google hasn't verified
   this app"_. Because it is your own app, choose **Advanced → Go to Dispenser V2 notify
   (unsafe)** and approve "Send email on your behalf".
3. The app stays limited to its owner in practice. Never share the client or authorize
   other accounts.

Short tests in Testing mode are fine, but expect re-authorization after 7 days.

## 4. OAuth client

**Google Auth Platform → Clients → Create client → Application type: Desktop app**, named
for example `dispenser-notify-authorize`.

- Download the JSON (`client_secret_….json`). It contains the client ID and the client
  secret.
- Store it outside the repository, for example in `~/.config/dispenser-notify-deploy/`
  (mode 700), and treat it as a secret.
- "Desktop app" allows the loopback redirect (`http://127.0.0.1:<port>`) that the helper
  script uses. No public redirect URL and no web server are needed.

## 5. Authorize (one time, interactive)

From `cloudflare/dispenser-notify`, with Node.js ≥ 22.12:

```sh
umask 077
mkdir -p ~/.config/dispenser-notify-deploy
node scripts/gmail-authorize.mjs \
  --client-file ~/.config/dispenser-notify-deploy/client_secret.json \
  --out ~/.config/dispenser-notify-deploy/gmail-secrets.json \
  --sender <dedicated sender address> \
  --recipient <your personal address>
```

1. The script prints a Google URL (no secret in it). Open it in the browser **signed in
   as the dedicated account**; `--sender` preselects it.
2. Approve the single permission "Send email on your behalf" (after the "unverified app"
   step if the app is published).
3. Google redirects to `127.0.0.1`. The script:
   - checks the state and that **only** `gmail.send` was granted;
   - exchanges the code using PKCE;
   - writes `gmail-secrets.json` (mode 0600) with the five Worker secrets
     `GMAIL_CLIENT_ID`, `GMAIL_CLIENT_SECRET`, `GMAIL_REFRESH_TOKEN`,
     `NOTIFY_SENDER_ADDRESS` and `NOTIFY_RECIPIENT_ADDRESS`.

   It never prints a token and refuses to write inside the repository.

4. If Google returns no refresh token (rare: an older grant exists), remove the app at
   <https://myaccount.google.com/permissions> (dedicated account) and run it again.

The file is uploaded during the approved deployment (`README.md`, "Deployment"), then
deleted.

## 6. Revocation and rotation

| Goal                              | Action                                                                                                                                                                                                                                                        |
| --------------------------------- | ------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| Stop all e-mail immediately       | `npx wrangler secret delete GMAIL_REFRESH_TOKEN`: delivery becomes "disabled" (fails closed); events stay `PENDING` and are sent after re-configuration                                                                                                       |
| Revoke Google access              | dedicated account → <https://myaccount.google.com/permissions> → Dispenser V2 notify → Remove access. The Worker then logs `token_invalid_grant` and sets `auth_attention = 1`; no attempts are consumed, events wait, and Google is retried every 30 minutes |
| New refresh token                 | revoke (above), run step 5 again, upload the new `GMAIL_REFRESH_TOKEN`                                                                                                                                                                                        |
| Rotate the client secret          | Google Auth Platform → Clients → the client → add a secret; upload it as `GMAIL_CLIENT_SECRET`; verify a delivery; then disable and delete the old secret                                                                                                     |
| Suspected leak of the client file | delete the OAuth client in the console (this invalidates all its tokens), create a new one, repeat steps 4–5                                                                                                                                                  |
| Change the recipient              | upload a new `NOTIFY_RECIPIENT_ADDRESS` secret (no Google change)                                                                                                                                                                                             |

A refresh token also becomes invalid when:

- the dedicated account's password changes (tokens with Gmail scopes are revoked);
- it is unused for 6 months;
- the app stays in Testing (7 days);
- too many tokens are issued for the same client and account (the oldest are invalidated).

The symptom is always `delivery_auth_error … token_invalid_grant` in the Worker logs, while
events queue as `PENDING`.

## 7. Limits

- A consumer Gmail account may send to about 500 recipients a day. Dispenser V2 sends a
  few e-mails a week.
- The Gmail API may rate-limit (`429` / `rateLimitExceeded`). The Worker backs off (at
  least the `Retry-After` value) and keeps the events.
