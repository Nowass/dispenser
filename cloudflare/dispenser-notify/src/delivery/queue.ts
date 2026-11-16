/**
 * Durable delivery queue on the notifications table (M1.6b).
 *
 * Claims are leases: one atomic UPDATE ... RETURNING takes the next due event, records the
 * owner (a random ID per delivery run), the lease expiry and counts the started attempt.
 * D1 executes each statement atomically, so two concurrent runs can never claim the same
 * event. Every later update is fenced by "lease_owner = ?" and only touches the delivery
 * columns; event_id, payload and payload_hash are never written here.
 */

export interface ClaimedEvent {
  event_id: string;
  payload: string;
  received_at: string | null;
  delivery_attempts: number;
}

const DUE =
  "delivery_status = 'PENDING' " +
  "AND (next_attempt_at IS NULL OR next_attempt_at <= ?1) " +
  "AND (lease_expires_at IS NULL OR lease_expires_at <= ?1)";

export async function hasDue(db: D1Database, now: number): Promise<boolean> {
  const row = await db
    .prepare(`SELECT 1 AS due FROM notifications WHERE ${DUE} LIMIT 1`)
    .bind(now)
    .first();
  return row !== null;
}

/** Atomically leases the next due event: critical first, then oldest (arrival order). */
export async function claimNext(
  db: D1Database,
  owner: string,
  now: number,
  leaseSeconds: number,
): Promise<ClaimedEvent | null> {
  const res = await db
    .prepare(
      "UPDATE notifications SET lease_owner = ?2, lease_expires_at = ?3, " +
        "delivery_attempts = delivery_attempts + 1 " +
        "WHERE event_id = (SELECT event_id FROM notifications WHERE " +
        DUE +
        " ORDER BY CASE severity WHEN 'CRITICAL' THEN 0 ELSE 1 END, received_at, rowid LIMIT 1) " +
        "RETURNING event_id, payload, received_at, delivery_attempts",
    )
    .bind(now, owner, now + leaseSeconds)
    .all<ClaimedEvent>();
  return res.results[0] ?? null;
}

/** D1 CURRENT_TIMESTAMP format, UTC. */
export function d1Timestamp(unixSeconds: number): string {
  return new Date(unixSeconds * 1000).toISOString().slice(0, 19).replace("T", " ");
}

async function fenced(db: D1Database, sql: string, args: unknown[]): Promise<boolean> {
  const res = await db
    .prepare(sql)
    .bind(...args)
    .run();
  return res.meta.changes === 1;
}

const OWNED = "WHERE event_id = ?1 AND lease_owner = ?2 AND delivery_status = 'PENDING'";

/** Returns false if the lease was lost (another run took over after expiry). */
export function markSent(
  db: D1Database,
  eventId: string,
  owner: string,
  now: number,
  providerMessageId: string | null,
): Promise<boolean> {
  return fenced(
    db,
    "UPDATE notifications SET delivery_status = 'SENT', delivered_at = ?3, " +
      "provider_message_id = ?4, last_error = NULL, next_attempt_at = NULL, " +
      `lease_owner = NULL, lease_expires_at = NULL ${OWNED}`,
    [eventId, owner, d1Timestamp(now), providerMessageId],
  );
}

export function markRetry(
  db: D1Database,
  eventId: string,
  owner: string,
  nextAttemptAt: number,
  error: string,
): Promise<boolean> {
  return fenced(
    db,
    "UPDATE notifications SET next_attempt_at = ?3, last_error = ?4, " +
      `lease_owner = NULL, lease_expires_at = NULL ${OWNED}`,
    [eventId, owner, nextAttemptAt, error],
  );
}

export function markFailed(
  db: D1Database,
  eventId: string,
  owner: string,
  error: string,
): Promise<boolean> {
  return fenced(
    db,
    "UPDATE notifications SET delivery_status = 'FAILED', last_error = ?3, " +
      `next_attempt_at = NULL, lease_owner = NULL, lease_expires_at = NULL ${OWNED}`,
    [eventId, owner, error],
  );
}

/**
 * Releases the lease without counting the attempt (configuration or credential problems
 * are not the event's fault and must not exhaust its attempts).
 */
export function releaseUncounted(
  db: D1Database,
  eventId: string,
  owner: string,
  error: string,
): Promise<boolean> {
  return fenced(
    db,
    "UPDATE notifications SET delivery_attempts = MAX(delivery_attempts - 1, 0), " +
      `last_error = ?3, lease_owner = NULL, lease_expires_at = NULL ${OWNED}`,
    [eventId, owner, error],
  );
}
