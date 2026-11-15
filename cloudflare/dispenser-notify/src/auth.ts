/**
 * Device authentication: `Authorization: Bearer <DEVICE_API_TOKEN>`.
 *
 * Fails closed: a missing or too short configured secret disables ingestion (503) instead
 * of accepting anything. The comparison hashes both values and compares the digests in
 * constant time, so neither content nor length leaks through timing.
 */

/** A configured token shorter than this is treated as a configuration error. */
export const MIN_TOKEN_LENGTH = 32;
const MAX_TOKEN_LENGTH = 512;
const BEARER_RE = /^Bearer ([\x21-\x7e]+)$/;

export type AuthResult = "ok" | "unauthorized" | "misconfigured";

async function sha256(s: string): Promise<ArrayBuffer> {
  return crypto.subtle.digest("SHA-256", new TextEncoder().encode(s));
}

export function secretConfigured(secret: string | undefined): secret is string {
  return (
    typeof secret === "string" &&
    secret.length >= MIN_TOKEN_LENGTH &&
    secret.length <= MAX_TOKEN_LENGTH
  );
}

export async function authenticate(
  header: string | null,
  secret: string | undefined,
): Promise<AuthResult> {
  if (!secretConfigured(secret)) {
    return "misconfigured";
  }
  if (header === null || header.length > MAX_TOKEN_LENGTH + 7) {
    return "unauthorized";
  }
  const m = BEARER_RE.exec(header);
  if (m === null) {
    return "unauthorized";
  }
  const [given, expected] = await Promise.all([sha256(m[1] as string), sha256(secret)]);
  return crypto.subtle.timingSafeEqual(given, expected) ? "ok" : "unauthorized";
}
