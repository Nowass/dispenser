/**
 * Wire protocol v1 between the Dispenser V2 firmware and the notification backend.
 *
 * The event model mirrors the firmware notification engine (M1.5, see
 * firmware/components/dispenser_domain/include/dsp_notify_state.h). Validation applies the
 * same structural rules as the firmware's dsp_notify_state_is_valid(), so anything the
 * firmware can queue is accepted and nothing else is. See PROTOCOL.md for the contract.
 */

export const SCHEMA_VERSION = 1;

/** Upper bound for a request body; a v1 event is about 300 bytes. */
export const MAX_BODY_BYTES = 4096;

/** Firmware time bounds (DSP_TIME_PLAUSIBLE_MIN, DSP_UTC_MAX). */
export const CREATED_AT_MIN = 1735689600; // 2025-01-01T00:00:00Z
export const CREATED_AT_MAX = 4133980799; // 2100-12-31T23:59:59Z

const U32_MAX = 4294967295;
const U16_MAX = 65535;
const U8_MAX = 255;
/** The firmware never assigns sequence number UINT32_MAX (next_seq would overflow). */
const SEQ_MAX = U32_MAX - 1;

export type EventType =
  "RESERVOIR_LOW" | "RESERVOIR_BLOCKED" | "BATTERY_LOW" | "DOSING_ERROR" | "DEVICE_ERROR";

export type Severity = "WARNING" | "CRITICAL";

interface TypeRule {
  severity: Severity;
  /** Allowed codes; null means the type carries no code. */
  codes: readonly string[] | null;
  /** Codes of one-shot events (no reminders); every code if the whole type is one-shot. */
  oneShot: (code: string | null) => boolean;
}

/** dsp_note_type_t, fixed severities and DSP_DOSEERR_* / DSP_DEVERR_* codes. */
const TYPES: Record<EventType, TypeRule> = {
  RESERVOIR_LOW: { severity: "WARNING", codes: null, oneShot: () => false },
  RESERVOIR_BLOCKED: { severity: "CRITICAL", codes: null, oneShot: () => false },
  BATTERY_LOW: { severity: "WARNING", codes: null, oneShot: () => false },
  DOSING_ERROR: {
    severity: "CRITICAL",
    codes: ["START_FAILED", "PUMP_FAILED", "INTERRUPTED", "STORAGE", "BLOCKED"],
    oneShot: () => true,
  },
  DEVICE_ERROR: {
    severity: "CRITICAL",
    codes: ["STORAGE", "TIME_SYNC", "CLOCK", "QUEUE_OVERFLOW"],
    oneShot: (code) => code === "QUEUE_OVERFLOW",
  },
};

/** dsp-<device 12 hex>-<epoch 8 hex>-<seq>, exactly as dsp_notify_format_id() prints it. */
const EVENT_ID_RE = /^dsp-([0-9a-f]{12})-([0-9a-f]{8})-([1-9][0-9]{0,9})$/;
const DEVICE_ID_RE = /^[0-9a-f]{12}$/;

/** A validated event. Field order here is the canonical order (see canonicalPayload). */
export interface NotificationEvent {
  event_id: string;
  device_id: string;
  type: EventType;
  severity: Severity;
  code: string | null;
  reminder_no: number;
  created_at: number | null;
  uncertain: boolean;
  manual: boolean;
  value: number;
  value2: number;
}

export interface ParsedRequest {
  event: NotificationEvent;
  /** Delivery attempt number from the firmware; transport metadata, not part of the event. */
  attempt: number;
}

const FIELDS = [
  "schema_version",
  "event_id",
  "device_id",
  "type",
  "severity",
  "code",
  "reminder_no",
  "created_at",
  "uncertain",
  "manual",
  "value",
  "value2",
  "attempt",
] as const;

export type ValidationError =
  | { error: "unsupported_schema_version" }
  | { error: "invalid_field"; field: string }
  | { error: "unknown_field"; field: string }
  | { error: "missing_field"; field: string };

export type ValidationResult =
  { ok: true; value: ParsedRequest } | { ok: false; reason: ValidationError };

function isInt(v: unknown, min: number, max: number): v is number {
  return typeof v === "number" && Number.isInteger(v) && v >= min && v <= max;
}

function invalid(field: string): ValidationResult {
  return { ok: false, reason: { error: "invalid_field", field } };
}

