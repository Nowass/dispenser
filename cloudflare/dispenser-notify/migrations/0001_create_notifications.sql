-- Migration 0001: the notifications table exactly as created manually in production
-- (verified read-only against the production database on 2026-11-15: columns, types,
-- NOT NULL, defaults, primary key, CHECK constraint and the delivery index).
-- IF NOT EXISTS makes every statement a no-op on the existing production database, so
-- applying it only records it in d1_migrations; it never drops, recreates or erases data.
-- event_id (PRIMARY KEY) is the idempotency key: uniqueness is enforced by the database.
CREATE TABLE IF NOT EXISTS notifications (
    event_id TEXT PRIMARY KEY,
    device_id TEXT NOT NULL,
    event_type TEXT NOT NULL,
    severity TEXT NOT NULL,
    payload TEXT NOT NULL,
    payload_hash TEXT NOT NULL,
    received_at TEXT NOT NULL DEFAULT CURRENT_TIMESTAMP,
    delivery_status TEXT NOT NULL DEFAULT 'PENDING'
        CHECK (delivery_status IN ('PENDING', 'SENT', 'FAILED')),
    delivery_attempts INTEGER NOT NULL DEFAULT 0,
    delivered_at TEXT,
    last_error TEXT
);

-- For the future e-mail sender: pending events in arrival order.
CREATE INDEX IF NOT EXISTS idx_notifications_delivery
    ON notifications(delivery_status, received_at);
