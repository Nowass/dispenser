/**
 * Gmail API transport (M1.6b): OAuth 2.0 access-token refresh and users.messages.send.
 * Scope: https://www.googleapis.com/auth/gmail.send only. Tokens are kept in memory for
 * one delivery run and never logged; error details contain only HTTP statuses and Google
 * error codes.
 */
import type { DeliveryConfig } from "./config";

export const GMAIL_SEND_SCOPE = "https://www.googleapis.com/auth/gmail.send";
export const DEFAULT_TIMEOUT_MS = 10_000;

export type FetchFn = (input: string, init: RequestInit) => Promise<Response>;

export type SendOutcome =
  | { kind: "sent"; messageId: string | null }
  /** Credentials, scope or account problem: not the event's fault; retry after fixing. */
  | { kind: "config"; detail: string }
  | { kind: "rate_limited"; detail: string; retryAfterS: number | null }
  /** Temporary failure; the message may or may not have been accepted (e.g. timeout). */
  | { kind: "transient"; detail: string }
  /**
   * Gmail rejected this message itself and the rejection clearly identifies a
   * non-recoverable payload problem (413, or a 400 that names the raw payload or its size).
   */
  | { kind: "permanent"; detail: string }
  /**
   * HTTP 400 that names neither the account/configuration nor the payload: may be either.
   * Kept PENDING (retried with backoff, never FAILED) and flagged for a human.
   */
  | { kind: "unclear"; detail: string };

export type RefreshOutcome =
  { kind: "ok" } | { kind: "config"; detail: string } | { kind: "transient"; detail: string };

/** 400 reasons that describe the account, not the message (e.g. Gmail not enabled). */
const ACCOUNT_REASONS = new Set([
  "failedPrecondition",
  "FAILED_PRECONDITION",
  "delegationDenied",
  "domainPolicy",
  "authError",
  "invalidCredentials",
]);

/**
 * HTTP 400 messages that point at configuration (sender, recipient, headers, account,
 * policy). These must never fail an event: the administrator fixes the configuration.
 */
const CONFIG_400_MESSAGE =
  /precondition|not enabled|mail service|delegat|header|recipient|sender|from address|account|domain|policy|permission|scope|credential|unauthori[sz]ed/i;

/**
 * HTTP 400 messages that clearly identify an invalid, non-recoverable payload: the raw
 * RFC 822 message itself is missing/undecodable or exceeds the size limit. Retrying the
 * same immutable message can never succeed. Anything not matched here or above is
 * "unclear" and stays PENDING.
 */
const PAYLOAD_400_MESSAGE =
  /too large|maximum (message )?size|size limit|'?raw'? rfc ?822 payload|invalid raw|raw (field|message) (is )?(invalid|malformed)|could not (parse|decode) (the )?(raw|message)|malformed (mime|message)/i;

const RATE_LIMIT_REASONS = new Set([
  "rateLimitExceeded",
  "userRateLimitExceeded",
  "dailyLimitExceeded",
]);

function isTimeout(e: unknown): boolean {
  return e instanceof Error && (e.name === "TimeoutError" || e.name === "AbortError");
}

async function jsonOrNull(res: Response): Promise<Record<string, unknown> | null> {
  try {
    const v: unknown = await res.json();
    return typeof v === "object" && v !== null ? (v as Record<string, unknown>) : null;
  } catch {
    return null;
  }
}

/** Google API error reason, e.g. "rateLimitExceeded" (never message text). */
function googleReason(body: Record<string, unknown> | null): string {
  const err = body?.error;
  if (typeof err === "string") return err.slice(0, 64); // OAuth endpoint: {"error":"invalid_grant"}
  if (typeof err === "object" && err !== null) {
    const errors = (err as { errors?: unknown }).errors;
    if (Array.isArray(errors) && typeof errors[0]?.reason === "string") {
      return String(errors[0].reason).slice(0, 64);
    }
    const status = (err as { status?: unknown }).status;
    if (typeof status === "string") return status.slice(0, 64);
  }
  return "unknown";
}

/**
 * Google API error message, used ONLY for classification and never logged or stored
 * (details carry only statuses and reason codes).
 */
function googleMessage(body: Record<string, unknown> | null): string {
  const err = body?.error;
  if (typeof err !== "object" || err === null) return "";
  const parts: string[] = [];
  const m = (err as { message?: unknown }).message;
  if (typeof m === "string") parts.push(m);
  const errors = (err as { errors?: unknown }).errors;
  if (Array.isArray(errors)) {
    for (const e of errors) {
      if (typeof e?.message === "string") parts.push(String(e.message));
    }
  }
  return parts.join(" ").slice(0, 1024);
}

/** Conservative classification of a Gmail HTTP 400 (see the constants above). */
export function classify400(
  body: Record<string, unknown> | null,
): "config" | "permanent" | "unclear" {
  const message = googleMessage(body);
  if (ACCOUNT_REASONS.has(googleReason(body)) || CONFIG_400_MESSAGE.test(message)) {
    return "config";
  }
  return PAYLOAD_400_MESSAGE.test(message) ? "permanent" : "unclear";
}

