// 08 — which network layer actually fails?
//
// R4HttpClient returns -1 for everything from "no DHCP lease" to "TLS timeout".
// This walks the stack one layer at a time and reports each separately:
//   L1 association   L2 DHCP lease   L3 DNS   L4 TCP/TLS connect   L5 HTTP GET

#include <WiFiS3.h>
#include <R4HttpClient.h>
#include <SPI.h>
#include <SD.h>
#include <ArduinoJson.h>
#include <RTClib.h>
#include "bench_common.h"
#include "secrets.h"

RTC_DS3231 rtc;
WiFiSSLClient sslc;
R4HttpClient http;
static unsigned long round_ = 0;

static void ip(const char *label, IPAddress a)
{
  Serial.print("BENCH   ");
  Serial.print(label);
  Serial.print("=");
  Serial.print(a[0]); Serial.print('.');
  Serial.print(a[1]); Serial.print('.');
  Serial.print(a[2]); Serial.print('.');
  Serial.println(a[3]);
}

static void probe()
{
  round_++;
  Serial.print("BENCH ===== round ");
  Serial.println(round_);

  // L1 association
  WiFi.end();
  delay(500);
  File f = SD.open("wifi.jsn", FILE_READ);
  JsonDocument doc;
  if (f) { deserializeJson(doc, f); f.close(); }
  bool up = false;
  String usedSsid;
  for (JsonObject o : doc.as<JsonArray>())
  {
    usedSsid = o["ssid"].as<String>();
    Serial.print("BENCH L1 associating to [");
    Serial.print(usedSsid);
    Serial.println("]");
    WiFi.begin(usedSsid.c_str(), o["pass"].as<String>().c_str());
    unsigned long t0 = millis();
    while (WiFi.status() != WL_CONNECTED && millis() - t0 < 20000) delay(200);
    if (WiFi.status() == WL_CONNECTED) { up = true; break; }
    Serial.print("BENCH L1 FAIL status=");
    Serial.println(WiFi.status());
  }
  if (!up) { Serial.println("BENCH L1 RESULT=FAIL no association"); return; }
  Serial.print("BENCH L1 RESULT=OK ssid=[");
  Serial.print(usedSsid);
  Serial.print("] rssi=");
  Serial.println(WiFi.RSSI());

  // L2 DHCP
  IPAddress me = WiFi.localIP();
  ip("ip", me);
  ip("gateway", WiFi.gatewayIP());
  ip("subnet", WiFi.subnetMask());
  ip("dns", WiFi.dnsIP());
  if (me[0] == 0) { Serial.println("BENCH L2 RESULT=FAIL no DHCP lease (0.0.0.0)"); return; }
  Serial.println("BENCH L2 RESULT=OK have a lease");

  // L3 gateway reachability, then DNS
  Serial.print("BENCH L3 resolving ");
  Serial.println(apiHost(API));
  IPAddress host;
  int r = WiFi.hostByName(apiHost(API).c_str(), host);
  if (r != 1) { Serial.print("BENCH L3 RESULT=FAIL hostByName="); Serial.println(r); }
  else { Serial.print("BENCH L3 RESULT=OK "); ip("resolved", host); }

  // L4 raw TLS connect, timed
  Serial.println("BENCH L4 TLS connect to :443");
  unsigned long t0 = millis();
  int ok = sslc.connect(apiHost(API).c_str(), 443);
  unsigned long dt = millis() - t0;
  Serial.print("BENCH L4 RESULT=");
  Serial.print(ok ? "OK" : "FAIL");
  Serial.print(" ms=");
  Serial.print(dt);
  Serial.print(" rssi=");
  Serial.println(WiFi.RSSI());
  if (ok)
  {
    // L5a hand-rolled GET over the socket we already proved
    sslc.print("GET /time/est HTTP/1.1\r\nHost: ");
    sslc.print(apiHost(API));
    sslc.print("\r\nConnection: close\r\n\r\n");
    String head;
    unsigned long r0 = millis();
    while (sslc.connected() && millis() - r0 < 12000 && head.length() < 120)
    {
      while (sslc.available() && head.length() < 120) head += (char)sslc.read();
    }
    sslc.stop();
    head.replace("\r", " ");
    head.replace("\n", " ");
    Serial.print("BENCH L5a raw GET first bytes: ");
    Serial.println(head.length() ? head : "(nothing)");
  }

  // L5b the library path the firmware uses
  String url = apiUrl(API, "/time/est");
  http.begin(sslc, url, 443);
  http.setTimeout(15000);
  http.addHeader("User-Agent: Arduino UNO R4 WiFi");
  unsigned long g0 = millis();
  int gc = http.GET();
  Serial.print("BENCH L5b R4HttpClient GET -> ");
  Serial.print(gc);
  Serial.print(" ms=");
  Serial.println(millis() - g0);
  http.close();

  RtcSnap s = rtcSnapshot(rtc);
  logSnap("08", round_, s, "EVENT=AFTER_NET_PROBE");
}

void setup()
{
  Serial.begin(9600);
  { unsigned long _t0 = millis(); while (!Serial && millis() - _t0 < 3000) { ; } }
  delay(1500);
  benchBanner("08_net_layers");
  parkActuator();
  rtcRailOn();
  delay(200);
  Wire.begin();
  rtc.begin();
  SD.begin(PIN_SD_CS);
}

void loop()
{
  probe();
  delay(10000);
}
