// The radio, OTA and watchdog behind SolarTracker, as no-ops, so the tracker's own decisions
// (src/SolarTracker/SolarTracker.cpp) compile and run on the host. Nothing here talks to a
// network or a chip: a sync gets no answer and OTA never has anything to do.
#include "../../src/WifiClient/WifiClient.h"
#include "../../src/OtaUpdater/OtaUpdater.h"
#include "../../src/Watchdog/Watchdog.h"

bool WifiClient::ensureWifiConnection() { return false; }
String WifiClient::get(String) { return ""; }
int WifiClient::probe(String) { return 0; }
int WifiClient::post(String, const String &, const char *) { return 0; }

void OtaUpdater::boot(uint8_t) {}
void OtaUpdater::service(WifiClient &, bool) {}
bool OtaUpdater::busy() const { return false; }
void OtaUpdater::onBeforeRestart(void (*fn)(void *ctx, const char *version), void *ctx)
{
  beforeRestart = fn;
  beforeRestartCtx = ctx;
}

void Watchdog::progress() {}
