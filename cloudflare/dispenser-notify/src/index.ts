/**
 * Dispenser V2 notification backend.
 * - fetch (M1.6a): accepts firmware notification events and stores them durably in D1 as
 *   PENDING. A success response means "durably accepted, delivery is now the backend's
 *   responsibility", never "delivered".
 * - scheduled (M1.6b): the Cron Trigger sends PENDING events by e-mail (Gmail API). There
 *   is no HTTP endpoint for delivery or administration.
 */
import { authenticate } from "./auth";
import { runDelivery } from "./delivery/run";
import { MAX_BODY_BYTES, canonicalPayload, payloadHash, validateRequest } from "./protocol";
import { StorageError, storeEvent } from "./store";

const EVENTS_PATH = "/api/v1/events";
const RETRY_AFTER_S = "60";

function json(status: number, body: Record<string, unknown>, headers?: HeadersInit): Response {
  const h = new Headers(headers);
  h.set("Content-Type", "application/json; charset=utf-8");
  h.set("Cache-Control", "no-store");
  h.set("X-Content-Type-Options", "nosniff");
  return new Response(JSON.stringify(body), { status, headers: h });
}

/** Structured log line without credentials or payload values. */
function log(fields: Record<string, string | number>): void {
  const line = JSON.stringify({ svc: "dispenser-notify", ...fields });
  if (fields.level === "warn") {
    console.warn(line); // shown as a warning in Workers Logs (filterable, alertable)
  } else {
    console.log(line);
  }
}

const retryLater = (): Response => json(503, { status: "retry" }, { "Retry-After": RETRY_AFTER_S });

/** Reads at most MAX_BODY_BYTES; null if the body is larger. */
async function readBounded(request: Request): Promise<Uint8Array | null> {
  const declared = request.headers.get("Content-Length");
  if (declared !== null && (!/^\d+$/.test(declared) || Number(declared) > MAX_BODY_BYTES)) {
    return null;
  }
  if (request.body === null) {
    return new Uint8Array(0);
  }
  const reader = request.body.getReader();
  const chunks: Uint8Array[] = [];
  let total = 0;
  for (;;) {
    const { done, value } = await reader.read();
    if (done) break;
    total += value.byteLength;
    if (total > MAX_BODY_BYTES) {
      await reader.cancel();
      return null;
    }
    chunks.push(value);
  }
  const out = new Uint8Array(total);
  let off = 0;
  for (const c of chunks) {
    out.set(c, off);
    off += c.byteLength;
  }
  return out;
}

async function ingest(request: Request, env: Env): Promise<Response> {
  const auth = await authenticate(request.headers.get("Authorization"), env.DEVICE_API_TOKEN);
  if (auth === "misconfigured") {
    log({ evt: "config_error", detail: "DEVICE_API_TOKEN missing or too short" });
    return retryLater(); // fail closed
  }
  if (auth !== "ok") {
    return json(
      401,
      { status: "unauthorized" },
      { "WWW-Authenticate": 'Bearer realm="dispenser"' },
    );
  }

  const contentType = (request.headers.get("Content-Type") ?? "").toLowerCase();
  if (!/^application\/json\s*(;\s*charset=utf-8\s*)?$/.test(contentType)) {
    return json(415, { status: "invalid", error: "unsupported_media_type" });
  }
  const raw = await readBounded(request);
  if (raw === null) {
    return json(413, { status: "invalid", error: "payload_too_large" });
  }
  let body: unknown;
  try {
    body = JSON.parse(new TextDecoder("utf-8", { fatal: true, ignoreBOM: false }).decode(raw));
  } catch {
    return json(400, { status: "invalid", error: "malformed_json" });
  }
  const parsed = validateRequest(body);
  if (!parsed.ok) {
    return json(400, { status: "invalid", ...parsed.reason });
  }
  const { event, attempt } = parsed.value;

  const payload = canonicalPayload(event);
  const hash = await payloadHash(payload);
  let outcome;
  try {
    outcome = await storeEvent(env.DB, event, payload, hash);
  } catch (e) {
    const detail = e instanceof StorageError ? e.message : "unexpected";
    log({ evt: "storage_error", event_id: event.event_id, detail });
    return retryLater();
  }
  log({ evt: "ingest", outcome, event_id: event.event_id, type: event.type, attempt });
  switch (outcome) {
    case "accepted":
      return json(201, { status: "accepted", event_id: event.event_id });
    case "duplicate":
      return json(200, { status: "duplicate", event_id: event.event_id });
    case "conflict":
      return json(409, { status: "conflict", event_id: event.event_id });
  }
}

export async function handle(request: Request, env: Env): Promise<Response> {
  const { pathname } = new URL(request.url);
  if (pathname === "/health") {
    if (request.method !== "GET" && request.method !== "HEAD") {
      return json(405, { status: "invalid", error: "method_not_allowed" }, { Allow: "GET, HEAD" });
    }
    return json(200, { status: "ok", service: "dispenser-notify", protocol: 1 });
  }
  if (pathname === EVENTS_PATH) {
    if (request.method !== "POST") {
      return json(405, { status: "invalid", error: "method_not_allowed" }, { Allow: "POST" });
    }
    return ingest(request, env);
  }
  return json(404, { status: "invalid", error: "not_found" });
}

export default {
  async fetch(request: Request, env: Env): Promise<Response> {
    try {
      return await handle(request, env);
    } catch {
      log({ evt: "internal_error" });
      return retryLater(); // never a false success; the device retries later
    }
  },

  async scheduled(_controller: ScheduledController, env: Env): Promise<void> {
    const report = await runDelivery(env, {
      fetch: (input, init) => fetch(input, init),
      now: () => Math.floor(Date.now() / 1000),
      leaseOwner: crypto.randomUUID(),
      log,
    });
    const { detail, health: _health, ...counts } = report;
    log({ evt: "delivery_run", ...counts, ...(detail === undefined ? {} : { detail }) });
  },
} satisfies ExportedHandler<Env>;