/** Strict validation of a parsed JSON body against wire format v1. */
export function validateRequest(body: unknown): ValidationResult {
  if (typeof body !== "object" || body === null || Array.isArray(body)) {
    return invalid("body");
  }
  const o = body as Record<string, unknown>;
  for (const key of Object.keys(o)) {
    if (!(FIELDS as readonly string[]).includes(key)) {
      return { ok: false, reason: { error: "unknown_field", field: key.slice(0, 64) } };
    }
  }
  if (!("schema_version" in o)) {
    return { ok: false, reason: { error: "missing_field", field: "schema_version" } };
  }
  if (o.schema_version !== SCHEMA_VERSION) {
    return { ok: false, reason: { error: "unsupported_schema_version" } };
  }
  for (const key of FIELDS) {
    if (!(key in o)) {
      return { ok: false, reason: { error: "missing_field", field: key } };
    }
  }

  const eventId = o.event_id;
  if (typeof eventId !== "string") return invalid("event_id");
  const m = EVENT_ID_RE.exec(eventId);
  if (m === null) return invalid("event_id");
  const [, idDevice, idEpoch, idSeq] = m as unknown as [string, string, string, string];
  if (idEpoch === "00000000" || Number(idSeq) > SEQ_MAX) return invalid("event_id");

  const deviceId = o.device_id;
  if (typeof deviceId !== "string" || !DEVICE_ID_RE.test(deviceId)) return invalid("device_id");
  if (deviceId !== idDevice) return invalid("device_id");

  const type = o.type;
  if (typeof type !== "string" || !Object.hasOwn(TYPES, type)) return invalid("type");
  const rule = TYPES[type as EventType];

  if (o.severity !== rule.severity) return invalid("severity");

  const code = o.code;
  if (rule.codes === null) {
    if (code !== null) return invalid("code");
  } else if (typeof code !== "string" || !rule.codes.includes(code)) {
    return invalid("code");
  }
  const codeValue = code as string | null;

  if (!isInt(o.reminder_no, 0, U8_MAX)) return invalid("reminder_no");
  if (o.reminder_no > 0 && rule.oneShot(codeValue)) return invalid("reminder_no");

  const createdAt = o.created_at;
  if (createdAt !== null && !isInt(createdAt, CREATED_AT_MIN, CREATED_AT_MAX)) {
    return invalid("created_at");
  }

  if (typeof o.uncertain !== "boolean") return invalid("uncertain");
  if (typeof o.manual !== "boolean") return invalid("manual");
  if (o.manual && type !== "DOSING_ERROR") return invalid("manual");

  if (!isInt(o.value, 0, U32_MAX)) return invalid("value");
  if (!isInt(o.value2, 0, U32_MAX)) return invalid("value2");
  if (!isInt(o.attempt, 1, U16_MAX)) return invalid("attempt");

  return {
    ok: true,
    value: {
      event: {
        event_id: eventId,
        device_id: deviceId,
        type: type as EventType,
        severity: rule.severity,
        code: codeValue,
        reminder_no: o.reminder_no,
        created_at: createdAt as number | null,
        uncertain: o.uncertain,
        manual: o.manual,
        value: o.value,
        value2: o.value2,
      },
      attempt: o.attempt,
    },
  };
}

/**
 * Canonical payload: the validated event's fields in a fixed order, serialized as compact
 * JSON. Only integers, booleans, null and ASCII strings from closed sets occur, so the
 * serialization is unique: key order, whitespace and number spelling in the request
 * (e.g. 8000 vs 8.0e3) do not matter. Excludes schema_version and attempt (transport
 * metadata). This exact string is stored in notifications.payload.
 */
export function canonicalPayload(e: NotificationEvent): string {
  const ordered: NotificationEvent = {
    event_id: e.event_id,
    device_id: e.device_id,
    type: e.type,
    severity: e.severity,
    code: e.code,
    reminder_no: e.reminder_no,
    created_at: e.created_at,
    uncertain: e.uncertain,
    manual: e.manual,
    value: e.value,
    value2: e.value2,
  };
  return JSON.stringify(ordered);
}

/** payload_hash: lowercase hex SHA-256 of the UTF-8 bytes of the canonical payload. */
export async function payloadHash(canonical: string): Promise<string> {
  const digest = await crypto.subtle.digest("SHA-256", new TextEncoder().encode(canonical));
  return [...new Uint8Array(digest)].map((b) => b.toString(16).padStart(2, "0")).join("");
}
