# Solar tracker PRDs

Nine proposals, written 2026-09-22, revised 2026-09-23. Each stands alone and can be built
without the others; the dependencies below are about ordering, not coupling.

All of them rest on a research pass whose adversarial verification stage did not finish.
Claims are well sourced but unverified — each PRD marks its own load-bearing ones.

## The goal these serve

> Make the board deployable, with full rich logs and full MQTT history, and OTA updates —
> so that when something fails we can debug it, build a new version, and push it.

Read that as a loop: **observe → diagnose → fix → deliver.** Every PRD below closes one gap
in that loop, and the loop is only as good as its weakest link.

| Loop stage | What closes it |
|---|---|
| Stay in contact | 09 (sync windows), 04 (watchdog) |
| Observe | 06 (logs), 07 (schema) |
| Deliver data | 05 (MQTT), 10 (server) |
| Deliver firmware | 01 (bridge), 02 (OTA) |
| Don't lie to us | 03 (clock healing) |

## The set

| # | Title | Estimate | Why |
|---|---|---|---|
| [01](01-esp32-bridge-firmware.md) | Update the ESP32-S3 bridge firmware | 30 min to find out | Every boot prints `Please upgrade the firmware`. May need no trip at all — read the version first |
| [02](02-esp32-ota-updates.md) | Over-the-air firmware updates | 2.5–3 days | The "push it" half of the goal. Staging happens in the ESP32's own 512 KB SPIFFS, so the flash ceiling doesn't apply |
| [03](03-robust-clock-healing.md) | Harden clock health detection | 6–9 h | `isHealthy()` accepts year 2043, a real observed garbage value |
| [04](04-watchdog-and-recovery.md) | Watchdog and unattended recovery | 2–2.5 days | Nothing recovers a hang without a human. Every other PRD adds hang surface |
| [05](05-mqtt-telemetry.md) | MQTT telemetry delivery | 3–4 days | The "full MQTT history" half. Build Option B — batched inside each window |
| [06](06-full-log-sync.md) | Ship the full diagnostic log to the cloud | 5–6 days | The "debug" half. Cloud rows show *that* something happened, never *what* |
| [07](07-telemetry-schema-v2.md) | A real diagnostics channel | 2–2.5 days | Wedge counts currently travel disguised as wind speed. Carries the version field OTA needs |
| [09](09-scheduled-sync-windows.md) | Scheduled sync windows inside a permanent AP | 2 days | Today a switch left on makes the board unreachable forever |
| [10](10-server-branch-reconciliation.md) | Reconcile the server branches | 3.5 h | `main`'s server is dead code; the deployed one lives on another branch |

Dropped: **08 (button panel pin audit)** — descoped 2026-09-23.

## Order

**Foundation — nothing works reliably without these.**

1. **10** (3.5 h) — settle the branches. 02, 06 and 07 all change the server; without this they land on a branch that isn't deployed.
2. **01** (30 min) — read `WiFi.firmwareVersion()`. Decides whether OTA needs a site visit or already works.
3. **04** (2–2.5 days) — the watchdog. Makes everything after it safe to leave unattended.
4. **09** (2 days) — sync windows. Until this lands, the settings switch can silence the board indefinitely, and no later PRD can promise a delivery interval.

**Then the loop, in dependency order.**

5. **07** — schema v2. Do this *before* 05 and 06, so they carry the new row instead of being rebuilt around it.
6. **03** — clock healing. Independent; fold into whichever flash is next.
7. **02** — OTA. Needs 07's version field.
8. **06** — logs, and **05** — MQTT. Both ride inside 09's windows and carry 07's schema.

**Batch the USB trip.** 01 needs USB regardless; 03, 04 and 09 all want a flash. One visit. After 02 lands, everything else goes over the air.
