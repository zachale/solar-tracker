// 15_ota_probe — isolate why OTAUpdate::startDownload() returns -26 (modem timeout).
//
// One variant per boot, chosen at build time with -DPROBE_A / -DPROBE_B / -DPROBE_C:
//   A  Arduino's own reference .ota on downloads.arduino.cc, https, GTS roots
//   B  our .ota on storage.googleapis.com, https, GTS roots   (the failing case)
//   C  the same object over plain http                        (takes TLS out)
//
// Runs in a clean environment: no tracker, no telemetry, no earlier TLS from the RA4M1.
// Never calls update(), so nothing is flashed whatever happens. Never prints the URL,
// because the OTA path carries a token and serial captures are committed.
//
// After the attempt, a heartbeat every 10 s asks the ESP32 for its WiFi status. If the
// bridge crashed on the way out of a failed attempt, those queries stop answering.

#define BENCH_WIFI
#include "bench_common.h"
#include <OTAUpdate.h>
#include "secrets.h"

namespace
{
#include "root_ca.h"
}

OTAUpdate ota;

#if defined(PROBE_A)
static const char *VARIANT = "A (downloads.arduino.cc, https)";
static String probeUrl() { return "https://downloads.arduino.cc/ota/UNOR4WIFI_Animation.ota"; }
#elif defined(PROBE_C)
static const char *VARIANT = "C (storage.googleapis.com, http)";
static String probeUrl()
{
  String u = String(OTA_BASE_URL) + "/v5.ota";
  u.replace("https://", "http://");
  return u;
}
#else
static const char *VARIANT = "B (storage.googleapis.com, https)";
static String probeUrl() { return String(OTA_BASE_URL) + "/v5.ota"; }
#endif

void setup()
{
  Serial.begin(9600);
  delay(5000);
  Serial.print("PROBE variant ");
  Serial.println(VARIANT);
  Serial.print("PROBE bridge ");
  Serial.println(WiFi.firmwareVersion());

  SD.begin(PIN_SD_CS);
  bool wifi = benchConnectAny();
  Serial.println(String("PROBE wifi ") + (wifi ? "connected" : "FAILED"));
  if (!wifi)
    return;

  int r = ota.begin("/update.bin");
  Serial.println("PROBE begin -> " + String(r));
  r = ota.setCACert(root_ca);
  Serial.println("PROBE setCACert -> " + String(r) + " (" + String(strlen(root_ca)) + " bytes)");

  String url = probeUrl();
  unsigned long t0 = millis();
  int size = ota.startDownload(url.c_str(), "/update.bin");
  Serial.println("PROBE startDownload -> " + String(size) + " after " + String(millis() - t0) + " ms");
  if (size <= 0)
    return;

  unsigned long t1 = millis();
  unsigned long lastLog = 0;
  int done = 0;
  while (millis() - t1 < 120000UL)
  {
    done = ota.downloadProgress();
    if (done < 0 || done >= size)
      break;
    if (millis() - lastLog >= 2000)
    {
      lastLog = millis();
      Serial.println("PROBE progress " + String(done) + "/" + String(size));
    }
    delay(250);
  }
  Serial.println("PROBE final progress " + String(done) + "/" + String(size) + " after " + String(millis() - t1) + " ms");
  if (done >= size)
  {
    Serial.println("PROBE verify -> " + String(ota.verify()) + " (nothing is flashed)");
  }
}

void loop()
{
  static unsigned long last = 0;
  if (millis() - last >= 10000)
  {
    last = millis();
    Serial.println("PROBE heartbeat t=" + String(millis() / 1000) + "s wifi_status=" + String(WiFi.status()));
  }
}
