/**
 * One delivery run (M1.6b), started by the Cron Trigger: sends due PENDING notifications
 * by e-mail through the Gmail API.
 *
 * Semantics: at-least-once. Gmail may accept a message and the Worker may then fail before
 * it records SENT (crash, timeout, lost lease); the event is then sent again later with the
 * same Message-ID. Gmail is NOT guaranteed to deduplicate by Message-ID. Durable leases
 * prevent concurrent duplicate sends; an expired lease (crashed run) is taken over.
 *
 * Retry policy (approved): temporary failures (429, 5xx, timeouts, network) are retried
 * WITHOUT an attempt limit, with exponential backoff capped at 6 hours: a long outage
 * never abandons a notification. Only clearly non-recoverable rejections of the message
 * itself end in FAILED (413, or a 400 that names the raw payload or its size). Credential/
 * account/configuration problems never consume attempts; events wait as PENDING until the
 * configuration is fixed. An HTTP 400 that is neither clearly configuration nor clearly the
 * payload ("unclear") keeps the event PENDING with backoff and raises a health warning.
 * Message-level outcomes (FAILED, unclear) never block other events: the run continues.
 */
import { validateRequest, type NotificationEvent } from "../protocol";
import { loadConfig } from "./config";
import { formatEmail, SENDER_NAME } from "./format";
import { GmailClient, type FetchFn } from "./gmail";
import { messageIdFor, rawMessage } from "./mime";
import {
  claimNext,
  hasDue,
  markFailed,
  markRetry,
  markSent,
  releaseUncounted,
  type ClaimedEvent,
} from "./queue";
import { readHealth, readState, recordRun, type DeliveryHealth, type RunEnding } from "./state";

/** Sends per run (the run stops earlier at the first failure). */
export const MAX_SENDS_PER_RUN = 10;
/** Lease per event: far longer than one send (token refresh + send, each <= 10 s). */
export const LEASE_SECONDS = 300;
export const BACKOFF_BASE_SECONDS = 60;
export const BACKOFF_CAP_SECONDS = 6 * 3600;
/** While Google authorization needs attention, try Google again at most this often. */
export const AUTH_RETRY_SECONDS = 1800;

/** Delay after the n-th failed attempt: 1, 2, 4, ... minutes, capped at 6 hours. */
export function backoffSeconds(attempts: number): number {
  const n = Math.max(1, Math.min(attempts, 30));
  return Math.min(BACKOFF_BASE_SECONDS * 2 ** (n - 1), BACKOFF_CAP_SECONDS);
}

export interface DeliveryDeps {
  fetch: FetchFn;
  /** Unix seconds. */
  now: () => number;
  /** Random per-run lease owner ID. */
  leaseOwner: string;
  timeoutMs?: number;
  log?: (fields: Record<string, string | number>) => void;
}

export type RunStatus =
  | "disabled" // configuration missing or invalid
  | "idle" // nothing due
  | "auth_wait" // authorization needs attention; Google is retried every AUTH_RETRY_SECONDS
  | "done" // queue drained or per-run limit reached
  | "stopped" // stopped after a temporary error or rate limit
  | "config_error" // credentials/scope/account problem (no attempts consumed)
  | "storage_error"; // D1 failed; leases expire and are recovered

export interface DeliveryReport {
  status: RunStatus;
  sent: number;
  retried: number;
  failed: number;
  released: number;
  detail?: string;
  health?: DeliveryHealth;
}

/** Reads a stored canonical payload back into a validated event. */
export function parseStoredPayload(payload: string): NotificationEvent | null {
  let o: unknown;
  try {
    o = JSON.parse(payload);
  } catch {
    return null;
  }
  if (typeof o !== "object" || o === null || Array.isArray(o)) return null;
  const v = validateRequest({ ...(o as object), schema_version: 1, attempt: 1 });
  return v.ok ? v.value.event : null;
}

