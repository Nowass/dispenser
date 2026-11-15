/**
 * Durable, idempotent storage of accepted events in the existing `notifications` table.
 *
 * Uniqueness of event_id is enforced by the database (PRIMARY KEY). The insert uses
 * ON CONFLICT DO NOTHING, so concurrent submissions of the same ID are serialized by D1:
 * exactly one inserts, every other one sees the stored row and is classified by comparing
 * payload hashes. An existing row is never modified here.
 */
import type { NotificationEvent } from "./protocol";

export type StoreOutcome = "accepted" | "duplicate" | "conflict";

/** Any storage failure; the caller must answer with a retryable error, never an ACK. */
export class StorageError extends Error {
  override readonly name = "StorageError";
}

const INSERT_SQL =
  "INSERT INTO notifications " +
  "(event_id, device_id, event_type, severity, payload, payload_hash, delivery_status) " +
  "VALUES (?1, ?2, ?3, ?4, ?5, ?6, 'PENDING') " +
  "ON CONFLICT(event_id) DO NOTHING";

const SELECT_HASH_SQL = "SELECT payload_hash FROM notifications WHERE event_id = ?1";

export async function storeEvent(
  db: D1Database,
  event: NotificationEvent,
  payload: string,
  hash: string,
): Promise<StoreOutcome> {
  let inserted: boolean;
  try {
    const res = await db
      .prepare(INSERT_SQL)
      .bind(event.event_id, event.device_id, event.type, event.severity, payload, hash)
      .run();
    if (!res.success) {
      throw new StorageError("insert not successful");
    }
    inserted = res.meta.changes === 1;
  } catch (e) {
    throw e instanceof StorageError ? e : new StorageError("insert failed");
  }
  if (inserted) {
    return "accepted"; // committed by D1 before run() resolved
  }
  let row: { payload_hash: unknown } | null;
  try {
    row = await db.prepare(SELECT_HASH_SQL).bind(event.event_id).first<{ payload_hash: unknown }>();
  } catch {
    throw new StorageError("lookup failed");
  }
  if (row === null || typeof row.payload_hash !== "string") {
    // Conflict reported but no readable row: state unknown, do not acknowledge.
    throw new StorageError("existing row not readable");
  }
  return row.payload_hash === hash ? "duplicate" : "conflict";
}
