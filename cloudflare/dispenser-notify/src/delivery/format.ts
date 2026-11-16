/**
 * Czech e-mail texts for firmware notifications (M1.6b). Only facts present in the event
 * are shown: a missing measurement is reported as missing, never estimated.
 */
import type { NotificationEvent } from "../protocol";

export const SENDER_NAME = "Dávkovač V2";

export interface FormattedEmail {
  subject: string;
  body: string;
}

const SEVERITY: Record<NotificationEvent["severity"], string> = {
  WARNING: "VAROVÁNÍ",
  CRITICAL: "KRITICKÉ",
};

/** "01:23:45:67:89:AB" from the 12-hex device ID (factory MAC). */
export function formatDeviceId(deviceId: string): string {
  return deviceId.toUpperCase().replace(/(..)(?!$)/g, "$1:");
}

/** µl -> "8,0 ml" (one decimal, Czech decimal comma). */
export function formatMl(ul: number): string {
  return `${(ul / 1000).toFixed(1).replace(".", ",")} ml`;
}

/** mV -> "3,30 V". */
export function formatVolts(mv: number): string {
  return `${(mv / 1000).toFixed(2).replace(".", ",")} V`;
}

/** Whole percent, rounded DOWN so that the stock is never overstated. */
export function reservoirPercent(remainingUl: number, capacityUl: number): number | null {
  if (capacityUl <= 0) return null;
  return Math.floor((remainingUl * 100) / capacityUl);
}

const PRAGUE = new Intl.DateTimeFormat("cs-CZ", {
  timeZone: "Europe/Prague",
  day: "numeric",
  month: "numeric",
  year: "numeric",
  hour: "2-digit",
  minute: "2-digit",
  timeZoneName: "shortOffset",
});

/** Unix seconds -> "20. 11. 2026 08:00 SEČ" (Europe/Prague, SEČ/SELČ). */
export function formatPragueTime(unixSeconds: number): string {
  const parts = Object.fromEntries(
    PRAGUE.formatToParts(new Date(unixSeconds * 1000)).map((p) => [p.type, p.value]),
  );
  const zone =
    parts.timeZoneName === "GMT+2"
      ? "SELČ"
      : parts.timeZoneName === "GMT+1"
        ? "SEČ"
        : parts.timeZoneName;
  return `${parts.day}. ${parts.month}. ${parts.year} ${parts.hour}:${parts.minute} ${zone}`;
}

/** D1 CURRENT_TIMESTAMP text ("2026-11-15 22:13:40", UTC) -> unix seconds, or null. */
export function parseD1Timestamp(text: string | null): number | null {
  if (text === null || !/^\d{4}-\d{2}-\d{2} \d{2}:\d{2}:\d{2}$/.test(text)) return null;
  const t = Date.parse(`${text.replace(" ", "T")}Z`);
  return Number.isNaN(t) ? null : Math.floor(t / 1000);
}

interface Content {
  title: string;
  description: string;
  details: string[];
  /** Short subject suffix, e.g. "8 %". */
  subjectHint?: string;
}

function reservoirDetails(e: NotificationEvent): { lines: string[]; hint?: string } {
  const pct = reservoirPercent(e.value, e.value2);
  const lines: string[] = [];
  let hint: string | undefined;
  if (pct === null) {
    lines.push("Zásoba (odhad): neuvedena (kapacita zásobníku není nastavena)");
  } else {
    lines.push(`Zásoba (odhad): ${pct} % (${formatMl(e.value)} z ${formatMl(e.value2)})`);
    hint = `${pct} %`;
  }
  lines.push(
    e.uncertain
      ? "Odhad zásoby je nejistý: ano – po neúspěšném nebo přerušeném dávkování nemusí odpovídat skutečnosti."
      : "Odhad zásoby je nejistý: ne",
  );
  return hint === undefined ? { lines } : { lines, hint };
}

const DOSING: Record<string, { title: string; description: string; consumed: boolean }> = {
  START_FAILED: {
    title: "Dávku se nepodařilo spustit",
    description: "Čerpadlo se nepodařilo spustit.",
    consumed: true,
  },
  PUMP_FAILED: {
    title: "Selhání čerpadla",
    description: "Čerpadlo selhalo během dávkování.",
    consumed: true,
  },
  INTERRUPTED: {
    title: "Dávkování bylo přerušeno",
    description:
      "Dávkování přerušil restart nebo výpadek napájení. Dávka se automaticky neopakuje.",
    consumed: true,
  },
  STORAGE: {
    title: "Dávka neproběhla – chyba úložiště",
    description: "Potřebné úložiště v zařízení nebylo dostupné, dávka se nespustila.",
    consumed: false,
  },
  BLOCKED: {
    title: "Dávka neproběhla – blokováno",
    description: "Dávku zablokoval kritický stav zařízení, dávka se nespustila.",
    consumed: false,
  },
};

