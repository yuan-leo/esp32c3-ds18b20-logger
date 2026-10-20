// ESP32-C3 + DS18B20: entirely local 5-second temperature recorder.
// Single factory firmware for 4MB ESP32-C3; no external Javascript or cloud.
// License: GPL-3.0 (see LICENSE).
#include <Arduino.h>
#include <WiFi.h>
#include <WebServer.h>
#include <Preferences.h>
#include <LittleFS.h>
#include <OneWire.h>
#include <DallasTemperature.h>
#include <math.h>
#include <time.h>
#include <sys/time.h>
#include <esp_system.h>
#include "dashboard.h"

namespace {
constexpr uint8_t DS18B20_PIN = 4;         // Data pin, with external 4.7k pull-up to 3V3
constexpr uint32_t SAMPLE_INTERVAL_MS = 5000;
constexpr uint32_t RESCAN_INTERVAL_MS = 60000;
constexpr uint16_t FLAG_VALID = 1;
constexpr uint16_t FLAG_WALL_CLOCK = 2;
constexpr uint32_t MIN_UNIX_TIME = 1704067200UL;  // 2024-01-01 00:00 UTC
constexpr uint8_t SEGMENT_COUNT = 12;
constexpr uint32_t SEGMENT_BYTES = 96UL * 1024UL; // 12 x 96KiB = 1.125MiB max data
constexpr size_t WRITE_BATCH = 12;          // Save every ~60 seconds to reduce wear
constexpr size_t SENSOR_QUEUE_DEPTH = 64;   // Keeps samples while exporting CSV
constexpr size_t CHART_MAX_POINTS = 1100;
constexpr char AP_PASSWORD[] = "TempLog1234";
constexpr uint32_t BOOT_JOIN_GRACE_MS = 15000; // Recovery AP is accessible before joining home Wi-Fi.
constexpr uint32_t STA_JOIN_TIMEOUT_MS = 22000;
constexpr uint32_t AP_CHECK_INTERVAL_MS = 8000;
constexpr char FIRMWARE_VERSION[] = "1.1.0-wifi-recovery";

// The fixed 8-byte binary format must not be changed without a migration.
struct __attribute__((packed)) Sample {
  uint32_t stamp;       // Unix UTC seconds when FLAG_WALL_CLOCK, else boot uptime seconds
  int16_t centiC;       // hundredths of Celsius; INT16_MIN when invalid
  uint16_t flags;       // bit 0: valid reading, bit 1: UTC wall clock
};
static_assert(sizeof(Sample) == 8, "Invalid binary log record size");

WebServer server(80);
Preferences prefs;
OneWire oneWire(DS18B20_PIN);
DallasTemperature thermometer(&oneWire);
QueueHandle_t sampleQueue = nullptr;
Sample pending[WRITE_BATCH];
size_t pendingCount = 0;
Sample latest{};
bool haveLatest = false;
bool filesystemOK = false;
bool storageFailure = false;
uint8_t activeSegment = 0;
String apName, homeSsid, homePassword;
volatile bool sensorPresent = false;
volatile uint32_t dropped = 0;
volatile bool apRunning = false;
volatile uint8_t lastStaDisconnectReason = 0;
volatile uint32_t apStarts = 0;
volatile uint32_t apStops = 0;
uint32_t bootCount = 0;
esp_reset_reason_t bootResetReason = ESP_RST_UNKNOWN;
bool staConnecting = false;
bool staRetryPending = false;
bool staSuspended = false; // Don't endlessly disrupt SoftAP when home Wi-Fi is unavailable.
uint32_t staJoinAt = 0;
uint32_t staConnectStarted = 0;
uint32_t lastApCheckAt = 0;
uint32_t lastWifiLogAt = 0;

void startStation();
void enterApOnly(bool rememberFailure);
void serviceWifi();

String segmentName(uint8_t index) {
  char path[18];
  snprintf(path, sizeof(path), "/log%02u.bin", unsigned(index));
  return String(path);
}

uint32_t nowStamp(bool &isWallClock) {
  const time_t now = time(nullptr);
  isWallClock = now >= static_cast<time_t>(MIN_UNIX_TIME) && now < 2140000000;
  return isWallClock ? static_cast<uint32_t>(now) : millis() / 1000UL;
}

// Sampling is in a separate FreeRTOS task so slow CSV downloads don't miss
// their 5-second cadence. Only the main loop writes to LittleFS.
void sensorTask(void *) {
  thermometer.begin();
  thermometer.setResolution(12);              // ~0.75 s conversion, 0.0625 C steps
  thermometer.setWaitForConversion(true);
  TickType_t wake = xTaskGetTickCount();
  uint32_t lastRescan = 0;
  for (;;) {
    if (thermometer.getDeviceCount() == 0 &&
        (lastRescan == 0 || uint32_t(millis() - lastRescan) >= RESCAN_INTERVAL_MS)) {
      thermometer.begin();
      thermometer.setResolution(12);
      thermometer.setWaitForConversion(true);
      lastRescan = millis();
    }

    const bool detected = thermometer.getDeviceCount() != 0;
    sensorPresent = detected;
    float c = DEVICE_DISCONNECTED_C;
    if (detected) {
      thermometer.requestTemperatures();
      c = thermometer.getTempCByIndex(0);
    }

    Sample s{};
    bool wallTime = false;
    s.stamp = nowStamp(wallTime);
    s.flags = wallTime ? FLAG_WALL_CLOCK : 0;
    if (detected && isfinite(c) && c >= -55.0f && c <= 125.0f &&
        c != DEVICE_DISCONNECTED_C) {
      s.centiC = static_cast<int16_t>(lroundf(c * 100.0f));
      s.flags |= FLAG_VALID;
    } else {
      s.centiC = INT16_MIN;
      if (detected) sensorPresent = false; // read failed even though sensor was seen
    }
    if (xQueueSend(sampleQueue, &s, 0) != pdPASS) ++dropped;
    vTaskDelayUntil(&wake, pdMS_TO_TICKS(SAMPLE_INTERVAL_MS));
  }
}

bool rotateSegment() {
  if (!filesystemOK) return false;
  const uint8_t next = (activeSegment + 1U) % SEGMENT_COUNT;
  const String path = segmentName(next);
  // Erase the oldest segment BEFORE selecting it as the active one, so a power
  // failure during rotation can be retried without appending to stale data.
  if (LittleFS.exists(path) && !LittleFS.remove(path)) return false;
  File empty = LittleFS.open(path, "w");
  if (!empty) return false;
  empty.close();
  prefs.putUChar("seg", next);
  activeSegment = next;
  return true;
}

bool flushLog() {
  if (!pendingCount) return true;
  if (!filesystemOK) return false;
  size_t saved = 0;
  while (saved < pendingCount) {
    File f = LittleFS.open(segmentName(activeSegment), "a");
    if (!f) break;
    const size_t fileLength = f.size();
    const size_t fits = fileLength < SEGMENT_BYTES ?
      (SEGMENT_BYTES - fileLength) / sizeof(Sample) : 0;
    if (fits == 0) {
      f.close();
      if (!rotateSegment()) break;
      continue;
    }
    const size_t n = min(fits, pendingCount - saved);
    const size_t bytes = f.write(reinterpret_cast<const uint8_t *>(pending + saved),
                                 n * sizeof(Sample));
    f.flush();
    f.close();
    if (bytes != n * sizeof(Sample)) break;
    saved += n;
  }
  if (saved) {
    memmove(pending, pending + saved, (pendingCount - saved) * sizeof(Sample));
    pendingCount -= saved;
  }
  if (pendingCount) storageFailure = true;
  else storageFailure = false;
  return pendingCount == 0;
}

void appendRecord(const Sample &s) {
  latest = s;
  haveLatest = true;
  if (pendingCount == WRITE_BATCH && !flushLog()) {
    ++dropped;                            // no room: retain latest records
    memmove(pending, pending + 1, (WRITE_BATCH - 1) * sizeof(Sample));
    pendingCount = WRITE_BATCH - 1;
  }
  pending[pendingCount++] = s;
  if (pendingCount >= WRITE_BATCH) flushLog();
}

// Consumers read a stable snapshot, including RAM records not yet flushed.
// Iterating oldest-to-newest uses the persisted rotating write index.
template <class Visitor>
void visitSamples(Visitor visitor) {
  const uint8_t currentSegment = activeSegment;
  const size_t inRAM = pendingCount;
  Sample ramSnapshot[WRITE_BATCH];
  memcpy(ramSnapshot, pending, inRAM * sizeof(Sample));
  if (filesystemOK) {
    for (unsigned step = 1; step <= SEGMENT_COUNT; ++step) {
      uint8_t seg = (currentSegment + step) % SEGMENT_COUNT;
      const String path = segmentName(seg);
      if (!LittleFS.exists(path)) continue;
      File f = LittleFS.open(path, "r");
      if (!f) continue;
      size_t records = f.size() / sizeof(Sample);
      for (size_t i = 0; i < records; ++i) {
        Sample s{};
        if (f.read(reinterpret_cast<uint8_t *>(&s), sizeof(s)) != sizeof(s)) break;
        if (!visitor(s)) { f.close(); return; }
        if ((i & 511U) == 511U) delay(1); // allow Wi-Fi + idle tasks to run
      }
      f.close();
    }
  }
  for (size_t i = 0; i < inRAM; ++i) {
    if (!visitor(ramSnapshot[i])) return;
  }
}

uint32_t sampleCount(uint32_t *rawLogBytes = nullptr) {
  uint32_t count = pendingCount;
  uint32_t bytes = 0;
  if (filesystemOK) {
    for (uint8_t i = 0; i < SEGMENT_COUNT; ++i) {
      const String path = segmentName(i);
      if (!LittleFS.exists(path)) continue;
      File f = LittleFS.open(path, "r");
      if (f) {
        const size_t n = f.size() / sizeof(Sample);
        count += n;
        bytes += n * sizeof(Sample);
        f.close();
      }
    }
  }
  if (rawLogBytes) *rawLogBytes = bytes + pendingCount * sizeof(Sample);
  return count;
}

String jsonString(const String &input) {
  String out;
  out.reserve(input.length() + 5);
  out += '"';
  for (size_t i = 0; i < input.length(); ++i) {
    const char c = input[i];
    if (c == '"' || c == '\\') { out += '\\'; out += c; }
    else if (static_cast<unsigned char>(c) < 32) {
      char hex[7];
      snprintf(hex, sizeof(hex), "\\u%04x", static_cast<unsigned char>(c));
      out += hex;
    } else out += c;
  }
  out += '"';
  return out;
}

void responseJSON(const String &body, int code = 200) {
  server.sendHeader("Cache-Control", "no-store");
  server.send(code, "application/json; charset=utf-8", body);
}

void onStatus() {
  uint32_t bytes = 0;
  uint32_t count = sampleCount(&bytes);
  bool clockSynced = false;
  nowStamp(clockSynced);
  const bool onLan = WiFi.status() == WL_CONNECTED;
  String body;
  body.reserve(650);
  body += "{\"sensor_found\":";
  body += sensorPresent ? "true" : "false";
  body += ",\"latest_valid\":";
  body += (haveLatest && (latest.flags & FLAG_VALID)) ? "true" : "false";
  body += ",\"latest_c\":";
  body += (haveLatest && (latest.flags & FLAG_VALID)) ? String(latest.centiC / 100.0f, 2) : "null";
  body += ",\"latest_record\":";
  if (haveLatest) {
    body += '[' + String(latest.stamp) + ',' + String(latest.centiC) + ',' +
            String(latest.flags) + ']';
  } else body += "null";
  body += ",\"samples\":" + String(count);
  body += ",\"log_bytes\":" + String(bytes);
  body += ",\"fs_total\":" + String(filesystemOK ? LittleFS.totalBytes() : 0);
  body += ",\"fs_ok\":";
  body += (filesystemOK && !storageFailure) ? "true" : "false";
  body += ",\"clock_synced\":";
  body += clockSynced ? "true" : "false";
  body += ",\"wifi_connected\":";
  body += onLan ? "true" : "false";
  body += ",\"wifi_ssid\":" + jsonString(onLan ? WiFi.SSID() : String());
  body += ",\"sta_ip\":" + jsonString(onLan ? WiFi.localIP().toString() : String());
  body += ",\"ap_ssid\":" + jsonString(apName);
  body += ",\"ap_ip\":" + jsonString(WiFi.softAPIP().toString());
  body += ",\"dropped\":" + String(dropped);
  body += '}';
  responseJSON(body);
}

void sendChunk(String &buffer) {
  if (buffer.length()) {
    server.sendContent(buffer);
    buffer = "";
  }
}

void onHistory() {
  uint32_t total = sampleCount();
  uint32_t requested = 720;
  if (server.hasArg("samples")) {
    const String raw = server.arg("samples");
    if (raw.length() <= 7) requested = strtoul(raw.c_str(), nullptr, 10);
  }
  const uint32_t selected = requested == 0 ? total : min(total, requested);
  const uint32_t start = total - selected;
  const uint32_t stride = selected == 0 ? 1 :
    (selected + static_cast<uint32_t>(CHART_MAX_POINTS) - 1) /
    static_cast<uint32_t>(CHART_MAX_POINTS);
  server.sendHeader("Cache-Control", "no-store");
  server.setContentLength(CONTENT_LENGTH_UNKNOWN);
  server.send(200, "application/json", "");
  String buffer = "{\"rows\":[";
  buffer.reserve(1400);
  uint32_t index = 0;
  bool first = true;
  uint32_t validCount = 0;
  int32_t minCenti = INT32_MAX;
  int32_t maxCenti = INT32_MIN;
  int64_t sumCenti = 0;
  visitSamples([&](const Sample &s) -> bool {
    if (index >= start && (s.flags & FLAG_VALID)) {
      ++validCount;
      minCenti = min(minCenti, static_cast<int32_t>(s.centiC));
      maxCenti = max(maxCenti, static_cast<int32_t>(s.centiC));
      sumCenti += s.centiC;
    }
    const bool include = index >= start &&
      ((index - start) % stride == 0 || index == total - 1);
    ++index;
    if (!include) return true;
    if (!first) buffer += ',';
    first = false;
    buffer += '[' + String(s.stamp) + ',' + String(s.centiC) + ',' + String(s.flags) + ']';
    if (buffer.length() >= 1200) sendChunk(buffer);
    return true;
  });
  buffer += "],\"stats\":{\"count\":" + String(validCount);
  buffer += ",\"min_c\":" + (validCount ? String(minCenti / 100.0, 4) : String("null"));
  buffer += ",\"max_c\":" + (validCount ? String(maxCenti / 100.0, 4) : String("null"));
  buffer += ",\"avg_c\":" + (validCount ? String(static_cast<double>(sumCenti) / validCount / 100.0, 4) : String("null"));
  buffer += "}}";
  sendChunk(buffer);
  server.sendContent("");
}

void onExport() {
  server.sendHeader("Content-Disposition", "attachment; filename=temperature_log.csv");
  server.sendHeader("Cache-Control", "no-store");
  server.setContentLength(CONTENT_LENGTH_UNKNOWN);
  server.send(200, "text/csv; charset=utf-8", "");
  String buffer = "timestamp_utc,uptime_seconds,temperature_c,temperature_f,status\r\n";
  buffer.reserve(1500);
  visitSamples([&](const Sample &s) -> bool {
    char row[105];
    char iso[24] = "";
    char uptime[18] = "";
    if (s.flags & FLAG_WALL_CLOCK) {
      time_t t = static_cast<time_t>(s.stamp);
      struct tm tmUTC;
      if (gmtime_r(&t, &tmUTC)) strftime(iso, sizeof(iso), "%Y-%m-%dT%H:%M:%SZ", &tmUTC);
    } else snprintf(uptime, sizeof(uptime), "%lu", static_cast<unsigned long>(s.stamp));
    if (s.flags & FLAG_VALID) {
      const double c = s.centiC / 100.0;
      snprintf(row, sizeof(row), "%s,%s,%.2f,%.2f,ok\r\n", iso, uptime, c, c * 1.8 + 32);
    } else snprintf(row, sizeof(row), "%s,%s,,,sensor_error\r\n", iso, uptime);
    buffer += row;
    if (buffer.length() >= 1200) sendChunk(buffer);
    return true;
  });
  sendChunk(buffer);
  server.sendContent("");
}

void onSetTime() {
  if (!server.hasArg("epoch")) { responseJSON("{\"error\":\"missing epoch\"}", 400); return; }
  const unsigned long val = strtoul(server.arg("epoch").c_str(), nullptr, 10);
  if (val < MIN_UNIX_TIME || val >= 2140000000UL) {
    responseJSON("{\"error\":\"invalid epoch\"}", 400);
    return;
  }
  struct timeval tv;
  tv.tv_sec = static_cast<time_t>(val);
  tv.tv_usec = 0;
  if (settimeofday(&tv, nullptr) == 0) responseJSON("{\"ok\":true}");
  else responseJSON("{\"error\":\"settimeofday failed\"}", 500);
}

void onSetWifi() {
  if (!server.hasArg("ssid")) { responseJSON("{\"error\":\"missing ssid\"}", 400); return; }
  String ssid = server.arg("ssid");
  String pass = server.arg("password");
  ssid.trim();
  if (ssid.isEmpty() || ssid.length() > 32 || pass.length() > 63 ||
      (pass.length() && pass.length() < 8)) {
    responseJSON("{\"error\":\"invalid Wi-Fi settings\"}", 400);
    return;
  }
  homeSsid = ssid;
  homePassword = pass;
  prefs.putString("ssid", homeSsid);
  prefs.putString("pwd", homePassword);
  reconnectNeeded = true;
  reconnectAt = millis() + 900;
  responseJSON("{\"ok\":true}");
}

void onClearLog() {
  if (!server.hasArg("confirm") || server.arg("confirm") != "erase") {
    responseJSON("{\"error\":\"confirmation required\"}", 400);
    return;
  }
  if (!filesystemOK) { responseJSON("{\"error\":\"filesystem not mounted\"}", 500); return; }
  pendingCount = 0;
  haveLatest = false;
  for (uint8_t i = 0; i < SEGMENT_COUNT; ++i) {
    String p = segmentName(i);
    if (LittleFS.exists(p)) LittleFS.remove(p);
  }
  activeSegment = 0;
  prefs.putUChar("seg", 0);
  storageFailure = false;
  responseJSON("{\"ok\":true}");
}

void startStation() {
  if (homeSsid.isEmpty()) return;
  WiFi.disconnect(false, false);
  WiFi.begin(homeSsid.c_str(), homePassword.c_str());
  nextWifiRetry = millis() + 30000UL;
  Serial.printf("Connecting to SSID: %s\n", homeSsid.c_str());
}

void setupWebserver() {
  server.on("/", HTTP_GET, []() {
    server.sendHeader("Cache-Control", "no-store");
    server.send_P(200, "text/html; charset=utf-8", DASHBOARD_HTML);
  });
  server.on("/api/status", HTTP_GET, onStatus);
  server.on("/api/history", HTTP_GET, onHistory);
  server.on("/export.csv", HTTP_GET, onExport);
  server.on("/api/time", HTTP_POST, onSetTime);
  server.on("/api/wifi", HTTP_POST, onSetWifi);
  server.on("/api/clear", HTTP_POST, onClearLog);
  server.onNotFound([]() { server.send(404, "text/plain", "Not found"); });
  server.begin();
}

} // namespace

