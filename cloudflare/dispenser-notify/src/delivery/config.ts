/**
 * E-mail delivery configuration (M1.6b). Everything comes from Cloudflare Secrets, so no
 * credential or personal address is ever in the repository. Missing or invalid
 * configuration disables delivery (fails closed); ingestion is unaffected.
 */

export const GMAIL_API_BASE = "https://gmail.googleapis.com";
export const GOOGLE_TOKEN_URL = "https://oauth2.googleapis.com/token";

export interface DeliveryConfig {
  clientId: string;
  clientSecret: string;
  refreshToken: string;
  sender: string;
  recipient: string;
  gmailApiBase: string;
  tokenUrl: string;
}

export type ConfigResult = { ok: true; config: DeliveryConfig } | { ok: false; problems: string[] };

/** Names of the required secrets (values are never logged). */
export const REQUIRED_SECRETS = [
  "GMAIL_CLIENT_ID",
  "GMAIL_CLIENT_SECRET",
  "GMAIL_REFRESH_TOKEN",
  "NOTIFY_SENDER_ADDRESS",
  "NOTIFY_RECIPIENT_ADDRESS",
] as const;

/** A plain addr-spec without display name, comments or header-breaking characters. */
const ADDRESS_RE = /^[A-Za-z0-9.!#$%&'*+/=?^_`{|}~-]+@[A-Za-z0-9-]+(\.[A-Za-z0-9-]+)+$/;

export function isValidAddress(a: string): boolean {
  return a.length <= 254 && ADDRESS_RE.test(a);
}

/**
 * Endpoint overrides exist only for local integration tests against a fake Gmail server:
 * they are accepted for loopback HTTP URLs only, so a deployed Worker always talks to
 * Google.
 */
function endpoint(value: unknown, fallback: string): string {
  if (typeof value === "string" && /^http:\/\/(127\.0\.0\.1|localhost):\d{1,5}(\/|$)/.test(value)) {
    return value.replace(/\/$/, "");
  }
  return fallback;
}

export function loadConfig(env: Record<string, unknown>): ConfigResult {
  const problems: string[] = [];
  const get = (name: string): string => {
    const v = env[name];
    if (typeof v !== "string" || v.trim() === "") {
      problems.push(`${name} missing`);
      return "";
    }
    return v.trim();
  };
  const clientId = get("GMAIL_CLIENT_ID");
  const clientSecret = get("GMAIL_CLIENT_SECRET");
  const refreshToken = get("GMAIL_REFRESH_TOKEN");
  const sender = get("NOTIFY_SENDER_ADDRESS");
  const recipient = get("NOTIFY_RECIPIENT_ADDRESS");
  if (sender !== "" && !isValidAddress(sender)) problems.push("NOTIFY_SENDER_ADDRESS invalid");
  if (recipient !== "" && !isValidAddress(recipient)) {
    problems.push("NOTIFY_RECIPIENT_ADDRESS invalid");
  }
  if (problems.length > 0) {
    return { ok: false, problems };
  }
  return {
    ok: true,
    config: {
      clientId,
      clientSecret,
      refreshToken,
      sender,
      recipient,
      gmailApiBase: endpoint(env.GMAIL_API_BASE_OVERRIDE, GMAIL_API_BASE),
      tokenUrl: endpoint(env.GOOGLE_TOKEN_URL_OVERRIDE, GOOGLE_TOKEN_URL),
    },
  };
}
