# Dispenser v2 — A1.1.1 UI redesign

Status: Approved visual direction (Petr, 2026-11-14). Implement before calendar milestone A1.2.

## Goal

Transform the working A1.1 Android app from a basic technical demo into a polished, calm, aquarium-inspired smart-home UI. Preserve all existing behavior, repository/domain boundaries, tests and demo safeguards. This task is **UI-only**.

## Visual identity

- Always use a consistent **dark** Material 3 theme by default, independent of system light mode. Prepare theme infrastructure for a future user-selectable light mode, but no toggle is required now.
- Prefer deep charcoal/navy surfaces, not pure black; turquoise/aqua is the primary accent. Small muted lime accent for battery/healthy status; warm amber and soft red for warnings/errors.
- Suggested design tokens (refine for legible contrast): background `#111820`, surface `#202B36`, raised surface `#1C3038`, primary `#55D6C0`, on-surface `#F1F7F9`, muted text `#A9BDC7`, warning `#F1B86A`, critical `#EF7D86`, battery `#B7DE78`.
- Rounded cards (16–24dp), consistent 8dp spacing grid, restrained elevation, crisp typography. Avoid gratuitous gradients, glassmorphism, animation or decorative clutter.
- Use semantic MaterialTheme color roles, typography and shapes; avoid scattering hardcoded colors through individual screens.
- Keep texts in Czech, 24h time and millilitres.

## Dashboard (primary deliverable)

1. Compact header: small app brand `DISPENSER V2`, larger `Moje akvárium`, clearly visible `DEMO` badge. Never imply a real device is connected.
2. Hero card: **next dose** with `Dnes v 08:00`/date as appropriate, schedule name, strong `10 ml` highlight, subtle droplet icon.
3. Two status cards: **Zásobník** shows amount remaining, capacity and accessible progress indicator; **Baterie** shows large percentage and progress or battery icon. Both explicitly say **odhad/simulace** as appropriate.
4. Upcoming calendar preview with seven-day strip and occurrence indicators, driven by the existing mock schedules. Navigation to Calendar is functional; never present placeholder dates as real occurrences.
5. Consistent four-item bottom navigation: Přehled, Kalendář, Historie, Nastavení. Selected item turquoise; unselected muted. Keep Android system bars legible in dark mode.
6. Responsive layout for compact Android phone and larger phone, no horizontal clipping, scrolling for short screens, edge-to-edge insets handled.

## Reusable components

Implement modest reusable Compose components under a feature/shared UI package as suits the existing project:

- `DispenserTheme`/semantic dark palette
- `StatusCard` or equivalent
- `MetricProgress` with accessible text/semantics
- `DemoBadge`
- `UpcomingDoseCard`
- `WeekPreview` (presentation only; no full calendar editor in this task)

No extra UI library or major Gradle upgrade unless clearly justified. Prefer stable existing Material 3 / Compose APIs.

## Dynamic states

- Preserve the source-of-truth mock state through the ViewModel. Do not hardcode sample numbers into composables.
- Make status styling ready for `normal`, `low stock` (amber), and `blocked` (red) device conditions, without modifying reservoir/scheduler business logic.
- The UI must clearly differentiate reservoir **estimate** from true sensor measurement.
- Test long Czech labels, low capacity, null/unknown battery, no next scheduled dose and dark contrast.

## Acceptance / quality

- A1.1 functionality and navigation unchanged; no BLE/Wi-Fi/firmware changes.
- Theme stays dark even when Android phone is set to light mode; intentional dynamic wallpaper color does not override brand colors.
- Screens rendered on emulator and reviewed visually (screenshot if available).
- Accessible contrast, content descriptions for icons, touch targets >= 48dp for controls.
- Run `./gradlew assembleDebug` and `./gradlew test` from `android/`, avoid resource-heavy instrumented UI tests while memory is limited.
- Report changed files, build results, visual QA observations and outstanding issues; do not commit/push without approval.

## First agent prompt

Read `CLAUDE.md`, this task and the existing Android code. Inspect Git status. Propose a concise UI change plan, then implement the dark Material 3 redesign of the A1.1 dashboard and reusable design tokens/components described here. Preserve domain logic and mock repository. Run `assembleDebug` and unit tests, but defer emulator instrumentation tests because the machine has limited free RAM. Stop after the redesign for visual review. Do not commit or push.