void setup() {
  Serial.begin(115200);
  delay(200);
  Serial.println("\nESP32-C3 DS18B20 offline temperature logger starting");

  prefs.begin("templog", false);
  activeSegment = prefs.getUChar("seg", 0);
  if (activeSegment >= SEGMENT_COUNT) activeSegment = 0;
  homeSsid = prefs.getString("ssid", "");
  homePassword = prefs.getString("pwd", "");

  filesystemOK = LittleFS.begin(false);
  if (!filesystemOK) {
    Serial.println("Filesystem not initialized; attempting first-boot format");
    filesystemOK = LittleFS.begin(true);
  }
  Serial.println(filesystemOK ? "LittleFS ready" : "ERROR: LittleFS unavailable; logging to RAM only");

  // ESP32-C3 eFuse MAC is stable across resets. Last six hex digits make the
  // hotspot SSID distinguishable if there is more than one logger nearby.
  char hotspot[25];
  snprintf(hotspot, sizeof(hotspot), "TempLogger-%06lX",
           static_cast<unsigned long>(ESP.getEfuseMac() & 0xFFFFFFUL));
  apName = hotspot;
  WiFi.mode(WIFI_AP_STA);
  WiFi.setSleep(false);
  WiFi.setHostname("temp-logger");
  if (!WiFi.softAP(apName.c_str(), AP_PASSWORD)) {
    Serial.println("ERROR: Could not create access point");
  }
  Serial.printf("Wi-Fi SSID: %s | password: %s | URL: http://%s/\n",
                apName.c_str(), AP_PASSWORD, WiFi.softAPIP().toString().c_str());
  startStation();
  if (!homeSsid.isEmpty()) configTime(0, 0, "pool.ntp.org", "time.google.com");
  setupWebserver();

  sampleQueue = xQueueCreate(SENSOR_QUEUE_DEPTH, sizeof(Sample));
  if (!sampleQueue || xTaskCreate(sensorTask, "sensor", 4096, nullptr, 1, nullptr) != pdPASS) {
    Serial.println("FATAL: Cannot create sensor task");
  }
}

void loop() {
  server.handleClient();
  if (sampleQueue) {
    Sample s;
    while (xQueueReceive(sampleQueue, &s, 0) == pdPASS) appendRecord(s);
  }
  if (reconnectNeeded && static_cast<int32_t>(millis() - reconnectAt) >= 0) {
    reconnectNeeded = false;
    startStation();
    if (!homeSsid.isEmpty()) configTime(0, 0, "pool.ntp.org", "time.google.com");
  }
  if (!homeSsid.isEmpty() && WiFi.status() != WL_CONNECTED &&
      static_cast<int32_t>(millis() - nextWifiRetry) >= 0 && !reconnectNeeded) {
    startStation();
  }
  delay(3);
}
