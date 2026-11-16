/**
 * Persistent delivery state and health (M1.6b diagnostics). One row in `delivery_state`
 * (migration 0002) keeps what a single run cannot know: consecutive failed runs, the last
 * Gmail/OAuth error, the last success and whether Google authorization needs attention.
 * Nothing here is reachable over HTTP; it is visible in the structured `delivery_health`
 * log line of every run and through `scripts/diagnostics.sql` (wrangler d1 execute).
 */

/**
 * Attention: any FAILED event (stays a problem until an administrator acts), any PENDING
 * event held by an unclear Gmail 400, a pending event older than this, or this many failed
 * runs in a row.
 */
export const PENDING_AGE_ATTENTION_SECONDS = 3600;
export const CONSECUTIVE_FAILURES_ATTENTION = 3;

export interface DeliveryState {
  consecutive_failures: number;
  last_error: string | null;
  last_error_at: number | null;
  last_success_at: number | null;
  auth_attention: number;
}

export interface DeliveryHealth {
  pending: number;
  failed: number;
  /** PENDING events whose last attempt got an unclear Gmail 400 (needs a human look). */
  unclear_pending: number;
  /** Seconds since the oldest PENDING event was received; null when none is pending. */
  oldest_pending_age_s: number | null;
  consecutive_failures: number;
  last_error: string | null;
  last_error_at: number | null;
  last_success_at: number | null;
  /** Google authorization (or the delivery configuration) needs an administrator. */
  auth_attention: boolean;
  /**
   * Anything above needs a look: authorization, any FAILED event, unclear rejections,
   * repeated failures or an old backlog.
   */
  attention: boolean;
}

export type RunEnding =
  | { kind: "none" }
  | { kind: "ok" }
  | { kind: "temporary"; error: string }
  /** Message-level rejections (FAILED or unclear); the service itself answered. */
  | { kind: "rejected"; error: string }
  | { kind: "auth"; error: string }
  | { kind: "disabled"; error: string };

export async function readState(db: D1Database): Promise<DeliveryState | null> {
  return db
    .prepare(
      "SELECT consecutive_failures, last_error, last_error_at, last_success_at, auth_attention " +
        "FROM delivery_state WHERE id = 1",
    )
    .first<DeliveryState>();
}

/** Updates the state row after a run (atomic single statements; concurrent runs are safe). */
export async function recordRun(
  db: D1Database,
  now: number,
  sent: number,
  ending: RunEnding,
): Promise<void> {
  const success = sent > 0 ? now : null;
  switch (ending.kind) {
    case "none":
      if (success === null) return;
      break;
    case "ok":
      await db
        .prepare(
          "UPDATE delivery_state SET consecutive_failures = 0, auth_attention = 0, " +
            "last_success_at = COALESCE(?1, last_success_at), updated_at = ?2 WHERE id = 1",
        )
        .bind(success, now)
        .run();
      return;
    case "rejected":
      // The service works (it answered); only messages were rejected. Their visibility
      // comes from the FAILED / unclear counts, not from the run counter.
      await db
        .prepare(
          "UPDATE delivery_state SET consecutive_failures = 0, auth_attention = 0, " +
            "last_error = ?1, last_error_at = ?2, " +
            "last_success_at = COALESCE(?3, last_success_at), updated_at = ?2 WHERE id = 1",
        )
        .bind(ending.error, now, success)
        .run();
      return;
    case "temporary":
    case "auth":
      await db
        .prepare(
          "UPDATE delivery_state SET " +
            "consecutive_failures = CASE WHEN ?3 IS NULL THEN consecutive_failures + 1 ELSE 1 END, " +
            "last_error = ?1, last_error_at = ?2, " +
            "auth_attention = CASE WHEN ?4 = 1 THEN 1 WHEN ?3 IS NULL THEN auth_attention ELSE 0 END, " +
            "last_success_at = COALESCE(?3, last_success_at), updated_at = ?2 WHERE id = 1",
        )
        .bind(ending.error, now, success, ending.kind === "auth" ? 1 : 0)
        .run();
      return;
    case "disabled":
      // Written only on change, so a disabled Worker does not write every 5 minutes.
      await db
        .prepare(
          "UPDATE delivery_state SET auth_attention = 1, last_error = ?1, last_error_at = ?2, " +
            "updated_at = ?2 WHERE id = 1 AND (auth_attention = 0 OR last_error IS NOT ?1)",
        )
        .bind(ending.error, now)
        .run();
      return;
  }
  // "none" with sends (cannot happen today, kept total): record the success only.
  await db
    .prepare("UPDATE delivery_state SET last_success_at = ?1, updated_at = ?1 WHERE id = 1")
    .bind(now)
    .run();
}

function d1Seconds(text: string | null): number | null {
  if (text === null || !/^\d{4}-\d{2}-\d{2} \d{2}:\d{2}:\d{2}$/.test(text)) return null;
  const t = Date.parse(`${text.replace(" ", "T")}Z`);
  return Number.isNaN(t) ? null : Math.floor(t / 1000);
}

export async function readHealth(db: D1Database, now: number): Promise<DeliveryHealth> {
  const row = await db
    .prepare(
      "SELECT " +
        "(SELECT COUNT(*) FROM notifications WHERE delivery_status = 'PENDING') AS pending, " +
        "(SELECT COUNT(*) FROM notifications WHERE delivery_status = 'FAILED') AS failed, " +
        "(SELECT COUNT(*) FROM notifications WHERE delivery_status = 'PENDING' " +
        "AND last_error GLOB 'gmail_400_unclear_*') AS unclear_pending, " +
        "(SELECT MIN(received_at) FROM notifications WHERE delivery_status = 'PENDING') AS oldest, " +
        "s.consecutive_failures, s.last_error, s.last_error_at, s.last_success_at, s.auth_attention " +
        "FROM delivery_state s WHERE s.id = 1",
    )
    .first<
      DeliveryState & {
        pending: number;
        failed: number;
        unclear_pending: number;
        oldest: string | null;
      }
    >();
  if (row === null) throw new Error("delivery_state row missing");
  const oldest = d1Seconds(row.oldest);
  const age = oldest === null ? null : Math.max(0, now - oldest);
  const auth = row.auth_attention === 1;
  return {
    pending: row.pending,
    failed: row.failed,
    unclear_pending: row.unclear_pending,
    oldest_pending_age_s: age,
    consecutive_failures: row.consecutive_failures,
    last_error: row.last_error,
    last_error_at: row.last_error_at,
    last_success_at: row.last_success_at,
    auth_attention: auth,
    attention:
      auth ||
      row.failed > 0 ||
      row.unclear_pending > 0 ||
      row.consecutive_failures >= CONSECUTIVE_FAILURES_ATTENTION ||
      (age !== null && age >= PENDING_AGE_ATTENTION_SECONDS),
  };
}