async function deliver(env: Env, deps: DeliveryDeps, report: DeliveryReport): Promise<RunEnding> {
  const log = deps.log ?? (() => undefined);
  const cfg = loadConfig(env as unknown as Record<string, unknown>);
  if (!cfg.ok) {
    report.status = "disabled";
    report.detail = cfg.problems.join(", ");
    log({ evt: "delivery_config_error", level: "warn", detail: report.detail });
    return { kind: "disabled", error: "config_incomplete" };
  }
  const db = env.DB;
  try {
    if (!(await hasDue(db, deps.now()))) {
      report.status = "idle"; // no Google traffic when nothing is due
      return { kind: "none" };
    }
    const state = await readState(db);
    if (
      state !== null &&
      state.auth_attention === 1 &&
      state.last_error_at !== null &&
      deps.now() - state.last_error_at < AUTH_RETRY_SECONDS &&
      !(state.last_error ?? "").startsWith("config_")
    ) {
      // Invalid credentials: do not hammer Google every 5 minutes; events stay PENDING.
      report.status = "auth_wait";
      report.detail = state.last_error ?? "";
      return { kind: "none" };
    }
  } catch {
    report.status = "storage_error";
    return { kind: "none" };
  }

  const gmail = new GmailClient(cfg.config, deps.fetch, deps.timeoutMs);
  /** Last message-level rejection in this run (FAILED or unclear); the run goes on. */
  let rejection: string | null = null;
  const refreshed = await gmail.refresh(); // before claiming anything
  if (refreshed.kind !== "ok") {
    report.status = refreshed.kind === "config" ? "config_error" : "stopped";
    report.detail = refreshed.detail;
    log({ evt: "delivery_auth_error", level: "warn", detail: refreshed.detail });
    return refreshed.kind === "config"
      ? { kind: "auth", error: refreshed.detail }
      : { kind: "temporary", error: refreshed.detail };
  }

  for (let i = 0; i < MAX_SENDS_PER_RUN; i++) {
    let claimed: ClaimedEvent | null;
    try {
      claimed = await claimNext(db, deps.leaseOwner, deps.now(), LEASE_SECONDS);
    } catch {
      report.status = "storage_error";
      return { kind: "none" };
    }
    if (claimed === null) break;
    const id = claimed.event_id;
    const event = parseStoredPayload(claimed.payload);
    try {
      if (event === null) {
        await markFailed(db, id, deps.leaseOwner, "invalid_stored_payload");
        report.failed++;
        rejection = "invalid_stored_payload";
        log({
          evt: "delivery",
          level: "warn",
          outcome: "failed",
          event_id: id,
          detail: "invalid_stored_payload",
        });
        continue;
      }
      const email = formatEmail(event, claimed.received_at);
      const raw = rawMessage({
        fromName: SENDER_NAME,
        from: cfg.config.sender,
        to: cfg.config.recipient,
        subject: email.subject,
        body: email.body,
        messageId: messageIdFor(id),
        eventId: id,
        date: new Date(deps.now() * 1000),
      });
      const out = await gmail.send(raw);
      const attempts = claimed.delivery_attempts;
      switch (out.kind) {
        case "sent": {
          const recorded = await markSent(db, id, deps.leaseOwner, deps.now(), out.messageId);
          report.sent++;
          // Not recorded = the lease was lost: the event may be sent again (at-least-once).
          log({
            evt: "delivery",
            outcome: recorded ? "sent" : "sent_lease_lost",
            event_id: id,
            attempts,
          });
          continue;
        }
        case "permanent":
          // Only this message is non-recoverable; continue with the other events.
          await markFailed(db, id, deps.leaseOwner, out.detail);
          report.failed++;
          report.detail = out.detail;
          rejection = out.detail;
          log({
            evt: "delivery",
            level: "warn",
            outcome: "failed",
            event_id: id,
            detail: out.detail,
          });
          continue;
        case "unclear": {
          // Ambiguous 400: never FAILED. Keep PENDING with backoff (the 6-hour cap bounds
          // the traffic), flag for a human, and continue with the other events.
          const wait = backoffSeconds(attempts);
          await markRetry(db, id, deps.leaseOwner, deps.now() + wait, out.detail);
          report.retried++;
          report.detail = out.detail;
          rejection = out.detail;
          log({
            evt: "delivery",
            level: "warn",
            outcome: "held",
            event_id: id,
            attempts,
            wait_s: wait,
            detail: out.detail,
          });
          continue;
        }
        case "config":
          await releaseUncounted(db, id, deps.leaseOwner, out.detail);
          report.released++;
          report.status = "config_error";
          report.detail = out.detail;
          log({ evt: "delivery_auth_error", level: "warn", event_id: id, detail: out.detail });
          return { kind: "auth", error: out.detail };
        case "rate_limited":
        case "transient": {
          // No attempt limit: retried until delivered (backoff capped at 6 hours).
          const wait = Math.max(
            backoffSeconds(attempts),
            out.kind === "rate_limited" ? (out.retryAfterS ?? 0) : 0,
          );
          await markRetry(db, id, deps.leaseOwner, deps.now() + wait, out.detail);
          report.retried++;
          report.status = "stopped";
          report.detail = out.detail;
          log({
            evt: "delivery",
            outcome: "retry",
            event_id: id,
            attempts,
            wait_s: wait,
            detail: out.detail,
          });
          return { kind: "temporary", error: out.detail };
        }
      }
    } catch {
      // D1 failed after the claim: the lease expires and the event is recovered later.
      report.status = "storage_error";
      return { kind: "none" };
    }
  }
  return rejection === null ? { kind: "ok" } : { kind: "rejected", error: rejection };
}

export async function runDelivery(env: Env, deps: DeliveryDeps): Promise<DeliveryReport> {
  const log = deps.log ?? (() => undefined);
  const report: DeliveryReport = { status: "done", sent: 0, retried: 0, failed: 0, released: 0 };
  const ending = await deliver(env, deps, report);
  try {
    await recordRun(env.DB, deps.now(), report.sent, ending);
    const health = await readHealth(env.DB, deps.now());
    report.health = health;
    log({
      evt: "delivery_health",
      level: health.attention ? "warn" : "info",
      status: report.status,
      pending: health.pending,
      failed: health.failed,
      unclear_pending: health.unclear_pending,
      oldest_pending_age_s: health.oldest_pending_age_s ?? -1,
      consecutive_failures: health.consecutive_failures,
      auth_attention: health.auth_attention ? 1 : 0,
      last_error: health.last_error ?? "",
      last_error_at: health.last_error_at ?? 0,
      last_success_at: health.last_success_at ?? 0,
    });
  } catch {
    log({ evt: "delivery_health_unavailable", level: "warn" });
  }
  return report;
}
