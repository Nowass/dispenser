-- Dispenser V2 delivery diagnostics (M1.6b). READ-ONLY: SELECT statements only.
-- Production (authenticated through your Cloudflare login, no public endpoint):
--   npx wrangler d1 execute dispenser-notifications --remote --json --command="$(cat scripts/diagnostics.sql)"
-- Local: replace --remote with --local.

-- 1. Health summary.
SELECT
    (SELECT COUNT(*) FROM notifications WHERE delivery_status = 'PENDING') AS pending,
    (SELECT COUNT(*) FROM notifications WHERE delivery_status = 'FAILED') AS failed,
    (SELECT COUNT(*) FROM notifications
       WHERE delivery_status = 'PENDING' AND last_error GLOB 'gmail_400_unclear_*') AS unclear_pending,
    (SELECT CAST(strftime('%s', 'now') AS INTEGER) - CAST(strftime('%s', MIN(received_at)) AS INTEGER)
       FROM notifications WHERE delivery_status = 'PENDING') AS oldest_pending_age_s,
    s.consecutive_failures,
    s.auth_attention,
    s.last_error,
    datetime(s.last_error_at, 'unixepoch') AS last_error_at_utc,
    datetime(s.last_success_at, 'unixepoch') AS last_success_at_utc
FROM delivery_state s
WHERE s.id = 1;

-- 2. Undelivered notifications (oldest first).
SELECT
    event_id,
    event_type,
    severity,
    delivery_status,
    delivery_attempts,
    last_error,
    received_at AS received_at_utc,
    datetime(next_attempt_at, 'unixepoch') AS next_attempt_utc,
    datetime(lease_expires_at, 'unixepoch') AS lease_expires_utc
FROM notifications
WHERE delivery_status <> 'SENT'
ORDER BY received_at, rowid
LIMIT 50;
