/**
 * Shared notification protocol v1 vectors (M1.8): ../../test-vectors/notification_protocol_v1.txt,
 * also consumed by the firmware host test test_proto_vectors.c. Expected values come from an
 * independent generator (PROTOCOL.md rules, Python json + hashlib), never from this code.
 *
 *  - encode:   the backend accepts every firmware body, derives exactly the vector's canonical
 *              payload and SHA-256, answers 201/200 with the exact acknowledgement bodies the
 *              firmware requires, and stores the canonical payload in D1;
 *  - request:  the backend answers each invalid request with exactly the vector's response
 *              (which the firmware classifies as listed in the same vector);
 *  - response: every backend-produced response in the vectors is produced exactly so.
 */
import { env } from "cloudflare:test";
import { exports } from "cloudflare:workers";
import { describe, expect, it } from "vitest";
import { canonicalPayload, payloadHash, validateRequest } from "../src/protocol";
import vectorText from "../../../test-vectors/notification_protocol_v1.txt?raw";

const TOKEN = "test-token-0123456789abcdef0123456789abcdef";
const BASE = "https://dispenser-notify.test";

interface Rec {
  kind: string;
  f: Record<string, string>;
}

function parseVectors(text: string): Rec[] {
  const out: Rec[] = [];
  for (const raw of text.split("\n")) {
    const line = raw.trim();
    if (line === "" || line.startsWith("#")) continue;
    const [kind, ...tokens] = line.split(" ");
    const f: Record<string, string> = {};
    for (const t of tokens) {
      const i = t.indexOf("=");
      if (i <= 0) throw new Error(`bad token ${t}`);
      f[t.slice(0, i)] = t.slice(i + 1);
    }
    out.push({ kind: kind as string, f });
  }
  return out;
}

const vectors = parseVectors(vectorText);
const of = (kind: string) => vectors.filter((v) => v.kind === kind);

async function post(
  body: string,
  opts: { token?: string | null; path?: string; contentType?: string } = {},
): Promise<Response> {
  const headers: Record<string, string> = {
    "Content-Type": opts.contentType ?? "application/json",
  };
  if (opts.token !== null) headers.Authorization = `Bearer ${opts.token ?? TOKEN}`;
  return exports.default.fetch(
    new Request(`${BASE}${opts.path ?? "/api/v1/events"}`, { method: "POST", headers, body }),
  );
}

async function removeRows(ids: string[]): Promise<void> {
  for (const id of ids) {
    await env.DB.prepare("DELETE FROM notifications WHERE event_id = ?1").bind(id).run();
  }
}

function withAttempt(body: string, attempt: number): string {
  const o = JSON.parse(body) as Record<string, unknown>;
  o.attempt = attempt;
  return JSON.stringify(o);
}

describe("shared protocol vectors", () => {
  it("contains every record kind", () => {
    expect(of("encode").length).toBeGreaterThanOrEqual(16);
    expect(of("noencode").length).toBeGreaterThanOrEqual(13);
    expect(of("request").length).toBeGreaterThanOrEqual(13);
    expect(of("response").length).toBeGreaterThanOrEqual(19);
  });

  it("encode: validator accepts the firmware body; canonical payload and hash match", async () => {
    for (const { f } of of("encode")) {
      const v = validateRequest(JSON.parse(f.body as string));
      expect(v.ok, f.name).toBe(true);
      if (!v.ok) continue;
      expect(v.value.attempt, f.name).toBe(Number(f.attempt));
      const canonical = canonicalPayload(v.value.event);
      expect(canonical, f.name).toBe(f.canonical);
      expect(await payloadHash(canonical), f.name).toBe(f.sha256);
    }
  });

  it("encode: 201 then 200 with the exact acknowledgement bodies; D1 holds the canonical payload", async () => {
    const encode = of("encode");
    const ids = encode.map(({ f }) => JSON.parse(f.body as string).event_id as string);
    await removeRows(ids);
    for (const [i, { f }] of encode.entries()) {
      const id = ids[i] as string;
      const first = await post(f.body as string);
      expect(first.status, f.name).toBe(201);
      expect(await first.text(), f.name).toBe(`{"status":"accepted","event_id":"${id}"}`);
      const row = await env.DB.prepare(
        "SELECT payload, payload_hash, delivery_status FROM notifications WHERE event_id = ?1",
      )
        .bind(id)
        .first<{ payload: string; payload_hash: string; delivery_status: string }>();
      expect(row?.payload, f.name).toBe(f.canonical);
      expect(row?.payload_hash, f.name).toBe(f.sha256);
      expect(row?.delivery_status, f.name).toBe("PENDING");
      // A retry (another attempt number, same frozen payload) is a duplicate.
      const attempt = Number(f.attempt);
      const retry = await post(withAttempt(f.body as string, attempt < 65535 ? attempt + 1 : 1));
      expect(retry.status, f.name).toBe(200);
      expect(await retry.text(), f.name).toBe(`{"status":"duplicate","event_id":"${id}"}`);
    }
    const count = await env.DB.prepare(
      `SELECT COUNT(*) AS n FROM notifications WHERE event_id IN (${ids.map(() => "?").join(",")})`,
    )
      .bind(...ids)
      .first<{ n: number }>();
    expect(count?.n).toBe(ids.length); // exactly one row per event ID
    await removeRows(ids);
  });

  it("request: the backend answers exactly as the vectors say", async () => {
    for (const { f } of of("request")) {
      const res = await post(f.body as string);
      expect(res.status, f.name).toBe(Number(f.status));
      expect(await res.text(), f.name).toBe(f.response);
    }
  });

  it("response: every backend-produced response is exactly the vector body", async () => {
    const base = of("encode")[0]?.f.body as string;
    const id = JSON.parse(base).event_id as string;
    await removeRows([id]);
    const changed = JSON.stringify({ ...JSON.parse(base), value: 8001 });
    const scenarios: Record<string, () => Promise<Response>> = {
      first_post: () => post(base),
      repeat_post: () => post(withAttempt(base, 2)),
      changed_payload: () => post(changed),
      wrong_token: () => post(base, { token: "wrong-token-00000000000000000000000000000000" }),
      wrong_path: () => post(base, { path: "/api/v1/nope" }),
      post_to_health: () => post(base, { path: "/health" }),
      oversized_body: () => post(base.padEnd(5000, " ")),
      text_plain: () => post(base, { contentType: "text/plain" }),
    };
    let produced = 0;
    for (const { f } of of("response")) {
      if (f.scenario === "-") continue; // synthetic (proxy/network), firmware-only
      const run = scenarios[f.scenario as string];
      expect(run, `unknown scenario ${f.scenario}`).toBeDefined();
      if (run === undefined) continue;
      const res = await run();
      expect(res.status, f.name).toBe(Number(f.status));
      expect(await res.text(), f.name).toBe(f.body);
      expect(f.sent, f.name).toBe(id);
      produced++;
    }
    expect(produced).toBe(Object.keys(scenarios).length);
    await removeRows([id]);
  });
});