function content(e: NotificationEvent): Content {
  switch (e.type) {
    case "RESERVOIR_LOW": {
      const r = reservoirDetails(e);
      return {
        title: "Nízká zásoba v zásobníku",
        description:
          "Odhadovaná zásoba klesla pod nastavenou mez. Doplňte zásobník a potvrďte doplnění v aplikaci.",
        details: r.lines,
        ...(r.hint === undefined ? {} : { subjectHint: r.hint }),
      };
    }
    case "RESERVOIR_BLOCKED": {
      const r = reservoirDetails(e);
      return {
        title: "Automatické dávkování je zablokováno",
        description:
          "Odhadovaná zásoba nestačí na další dávku, automatické dávkování je proto zastaveno. Doplňte zásobník a potvrďte doplnění v aplikaci.",
        details: r.lines,
        ...(r.hint === undefined ? {} : { subjectHint: r.hint }),
      };
    }
    case "BATTERY_LOW":
      return {
        title: "Slabá baterie",
        description: "Baterie zařízení je slabá. Vyměňte nebo nabijte baterii.",
        details: [
          e.value > 0 ? `Napětí baterie: ${formatVolts(e.value)}` : "Napětí baterie: neuvedeno",
        ],
        ...(e.value > 0 ? { subjectHint: formatVolts(e.value) } : {}),
      };
    case "DOSING_ERROR": {
      const d = DOSING[e.code ?? ""] ?? {
        title: "Chyba dávkování",
        description: "Při dávkování došlo k chybě.",
        consumed: false,
      };
      const details = [
        `Druh dávky: ${e.manual ? "ruční" : "plánovaná (automatická)"}`,
        e.value > 0
          ? `Požadované množství: ${formatMl(e.value)}`
          : "Požadované množství: neuvedeno",
      ];
      details.push(
        d.consumed
          ? "Dávka byla předem odečtena z odhadu zásoby; odhad je do příštího doplnění nejistý."
          : "Dávka nebyla spuštěna.",
      );
      return { title: d.title, description: d.description, details };
    }
    case "DEVICE_ERROR":
      switch (e.code) {
        case "STORAGE":
          return {
            title: "Chyba úložiště zařízení",
            description:
              "Zařízení nemůže spolehlivě číst nebo zapisovat svá data. Automatické dávkování může být zastaveno.",
            details: e.value > 0 ? [`Kód detailu: ${e.value}`] : [],
          };
        case "TIME_SYNC":
          return {
            title: "Synchronizace času je opožděná",
            description:
              "Zařízení se delší dobu nesynchronizovalo s internetovým časem. Dávkování pokračuje, zkontrolujte připojení k Wi-Fi.",
            details: e.value > 0 ? [`Neúspěšné pokusy o synchronizaci: ${e.value}`] : [],
          };
        case "CLOCK":
          return {
            title: "Čas zařízení není znám",
            description:
              "Zařízení nezná přesný čas, automatické dávkování je proto pozastaveno. Zkontrolujte připojení k Wi-Fi.",
            details: e.value > 0 ? [`Neúspěšné pokusy o synchronizaci: ${e.value}`] : [],
          };
        case "QUEUE_OVERFLOW":
          return {
            title: "Přetečení fronty upozornění",
            description:
              "Některá upozornění nebylo možné v zařízení uložit, a proto nebyla odeslána.",
            details: [`Počet ztracených upozornění: ${e.value}`],
          };
        default:
          return { title: "Chyba zařízení", description: "Zařízení hlásí chybu.", details: [] };
      }
  }
}

/** Builds the subject and plain-text body. receivedAt: D1 received_at (UTC text). */
export function formatEmail(e: NotificationEvent, receivedAt: string | null): FormattedEmail {
  const c = content(e);
  const reminder = e.reminder_no > 0;
  const subject =
    `[${SENDER_NAME}] ${SEVERITY[e.severity]}: ${reminder ? "Připomínka – " : ""}${c.title}` +
    (c.subjectHint === undefined ? "" : ` (${c.subjectHint})`);
  const received = parseD1Timestamp(receivedAt);
  const lines = [
    `${SENDER_NAME} – upozornění`,
    "",
    `Zařízení: ${formatDeviceId(e.device_id)}`,
    `Závažnost: ${SEVERITY[e.severity]}`,
    `Událost: ${c.title}`,
    ...(reminder ? [`Připomínka č. ${e.reminder_no}: stav stále trvá.`] : []),
    "",
    c.description,
    ...(c.details.length > 0 ? ["", ...c.details] : []),
    "",
    e.created_at === null
      ? "Čas události: neznámý (hodiny zařízení nebyly synchronizovány)"
      : `Čas události: ${formatPragueTime(e.created_at)}`,
    received === null
      ? "Přijato serverem: neznámo"
      : `Přijato serverem: ${formatPragueTime(received)}`,
    `ID události: ${e.event_id}`,
    "",
    "--",
    `Automatická zpráva systému ${SENDER_NAME}. Na tento e-mail neodpovídejte.`,
  ];
  return { subject, body: lines.join("\n") };
}
