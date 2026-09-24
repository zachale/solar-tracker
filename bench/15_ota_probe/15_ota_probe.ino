// 15_ota_probe — isolate why OTAUpdate::startDownload() returns -26 (modem timeout).
//
// One variant per boot, chosen at build time with -DPROBE_A / -DPROBE_B / -DPROBE_C:
//   A  Arduino's own reference .ota on downloads.arduino.cc, https, GTS roots
//   B  our .ota on storage.googleapis.com, https, GTS roots   (the failing case)
//   C  the same object over plain http                        (takes TLS out)
//   D  Arduino's 29 KB file copied to the bucket at a short path (GCS itself vs our path)
//   E  Arduino's 29 KB file at our token path                  (the path alone)
//   F  our 112 KB image at a short path                        (the file alone)
//   G  our image under a shorter token, command line < 128 bytes (length alone)
//
// Result: A, D, G work; B, C, E fail. Every failing case formats an AT command of 128
// bytes or more, and WiFiS3's Modem::write() formats into a 128 byte buffer, so the
// line loses its closing \n and the bridge never runs it. See tools/ota/README.md.
//
// Runs in a clean environment: no tracker, no telemetry, no earlier TLS from the RA4M1.
// Never calls update(), so nothing is flashed whatever happens. Never prints the URL,
// because the OTA path carries a token and serial captures are committed.
//
// Every result line is kept and replayed every 10 s, without touching the modem again,
// so the outcome can be read whenever a capture attaches and even if the attempt left the
// bridge unable to answer AT commands. That makes a power cycle run the experiment.

#define BENCH_WIFI
#include "bench_common.h"
#include <OTAUpdate.h>
#include "secrets.h"

namespace
{
#include "root_ca.h"
}

OTAUpdate ota;

static String results[16];
static int resultCount = 0;

static void note(const String &line)
{
  String stamped = "PROBE [" + String(millis() / 1000) + "s] " + line;
  Serial.println(stamped);
  if (resultCount < 16)
    results[resultCount++] = stamped;
}

#if defined(PROBE_G)
static const char *VARIANT = "G (storage.googleapis.com, https, shorter token path, our 112 KB v5)";
static String probeUrl() { return String(OTA_BASE_URL) + "/v5.ota"; }
#elif defined(PROBE_E)
static const char *VARIANT = "E (storage.googleapis.com, https, token path, Arduino's 29 KB file)";
static String probeUrl() { return String(OTA_BASE_URL) + "/a.ota"; }
#elif defined(PROBE_F)
static const char *VARIANT = "F (storage.googleapis.com, https, short path, our 112 KB v5)";
static String probeUrl() { return "https://storage.googleapis.com/solar-tracker-442903-ota/f.ota"; }
#elif defined(PROBE_D)
static const char *VARIANT = "D (storage.googleapis.com, https, short path, Arduino's 29 KB file)";
static String probeUrl() { return "https://storage.googleapis.com/solar-tracker-442903-ota/a.ota"; }
#elif defined(PROBE_A)
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
  note(String("variant ") + VARIANT);
  note("bridge " + String(WiFi.firmwareVersion()));

  SD.begin(PIN_SD_CS);
  bool wifi = benchConnectAny();
  note(String("wifi ") + (wifi ? "connected" : "FAILED"));
  if (!wifi)
    return;

  int r = ota.begin("/update.bin");
  note("begin -> " + String(r));
  r = ota.setCACert(root_ca);
  note("setCACert -> " + String(r) + " (" + String(strlen(root_ca)) + " bytes)");

  String url = probeUrl();
  unsigned long t0 = millis();
  int size = ota.startDownload(url.c_str(), "/update.bin");
  note("startDownload -> " + String(size) + " after " + String(millis() - t0) + " ms");
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
  note("final progress " + String(done) + "/" + String(size) + " after " + String(millis() - t1) + " ms");
  if (done >= size)
  {
    note("verify -> " + String(ota.verify()) + " (nothing is flashed)");
  }
}

void loop()
{
  static unsigned long last = 0;
  if (millis() - last >= 10000)
  {
    last = millis();
    Serial.println("PROBE replay at " + String(millis() / 1000) + "s, " + String(resultCount) + " lines:");
    for (int i = 0; i < resultCount; i++)
      Serial.println("  " + results[i]);
  }
}
