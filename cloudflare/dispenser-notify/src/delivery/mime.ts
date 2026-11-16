/**
 * RFC 5322 / MIME message construction for the Gmail API "raw" field (M1.6b).
 * Plain text, UTF-8, base64 transfer encoding; the subject and display name use RFC 2047
 * encoded words, so Czech text survives any mail path. No header value contains CR/LF.
 */

export interface MailMessage {
  fromName: string;
  from: string;
  to: string;
  subject: string;
  body: string;
  /** Stable Message-ID derived from the event ID (Gmail is not guaranteed to deduplicate). */
  messageId: string;
  eventId: string;
  date: Date;
}

const encoder = new TextEncoder();

function base64(bytes: Uint8Array): string {
  let s = "";
  for (let i = 0; i < bytes.length; i += 0x8000) {
    s += String.fromCharCode(...bytes.subarray(i, i + 0x8000));
  }
  return btoa(s);
}

export function base64Url(bytes: Uint8Array): string {
  return base64(bytes).replace(/\+/g, "-").replace(/\//g, "_").replace(/=+$/, "");
}

function stripLineBreaks(s: string): string {
  return s.replace(/[\r\n]+/g, " ");
}

/**
 * RFC 2047 "B" encoded words, each at most 75 characters, never splitting a UTF-8
 * character; pure printable ASCII is returned unchanged.
 */
export function encodeHeaderWords(text: string): string {
  const clean = stripLineBreaks(text);
  if (/^[\x20-\x7e]*$/.test(clean) && !clean.includes("=?")) {
    return clean;
  }
  const words: string[] = [];
  let chunk = "";
  for (const ch of clean) {
    // 45 bytes -> 60 base64 characters + 12 for "=?UTF-8?B?" and "?=" = 72 <= 75.
    if (encoder.encode(chunk + ch).length > 45) {
      words.push(chunk);
      chunk = "";
    }
    chunk += ch;
  }
  if (chunk !== "") words.push(chunk);
  return words.map((w) => `=?UTF-8?B?${base64(encoder.encode(w))}?=`).join("\r\n ");
}

function wrap76(s: string): string {
  return s.replace(/.{1,76}/g, "$&\r\n");
}

/** RFC 5322 date in UTC, e.g. "Fri, 20 Nov 2026 07:00:00 +0000". */
export function rfc5322Date(d: Date): string {
  return d.toUTCString().replace(/GMT$/, "+0000");
}

export function buildMessage(m: MailMessage): string {
  const headers = [
    `From: ${encodeHeaderWords(m.fromName)} <${m.from}>`,
    `To: <${m.to}>`,
    `Subject: ${encodeHeaderWords(m.subject)}`,
    `Date: ${rfc5322Date(m.date)}`,
    `Message-ID: ${m.messageId}`,
    `X-Dispenser-Event-Id: ${stripLineBreaks(m.eventId)}`,
    "Auto-Submitted: auto-generated",
    "MIME-Version: 1.0",
    "Content-Type: text/plain; charset=UTF-8",
    "Content-Transfer-Encoding: base64",
  ];
  const body = m.body.replace(/\r?\n/g, "\r\n");
  return `${headers.join("\r\n")}\r\n\r\n${wrap76(base64(encoder.encode(body)))}`;
}

/** The Gmail API "raw" value: the whole message, base64url without padding. */
export function rawMessage(m: MailMessage): string {
  return base64Url(encoder.encode(buildMessage(m)));
}

/** Stable per-event Message-ID; reminders have their own event IDs, so their own IDs. */
export function messageIdFor(eventId: string): string {
  return `<${eventId}@dispenser-notify.invalid>`;
}