function retryAfterSeconds(res: Response): number | null {
  const h = res.headers.get("Retry-After");
  if (h === null) return null;
  if (/^\d{1,6}$/.test(h.trim())) return Number(h.trim());
  const t = Date.parse(h);
  return Number.isNaN(t) ? null : Math.max(0, Math.ceil((t - Date.now()) / 1000));
}

export class GmailClient {
  private accessToken: string | null = null;

  constructor(
    private readonly cfg: DeliveryConfig,
    private readonly fetchFn: FetchFn,
    private readonly timeoutMs: number = DEFAULT_TIMEOUT_MS,
  ) {}

  /** Exchanges the refresh token for a fresh access token. */
  async refresh(): Promise<RefreshOutcome> {
    this.accessToken = null;
    let res: Response;
    try {
      res = await this.fetchFn(this.cfg.tokenUrl, {
        method: "POST",
        headers: { "Content-Type": "application/x-www-form-urlencoded" },
        body: new URLSearchParams({
          client_id: this.cfg.clientId,
          client_secret: this.cfg.clientSecret,
          refresh_token: this.cfg.refreshToken,
          grant_type: "refresh_token",
        }).toString(),
        signal: AbortSignal.timeout(this.timeoutMs),
      });
    } catch (e) {
      return { kind: "transient", detail: isTimeout(e) ? "token_timeout" : "token_network" };
    }
    const body = await jsonOrNull(res);
    if (res.ok) {
      const token = body?.access_token;
      if (typeof token === "string" && token.length > 0) {
        this.accessToken = token;
        return { kind: "ok" };
      }
      return { kind: "transient", detail: "token_malformed_response" };
    }
    if (res.status === 429 || res.status >= 500) {
      return { kind: "transient", detail: `token_http_${res.status}` };
    }
    // 400/401: invalid_grant (expired or revoked refresh token, e.g. the 7-day Testing
    // limit), invalid_client, unauthorized_client: needs a human.
    return { kind: "config", detail: `token_${googleReason(body)}` };
  }

  /** Sends one RFC 5322 message (base64url "raw"). Refreshes once on HTTP 401. */
  async send(raw: string): Promise<SendOutcome> {
    if (this.accessToken === null) {
      const r = await this.refresh();
      if (r.kind !== "ok") return r;
    }
    const first = await this.sendOnce(raw);
    if (first.kind !== "unauthorized") return first;
    const r = await this.refresh(); // expired or revoked access token
    if (r.kind !== "ok") return r;
    const second = await this.sendOnce(raw);
    return second.kind === "unauthorized"
      ? { kind: "config", detail: "gmail_unauthorized" }
      : second;
  }

  private async sendOnce(raw: string): Promise<SendOutcome | { kind: "unauthorized" }> {
    let res: Response;
    try {
      res = await this.fetchFn(`${this.cfg.gmailApiBase}/gmail/v1/users/me/messages/send`, {
        method: "POST",
        headers: {
          Authorization: `Bearer ${this.accessToken ?? ""}`,
          "Content-Type": "application/json",
        },
        body: JSON.stringify({ raw }),
        signal: AbortSignal.timeout(this.timeoutMs),
      });
    } catch (e) {
      // After a timeout Gmail may still have accepted the message: retrying may duplicate it.
      return { kind: "transient", detail: isTimeout(e) ? "gmail_timeout" : "gmail_network" };
    }
    const body = await jsonOrNull(res);
    if (res.ok) {
      const id = body?.id;
      return { kind: "sent", messageId: typeof id === "string" ? id.slice(0, 128) : null };
    }
    const reason = googleReason(body);
    if (res.status === 401) return { kind: "unauthorized" };
    if (res.status === 429 || (res.status === 403 && RATE_LIMIT_REASONS.has(reason))) {
      return {
        kind: "rate_limited",
        detail: `gmail_${res.status}_${reason}`,
        retryAfterS: retryAfterSeconds(res),
      };
    }
    if (res.status === 403 || res.status === 404) {
      // Scope missing, account disabled or without Gmail, user not found: the account or
      // its authorization needs an administrator; never the event's fault.
      return { kind: "config", detail: `gmail_${res.status}_${reason}` };
    }
    if (res.status === 413) {
      // Too large: retrying the same immutable message can never succeed.
      return { kind: "permanent", detail: `gmail_413_${reason}` };
    }
    if (res.status === 400) {
      // HTTP 400 is ambiguous (bad payload OR bad configuration): conservative mapping.
      switch (classify400(body)) {
        case "config":
          return { kind: "config", detail: `gmail_400_${reason}` };
        case "permanent":
          return { kind: "permanent", detail: `gmail_400_invalid_payload` };
        case "unclear":
          return { kind: "unclear", detail: `gmail_400_unclear_${reason}` };
      }
    }
    // 408, 5xx and any other unexpected status: temporary, retried without limit.
    return { kind: "transient", detail: `gmail_${res.status}` };
  }
}
