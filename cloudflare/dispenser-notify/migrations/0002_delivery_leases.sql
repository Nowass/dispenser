-- Migration 0002 (M1.6b): e-mail delivery scheduling, per-event leases and delivery state.
-- Purely additive: four new NULLable columns and one new table. Existing rows, the
-- ingestion columns, the delivery_status CHECK constraint and the indexes are unchanged;
-- ingestion keeps writing the same six columns and leaves these NULL.
--
-- next_attempt_at      unix seconds; NULL = due now. Set after a failed attempt (backoff).
-- lease_owner          random ID of the delivery run that currently owns the event.
-- lease_expires_at     unix seconds; an expired lease may be taken over (crash recovery).
-- provider_message_id  Gmail message ID after a successful send (diagnostics).
ALTER TABLE notifications ADD COLUMN next_attempt_at INTEGER;
ALTER TABLE notifications ADD COLUMN lease_owner TEXT;
ALTER TABLE notifications ADD COLUMN lease_expires_at INTEGER;
ALTER TABLE notifications ADD COLUMN provider_message_id TEXT;

-- Delivery state for diagnostics (one row): consecutive failed runs, the last Gmail/OAuth
-- error, the last success and whether Google authorization needs attention. New table,
-- nothing existing changes.
CREATE TABLE IF NOT EXISTS delivery_state (
    id INTEGER PRIMARY KEY CHECK (id = 1),
    consecutive_failures INTEGER NOT NULL DEFAULT 0,
    last_error TEXT,
    last_error_at INTEGER,
    last_success_at INTEGER,
    auth_attention INTEGER NOT NULL DEFAULT 0 CHECK (auth_attention IN (0, 1)),
    updated_at INTEGER
);
INSERT OR IGNORE INTO delivery_state (id) VALUES (1);
