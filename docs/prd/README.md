# Solar tracker PRDs

Ten independent proposals, written 2026-09-22. Each one stands alone and can be built
without the others; the dependencies below are about ordering, not coupling.

All of them are grounded in a research pass whose adversarial verification stage did not
finish. Claims are well sourced but unverified — each PRD marks its own load-bearing ones.

## The set

| # | Title | Estimate | Why now |
|---|---|---|---|
| [01](01-esp32-bridge-firmware.md) | Update the ESP32-S3 bridge firmware | 30 min to find out | Every boot prints `Please upgrade the firmware`. May not need a trip at all — read the version first |
| [02](02-esp32-ota-updates.md) | Over-the-air firmware updates | 2.5–3 days | Ends the laptop trips. Confirmed viable: staging happens in the ESP32's own 512 KB SPIFFS |
| [03](03-robust-clock-healing.md) | Harden clock health detection | 6–9 h | `isHealthy()` accepts year 2043, which is a real observed garbage value |
| [04](04-watchdog-and-recovery.md) | Watchdog and unattended recovery | 2–2.5 days | There is no watchdog. Nothing recovers a hang without a human |
| [05](05-mqtt-telemetry.md) | MQTT telemetry delivery | 1 h decision | Recommends **against** a persistent session, and says why at source level |
| [06](06-full-log-sync.md) | Ship the full diagnostic log to the cloud | 5–6 days | Cloud rows show *that* something happened, never *what* |
| [07](07-telemetry-schema-v2.md) | A real diagnostics channel | 2–2.5 days | Wedge counts currently travel disguised as wind speed |
| [08](08-button-panel-pin-audit.md) | Audit the button panel switch pins | ~5 h | A switch read `true` while physically off. Floating pin suspected |
| [09](09-time-limited-access-point.md) | Time-limit the settings access point | 1.5 days | A switch left on silently disables every cloud path |
| [10](10-server-branch-reconciliation.md) | Reconcile the server branches | 3.5 h | `main`'s server is dead code; the deployed one lives on another branch |

## Suggested order

Three of these gate the rest, and all three are short.

1. **10** — settle the branches. PRDs 02, 06 and 07 all change the server; without this they
   land on a branch that is not deployed.
2. **01** — read `WiFi.firmwareVersion()`. Thirty minutes, and it decides whether OTA needs a
   site visit or already works.
3. **04** — the watchdog. Every feature that adds network activity adds hang surface, and
   this is what makes the rest safe to leave unattended.

Then the cheap independent wins: **03** (clock healing), **08** (pin audit).

Then the features: **07** → **02** (the OTA trigger needs a version field), **09**, **06**.

**05** is a decision, not a build. Read it before assuming MQTT is wanted.

## Batch the USB trip

01, 03 and 04 all want a flash. 01 needs USB regardless. Do them in one visit — after 02
lands, later changes go over the air.
