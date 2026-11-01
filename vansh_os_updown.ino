/*
  Vansh OS: two-button ESP32 + SH1106 OLED mini computer.
  Standalone sketch: open this file, or paste all of it into Arduino IDE.
  Board: ESP32 Dev Module (classic WROOM), Arduino ESP32 core 3.3.12.
  Partition: Huge APP (3MB No OTA/1MB SPIFFS), 4MB flash.
  Library: U8g2. Wiring: SDA 21, SCL 22, VCC 3V3, GND GND.
  Buttons (each between the GPIO and GND, internal pull-up used):
    DOWN = GPIO4   click: move down / next page   hold (0.7 s): SELECT / open
    UP   = GPIO27  click: move up / previous page hold (0.7 s): BACK
  Android setup: ESP Provisioning app; name and PIN appear on OLED.

  News: infinite-style headline feed. When you get within NEWS_PREFETCH
  headlines of the end, the UI asks the network worker for more. The worker
  rotates through several Ars Technica section feeds, appends only unseen
  stories (de-duplicated by article-link hash) and drops the oldest when the
  fixed-size store is full. The UI never touches the network.
*/
#include <Arduino.h>

// BEGIN PORTABLE NEWS PARSER
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

// Bounded RSS 2.0 reader: title + a hash of the article link (for de-duplication).
// No article bodies or entire feeds are retained.
namespace vbot {
constexpr size_t NEWS_COUNT = 24;   // max items kept from ONE feed download
constexpr size_t TITLE_SIZE = 241;
struct Headlines {
  char titles[NEWS_COUNT][TITLE_SIZE] = {};
  uint32_t ids[NEWS_COUNT] = {};    // FNV-1a hash of the article link (or title)
  size_t count = 0;
};

// 32-bit FNV-1a over the text with surrounding whitespace trimmed.
// Returns 0 for empty text (0 means "no identifier").
inline uint32_t hashText(const char *s, size_t n) {
  auto blank = [](char c) { return c == ' ' || c == '\n' || c == '\r' || c == '\t'; };
  while (n && blank(s[0])) { ++s; --n; }
  while (n && blank(s[n - 1])) --n;
  if (!n) return 0;
  uint32_t h = 2166136261u;
  for (size_t i = 0; i < n; ++i) { h ^= (unsigned char)s[i]; h *= 16777619u; }
  return h ? h : 1;
}

inline char displayChar(unsigned long c) {
  if (c >= 32 && c <= 126) return (char)c;
  if (c == 0x2018 || c == 0x2019) return '\'';
  if (c == 0x201c || c == 0x201d) return '"';
  if (c == 0x2013 || c == 0x2014) return '-';
  if (c == 0x2026) return '.';
  return ' ';
}

inline void plainTitle(const char *raw, char *out, size_t cap) {
  size_t n = 0;
  bool clipped = false;
  while (*raw) {
    unsigned long cp = (unsigned char)*raw++;
    if (cp == '&') {
      const char *end = strchr(raw, ';');
      if (end && end - raw < 12) {
        char entity[13] = {};
        memcpy(entity, raw, end - raw);
        if (!strcmp(entity, "amp")) cp = '&';
        else if (!strcmp(entity, "quot")) cp = '"';
        else if (!strcmp(entity, "apos") || !strcmp(entity, "rsquo")) cp = '\'';
        else if (!strcmp(entity, "lt")) cp = '<';
        else if (!strcmp(entity, "gt")) cp = '>';
        else if (!strcmp(entity, "nbsp")) cp = ' ';
        else if (entity[0] == '#') {
          char *tail;
          bool hex = entity[1] == 'x' || entity[1] == 'X';
          cp = strtoul(entity + (hex ? 2 : 1), &tail, hex ? 16 : 10);
          if (*tail) cp = ' ';
        } else cp = ' ';
        raw = end + 1;
      }
    } else if (cp >= 0xc0) {
      int extra = cp < 0xe0 ? 1 : cp < 0xf0 ? 2 : 3;
      cp &= extra == 1 ? 0x1f : extra == 2 ? 0x0f : 0x07;
      while (extra-- && *raw && ((unsigned char)*raw & 0xc0) == 0x80)
        cp = (cp << 6) | ((unsigned char)*raw++ & 0x3f);
    }
    char c = displayChar(cp);
    if (c == ' ' && (!n || out[n - 1] == ' ')) continue;
    if (n + 1 >= cap) { clipped = true; break; }
    out[n++] = c;
  }
  while (n && out[n - 1] == ' ') --n;
  if (clipped && n >= 3) memcpy(out + n - 3, "...", 3);
  out[n] = 0;
}

class RssReader {
 public:
  Headlines result;
  // Make the reader ready for a new download without re-zeroing the big arrays.
  void reset() {
    result.count = 0;
    tagLen = rawLen = linkLen = 0;
    inTag = inItem = inTitle = haveTitle = false;
    inLink = haveLink = linkOverflow = false;
    cdata = comment = overflow = false;
    previous = beforePrevious = 0;
  }
  void feed(const char *data, size_t len) {
    for (size_t i = 0; i < len && result.count < NEWS_COUNT; ++i) consume(data[i]);
  }
 private:
  char tag[96] = {}, raw[1024] = {}, link[256] = {};
  size_t tagLen = 0, rawLen = 0, linkLen = 0;
  bool inTag = false, inItem = false, inTitle = false, haveTitle = false;
  bool inLink = false, haveLink = false, linkOverflow = false;
  bool cdata = false, comment = false, overflow = false;
  char previous = 0, beforePrevious = 0;
  void append(char c) {
    if (inTitle) {
      if (rawLen + 1 < sizeof(raw)) raw[rawLen++] = c;
      else overflow = true;
    } else if (inLink) {
      if (linkLen < sizeof(link)) link[linkLen++] = c;
      else linkOverflow = true;
    }
  }
  void consume(char c) {
    if (comment) {
      if (beforePrevious == '-' && previous == '-' && c == '>') comment = false;
      beforePrevious = previous; previous = c; return;
    }
    if (cdata) {
      if (beforePrevious == ']' && previous == ']' && c == '>') {
        if (inTitle && rawLen >= 2 && !overflow) rawLen -= 2;
        else if (inLink && linkLen >= 2 && !linkOverflow) linkLen -= 2;
        cdata = false;
      } else append(c);
      beforePrevious = previous; previous = c; return;
    }
    if (!inTag) {
      if (c == '<') { inTag = true; tagLen = 0; }
      else append(c);
      return;
    }
    if (c != '>') {
      if (tagLen + 1 < sizeof(tag)) tag[tagLen++] = c;
      tag[tagLen] = 0;
      if (!strcmp(tag, "![CDATA[")) {
        cdata = true; inTag = false; previous = beforePrevious = 0;
      } else if (!strcmp(tag, "!--")) {
        comment = true; inTag = false; previous = beforePrevious = 0;
      }
      return;
    }
    inTag = false;
    if (!strcmp(tag, "item")) {
      inItem = true; inTitle = haveTitle = overflow = false; rawLen = 0;
      inLink = haveLink = linkOverflow = false; linkLen = 0;
    } else if (!strcmp(tag, "title") && inItem && !haveTitle) inTitle = true;
    else if (!strcmp(tag, "/title") && inTitle) { inTitle = false; haveTitle = true; }
    else if (!strcmp(tag, "link") && inItem && !haveLink) inLink = true;
    else if (!strcmp(tag, "/link") && inLink) { inLink = false; haveLink = true; }
    else if (!strcmp(tag, "/item") && inItem) {
      if (haveTitle) {
        raw[rawLen] = 0;
        char title[TITLE_SIZE];
        plainTitle(raw, title, sizeof(title));
        if (overflow && strlen(title) >= 3) memcpy(title + strlen(title) - 3, "...", 3);
        // Reliable identifier: the article link. Fall back to the title if there is none.
        uint32_t id = haveLink ? hashText(link, linkLen) : 0;
        if (!id) id = hashText(title, strlen(title));
        bool duplicate = false;
        for (size_t i = 0; i < result.count; ++i)
          if (result.ids[i] == id) duplicate = true;
        if (*title && id && !duplicate) {
          strcpy(result.titles[result.count], title);
          result.ids[result.count++] = id;
        }
      }
      inItem = inTitle = inLink = false;
    }
  }
};

// Word-wrapped lines used by both OLED messages and headline pages.
inline size_t wrap(const char *text, char rows[][25], size_t maxRows) {
  size_t count = 0;
  while (*text && count < maxRows) {
    while (*text == ' ') ++text;
    if (!*text) break;
    size_t len = strlen(text), take = len < 24 ? len : 24;
    if (len > 24 && text[24] != ' ') {
      for (size_t i = 24; i > 0; --i)
        if (text[i] == ' ') { take = i; break; }
    }
    memcpy(rows[count], text, take); rows[count++][take] = 0;
    text += take;
  }
  return count;
}
}  // namespace vbot
// END PORTABLE NEWS PARSER

// Small status mailbox published by the network worker (headline text is NOT
// in here any more; it lives in the mutex-protected store below).
struct NetworkSnapshot {
  bool wifi = false;
  bool provisioning = false;
  bool feedOk = false;
  bool setupError = false;
  bool fetching = false;      // a feed download is in progress
  bool fetchFailed = false;   // last download failed; worker retries with back-off
  bool exhausted = false;     // every source was checked recently, nothing new left
  char name[24] = {};
  char pin[9] = {};
  uint32_t fetchedAt = 0;
  uint32_t revision = 0;
  uint32_t newsFirst = 0;     // absolute number of the oldest headline still stored
  uint32_t newsCount = 0;     // headlines currently stored
};

bool startNetwork();
bool readNetwork(NetworkSnapshot &snapshot);
// Thread-safe copy of headline number `number` (absolute, see newsFirst). False if gone.
bool newsTitle(uint32_t number, char *out, size_t cap);
// Cheap, non-blocking hint from the UI that the user is near the end of the list.
void requestMoreNews();

#include <WiFi.h>
#include <Preferences.h>
#include <atomic>
#include <time.h>
#include <esp_wifi.h>
#include <esp32-hal-alloc-ble-mem.h>
#include <esp_http_client.h>
#include <esp_crt_bundle.h>
#include <network_provisioning/manager.h>
#include <network_provisioning/scheme_ble.h>
#include <freertos/queue.h>
#include <freertos/semphr.h>

#if !CONFIG_IDF_TARGET_ESP32
#error "This wiring/build targets the classic ESP32 (WROOM/Dev Module), not S2/S3/C3. Adapt and test before using another board."
#endif
#if !CONFIG_ESP_PROTOCOMM_SUPPORT_SECURITY_VERSION_1
#error "Vansh OS needs provisioning Security 1 with proof of possession."
#endif

namespace {
void onEvent(arduino_event_t *event);
bool beginSetup();
bool fetchFeed(const char *url);
void networkTask(void *);

// Plain RSS 2.0 feeds have no pagination/cursor, so "more" means "next source".
// Feed 0 is the main one (refreshed on a timer); the rest are tapped on demand.
const char *const FEEDS[] = {
  "https://feeds.arstechnica.com/arstechnica/technology-lab",
  "https://feeds.arstechnica.com/arstechnica/index",
  "https://feeds.arstechnica.com/arstechnica/features",
  "https://feeds.arstechnica.com/arstechnica/apple",
  "https://feeds.arstechnica.com/arstechnica/cars",
};
constexpr size_t FEED_COUNT = sizeof(FEEDS) / sizeof(FEEDS[0]);
constexpr uint32_t FETCH_MS = 30UL * 60UL * 1000UL;     // refresh of feed 0
constexpr uint32_t FEED_GAP_MS = 5UL * 60UL * 1000UL;   // never re-download one feed sooner
constexpr uint32_t RETRY_MIN_MS = 10000;                // back-off after a failure: 10 s ...
constexpr uint32_t RETRY_MAX_MS = 5UL * 60UL * 1000UL;  // ... doubling up to 5 min
constexpr uint32_t RECONNECT_MS = 30000;
constexpr uint32_t SETUP_AFTER_MS = 90000;
constexpr size_t MAX_FEED_BYTES = 256 * 1024;
constexpr size_t STORE_MAX = 40;   // headlines kept (40 x 241 B = ~9.6 KB)
constexpr size_t SEEN_MAX = 160;   // article hashes remembered for de-duplication

QueueHandle_t snapshots;
NetworkSnapshot state;
std::atomic<bool> credentialsReceived{false}, credentialsFailed{false};
std::atomic<bool> provisioningEnded{false}, gotIp{false};
std::atomic<bool> moreRequested{false};
bool managerActive = false;
char savedSsid[33] = {}, savedPassword[65] = {};

// Headline store: written only by the network task, read by the UI task.
// Every access holds storeLock; the lock is never held during network I/O.
struct NewsStore {
  char titles[STORE_MAX][vbot::TITLE_SIZE];
  size_t count;
  uint32_t first;   // absolute number of titles[0]; grows when old ones are dropped
};
NewsStore store;
SemaphoreHandle_t storeLock = nullptr;

// Network-task-only bookkeeping.
vbot::RssReader feedParser;               // static: keeps ~7 KB off the task stack
uint32_t seen[SEEN_MAX] = {};
size_t seenCount = 0, seenNext = 0;
uint32_t feedFetchedAt[FEED_COUNT] = {};
bool feedKnown[FEED_COUNT] = {};
size_t nextFeed = 1;

bool wasSeen(uint32_t id) {
  for (size_t i = 0; i < seenCount; ++i)
    if (seen[i] == id) return true;
  return false;
}

void markSeen(uint32_t id) {
  seen[seenNext] = id;
  seenNext = (seenNext + 1) % SEEN_MAX;
  if (seenCount < SEEN_MAX) ++seenCount;
}

bool feedEligible(size_t f, uint32_t now) {
  return !feedKnown[f] || now - feedFetchedAt[f] >= FEED_GAP_MS;
}

// Next source in rotation that has not been downloaded in the last FEED_GAP_MS.
int pickFeed(uint32_t now) {
  for (size_t k = 0; k < FEED_COUNT; ++k) {
    size_t f = (nextFeed + k) % FEED_COUNT;
    if (feedEligible(f, now)) return (int)f;
  }
  return -1;
}

// Append unseen headlines. When the store is full the oldest are dropped
// (store.first moves on, so absolute headline numbers never change).
size_t ingest(const vbot::Headlines &batch) {
  size_t added = 0;
  if (xSemaphoreTake(storeLock, portMAX_DELAY) != pdTRUE) return 0;
  for (size_t i = 0; i < batch.count; ++i) {
    uint32_t id = batch.ids[i];
    if (!id || wasSeen(id)) continue;
    markSeen(id);
    if (store.count == STORE_MAX) {
      memmove(store.titles[0], store.titles[1], (STORE_MAX - 1) * sizeof(store.titles[0]));
      --store.count;
      ++store.first;
    }
    strcpy(store.titles[store.count++], batch.titles[i]);
    ++added;
  }
  state.newsFirst = store.first;
  state.newsCount = (uint32_t)store.count;
  xSemaphoreGive(storeLock);
  return added;
}

void onEvent(arduino_event_t *event) {
  // Callbacks run on a different task. Never draw, write flash, or block here.
  switch (event->event_id) {
    case ARDUINO_EVENT_PROV_CRED_RECV: credentialsReceived = true; break;
    case ARDUINO_EVENT_PROV_CRED_FAIL: credentialsFailed = true; break;
    case ARDUINO_EVENT_PROV_END: provisioningEnded = true; break;
    case ARDUINO_EVENT_WIFI_STA_GOT_IP: gotIp = true; break;
    default: break;
  }
}

bool beginSetup() {
  network_prov_mgr_config_t config = {};
  config.scheme = network_prov_scheme_ble;
  // Keep BT memory available so setup can restart after a later network loss.
  config.scheme_event_handler = NETWORK_PROV_EVENT_HANDLER_NONE;
  if (network_prov_mgr_init(config) != ESP_OK) return false;
  static uint8_t uuid[16] = {
    0xb4, 0xdf, 0x5a, 0x1c, 0x3f, 0x6b, 0xf4, 0xbf,
    0xea, 0x4a, 0x82, 0x03, 0x04, 0x90, 0x1a, 0x02
  };
  if (network_prov_scheme_ble_set_service_uuid(uuid) != ESP_OK ||
      network_prov_mgr_start_provisioning(NETWORK_PROV_SECURITY_1, state.pin, state.name, nullptr) != ESP_OK) {
    network_prov_mgr_deinit();
    return false;
  }
  managerActive = true;
  state.provisioning = true;
  Serial.printf("Bluetooth setup: %s; PIN is on the screen.\n", state.name);
  return true;
}

// Downloads one feed and parses it into feedParser.result. Runs only in the network task.
bool fetchFeed(const char *url) {
  // A valid clock is required for certificate validity checking. Never use insecure TLS.
  if (time(nullptr) < 1704067200) return false;
  esp_http_client_config_t config = {};
  config.url = url;
  config.crt_bundle_attach = esp_crt_bundle_attach;
  config.timeout_ms = 3500;
  config.buffer_size = 1024;
  config.user_agent = "VanshOS/1.0 (personal OLED internet computer)";
  config.disable_auto_redirect = true;
  esp_http_client_handle_t client = esp_http_client_init(&config);
  if (!client) return false;
  esp_http_client_set_header(client, "Accept", "application/rss+xml, application/xml, text/xml");
  esp_http_client_set_header(client, "Accept-Encoding", "identity");
  bool ok = false;
  // Feed locations are fixed; unexpected redirects/HTML fail safely and retry later.
  if (esp_http_client_open(client, 0) == ESP_OK && esp_http_client_fetch_headers(client) >= 0 &&
      esp_http_client_get_status_code(client) == 200) {
    feedParser.reset();
    char buffer[768];
    size_t total = 0;
    uint32_t began = millis();
    bool finished = false;
    while (millis() - began < 15000 && total < MAX_FEED_BYTES) {
      int count = esp_http_client_read(client, buffer, sizeof(buffer));
      if (count < 0) break;
      if (count == 0) { finished = esp_http_client_is_complete_data_received(client); break; }
      total += count;
      feedParser.feed(buffer, (size_t)count);
      if (feedParser.result.count == vbot::NEWS_COUNT) { finished = true; break; }
      vTaskDelay(1);
    }
    ok = finished && feedParser.result.count;
  }
  esp_http_client_close(client);
  esp_http_client_cleanup(client);
  return ok;
}

void networkTask(void *) {
  Preferences preferences;
  bool storage = preferences.begin("vanshos", false);
  if (storage) {
    preferences.getString("ssid", savedSsid, sizeof(savedSsid));
    preferences.getString("password", savedPassword, sizeof(savedPassword));
    preferences.getString("pin", state.pin, sizeof(state.pin));
  }
  if (!*state.pin) {
    snprintf(state.pin, sizeof(state.pin), "%08lu", (unsigned long)(esp_random() % 100000000));
    if (storage) preferences.putString("pin", state.pin);
  }
  uint64_t mac = ESP.getEfuseMac();
  snprintf(state.name, sizeof(state.name), "VANSH_OS_%06lX", (unsigned long)(mac & 0xffffff));
  WiFi.onEvent(onEvent);
  WiFi.persistent(false); // Vansh OS owns its last-known-good credentials in Preferences.
  WiFi.mode(WIFI_STA);
  WiFi.setAutoReconnect(false); // Worker controls retry timing, including during provisioning.
  WiFi.setHostname("vansh-os");
  configTime(0, 0, "pool.ntp.org", "time.google.com");
  if (*savedSsid) WiFi.begin(savedSsid, savedPassword);

  uint32_t disconnectedAt = millis(), reconnectAt = millis(), setupAttemptAt = 0;
  uint32_t credentialAt = 0, failedAt = 0;
  uint32_t refreshFrom = 0, refreshWait = 0;   // when feed 0 is next refreshed
  uint32_t retryFrom = 0, retryWait = 0;       // back-off after a failed download
  uint8_t failures = 0;
  bool refreshArmed = false;
  bool wasConnected = false, credentialBusy = false, failurePending = false;
  bool setupAttempted = false;
  for (;;) {
    uint32_t now = millis();
    if (credentialsReceived.exchange(false)) { credentialBusy = true; credentialAt = now; }
    if (credentialsFailed.exchange(false)) {
      failurePending = true; failedAt = now;
      Serial.println("Wi-Fi setup failed. Check password/network and retry in the app.");
    }
    if (provisioningEnded.exchange(false)) {
      // Arduino WiFiGeneric deinitializes the manager before dispatching this event.
      managerActive = state.provisioning = false;
      credentialBusy = failurePending = false;
    }
    bool connected = WiFi.status() == WL_CONNECTED;
    if (gotIp.exchange(false) && connected) {
      String ssid = WiFi.SSID(), password = WiFi.psk();
      if (ssid.length() && (strcmp(savedSsid, ssid.c_str()) || strcmp(savedPassword, password.c_str()))) {
        ssid.toCharArray(savedSsid, sizeof(savedSsid));
        password.toCharArray(savedPassword, sizeof(savedPassword));
        if (storage) {
          preferences.putString("ssid", savedSsid);
          preferences.putString("password", savedPassword);
        }
      }
      credentialBusy = false;
      Serial.println("Wi-Fi connected. Vansh OS is online.");
    }
    if (!connected && wasConnected) { disconnectedAt = now; state.feedOk = false; }
    if (connected && !wasConnected) {
      // Give NTP a few seconds, then refresh the main feed (kept headlines stay visible).
      refreshFrom = now; refreshWait = 5000; refreshArmed = true;
    }
    wasConnected = connected;
    state.wifi = connected;

    if (managerActive && failurePending && now - failedAt >= 10000) {
      // Leave time for the phone to read the failure, then allow another password attempt.
      network_prov_mgr_reset_wifi_sm_state_on_failure();
      failurePending = credentialBusy = false;
      reconnectAt = now;
    }
    if (credentialBusy && now - credentialAt >= 60000 && !failurePending) {
      // An interrupted setup must not permanently suppress reconnect attempts.
      network_prov_mgr_stop_provisioning();
      credentialBusy = false;
    }
    if (!connected && !managerActive &&
        (!*savedSsid || now - disconnectedAt >= SETUP_AFTER_MS) &&
        (!setupAttempted || now - setupAttemptAt >= 30000)) {
      setupAttempted = true; setupAttemptAt = now;
      state.setupError = !beginSetup();
      reconnectAt = now;
    }
    if (!connected && *savedSsid && !credentialBusy && !failurePending && now - reconnectAt >= RECONNECT_MS) {
      reconnectAt = now;
      WiFi.begin(savedSsid, savedPassword);
    }

    // "No more stories" = the first load worked and every source was fetched recently.
    bool anyEligible = false;
    for (size_t f = 0; f < FEED_COUNT; ++f) anyEligible = anyEligible || feedEligible(f, now);
    state.exhausted = feedKnown[0] && !anyEligible;

    // Publish before doing blocking TLS work, so display can show setup/network changes.
    xQueueOverwrite(snapshots, &state);

    if (connected && !state.provisioning && now - retryFrom >= retryWait) {
      int feed = -1;
      if (refreshArmed && now - refreshFrom >= refreshWait) feed = 0;          // newest stories
      else if (moreRequested.exchange(false)) feed = pickFeed(now);            // user near the end
      if (feed >= 0) {
        state.fetching = true;
        xQueueOverwrite(snapshots, &state);   // UI shows "LOADING..." during the download
        bool ok = fetchFeed(FEEDS[feed]);
        uint32_t done = millis();
        if (ok) {
          size_t added = ingest(feedParser.result);
          feedFetchedAt[feed] = done; feedKnown[feed] = true;
          failures = 0; retryWait = 0;
          state.feedOk = true; state.fetchFailed = false;
          state.fetchedAt = done;
          if (added) ++state.revision;
          if (feed == 0) { refreshArmed = true; refreshFrom = done; refreshWait = FETCH_MS; }
          else nextFeed = ((size_t)feed + 1) % FEED_COUNT;
          Serial.printf("Feed %d: %u new headlines.\n", feed, (unsigned)added);
        } else {
          state.feedOk = false; state.fetchFailed = true;
          if (failures < 6) ++failures;
          uint32_t wait = RETRY_MIN_MS << (failures - 1);
          retryWait = wait < RETRY_MAX_MS ? wait : RETRY_MAX_MS;
          retryFrom = done;
          Serial.println("News unavailable; will retry.");
        }
        state.fetching = false;
        xQueueOverwrite(snapshots, &state);
      }
    }
    vTaskDelay(pdMS_TO_TICKS(200));
  }
}
} // namespace

bool startNetwork() {
  storeLock = xSemaphoreCreateMutex();
  if (!storeLock) return false;
  snapshots = xQueueCreate(1, sizeof(NetworkSnapshot));
  if (!snapshots) return false;
  // Priority 1 plus yielding leaves time for the display even on a single application core.
  if (xTaskCreate(networkTask, "vansh-net", 16384, nullptr, 1, nullptr) != pdPASS) {
    vQueueDelete(snapshots); snapshots = nullptr; return false;
  }
  return true;
}

bool readNetwork(NetworkSnapshot &snapshot) {
  return snapshots && xQueueReceive(snapshots, &snapshot, 0) == pdTRUE;
}

bool newsTitle(uint32_t number, char *out, size_t cap) {
  if (!storeLock || !out || !cap) return false;
  bool ok = false;
  // The worker only holds the lock for a short memcpy/memmove, never during network I/O.
  if (xSemaphoreTake(storeLock, pdMS_TO_TICKS(20)) == pdTRUE) {
    if (number >= store.first && number - store.first < store.count) {
      const char *src = store.titles[number - store.first];
      size_t n = strnlen(src, cap - 1);
      memcpy(out, src, n);
      out[n] = 0;
      ok = true;
    }
    xSemaphoreGive(storeLock);
  }
  return ok;
}

void requestMoreNews() {
  moreRequested.store(true);
}

// ───────────────────────── UI + two-button input ─────────────────────────
#include <Wire.h>
#include <U8g2lib.h>

void centered(const char *text, int y);
void moveAction(int dir);
void selectAction();
void backAction();
void clampNewsPos();
void updateOS(uint32_t now);
void render();
void setup();
void loop();

constexpr uint8_t BRIGHTNESS = 150;
constexpr uint8_t DOWN_BUTTON = 4;     // click: down / next page   hold: select
constexpr uint8_t UP_BUTTON = 27;      // click: up / previous page hold: back
constexpr uint32_t DEBOUNCE_MS = 25;
constexpr uint32_t LONG_PRESS_MS = 700;
constexpr uint8_t ROWS_PER_PAGE = 5;
constexpr uint32_t NEWS_PREFETCH = 3;           // ask for more when this close to the end
constexpr uint32_t PREFETCH_EVERY_MS = 250;     // throttle for the (cheap) request flag

constexpr uint8_t BTN_NONE = 0, BTN_SHORT = 1, BTN_LONG = 2;

// One button, one set of rules for BOTH buttons:
//   - debounced (state must be stable for DEBOUNCE_MS)
//   - SHORT fires when released before LONG_PRESS_MS
//   - LONG fires once, while still held, after LONG_PRESS_MS (no SHORT afterwards)
struct Button {
  uint8_t pin = 0;
  bool raw = false, stable = false, longFired = false;
  uint32_t changedAt = 0, downAt = 0;

  void begin(uint8_t p) {
    pin = p;
    pinMode(pin, INPUT_PULLUP);
    raw = stable = digitalRead(pin) == LOW;
    longFired = stable;   // held during boot: ignore it until it is released
  }

  uint8_t poll(uint32_t now) {
    bool reading = digitalRead(pin) == LOW;
    if (reading != raw) { raw = reading; changedAt = now; }
    if (raw != stable && now - changedAt >= DEBOUNCE_MS) {
      stable = raw;
      if (stable) { downAt = now; longFired = false; }
      else if (!longFired) return BTN_SHORT;
    }
    if (stable && !longFired && now - downAt >= LONG_PRESS_MS) {
      longFired = true;
      return BTN_LONG;
    }
    return BTN_NONE;
  }
};

Button downButton, upButton;

U8G2_SH1106_128X64_NONAME_F_HW_I2C oled(U8G2_R2, U8X8_PIN_NONE);
NetworkSnapshot network;
bool networkStarted = false;
uint32_t frameAt = 0;

char headlineRows[20][25] = {};
size_t headlineLines = 0;
uint8_t readPage = 0;

enum Screen { HOME, NEWS_LIST, NEWS_READ, WIFI_INFO, SETUP_INFO, ABOUT };
Screen screen = HOME;
uint8_t menuIndex = 0;
// Absolute headline number (not an array index), so new/dropped headlines never move the user.
uint32_t newsPos = 0;
const char *const menuItems[] = {"Tech headlines", "Wi-Fi status", "Connect Wi-Fi", "About Vansh OS"};
constexpr uint8_t MENU_COUNT = sizeof(menuItems) / sizeof(menuItems[0]);

void centered(const char *text, int y) {
  oled.drawStr(max(0, (128 - (int)oled.getStrWidth(text)) / 2), y, text);
}

// Keep newsPos inside the headlines that currently exist.
void clampNewsPos() {
  if (!network.newsCount) { newsPos = network.newsFirst; return; }
  uint32_t last = network.newsFirst + network.newsCount - 1;
  if (newsPos < network.newsFirst) newsPos = network.newsFirst;
  else if (newsPos > last) newsPos = last;
}

// DOWN click (+1) / UP click (-1)
void moveAction(int dir) {
  if (screen == HOME) {
    menuIndex = (menuIndex + MENU_COUNT + dir) % MENU_COUNT;
  } else if (screen == NEWS_LIST && network.newsCount) {
    uint32_t last = network.newsFirst + network.newsCount - 1;
    if (dir < 0) {
      if (newsPos > network.newsFirst) --newsPos;   // stops at the top, no wrap
    } else if (newsPos < last) {
      ++newsPos;
    } else if (!network.fetching && (network.exhausted || network.fetchFailed || !network.wifi)) {
      newsPos = network.newsFirst;   // truly the end: wrap to the oldest kept headline
    } else {
      requestMoreNews();             // more is on the way: stay here, header shows LOADING
    }
  } else if (screen == NEWS_READ) {
    size_t pages = (headlineLines + ROWS_PER_PAGE - 1) / ROWS_PER_PAGE;
    if (pages) readPage = (readPage + (dir > 0 ? 1 : pages - 1)) % pages;
  }
}

// DOWN hold: open the highlighted item
void selectAction() {
  if (screen == HOME) {
    screen = menuIndex == 0 ? NEWS_LIST
           : menuIndex == 1 ? WIFI_INFO
           : menuIndex == 2 ? SETUP_INFO
                            : ABOUT;
  } else if (screen == NEWS_LIST) {
    clampNewsPos();
    char title[vbot::TITLE_SIZE];
    if (network.newsCount && newsTitle(newsPos, title, sizeof(title))) {
      headlineLines = vbot::wrap(title, headlineRows, 20);
      readPage = 0;
      screen = NEWS_READ;
    }
  }
}

// UP hold: go back one level
void backAction() {
  if (screen == NEWS_READ) screen = NEWS_LIST;
  else if (screen != HOME) screen = HOME;
}

void updateOS(uint32_t now) {
  readNetwork(network);
  clampNewsPos();

  // Infinite feed: when the user is within the last few headlines, ask the worker for more.
  // This only sets a flag; all downloading happens in the network task.
  static uint32_t prefetchAt = 0;
  if ((screen == NEWS_LIST || screen == NEWS_READ) && network.wifi && network.newsCount &&
      now - prefetchAt >= PREFETCH_EVERY_MS) {
    prefetchAt = now;
    uint32_t last = network.newsFirst + network.newsCount - 1;
    if (last - newsPos < NEWS_PREFETCH) requestMoreNews();
  }

  uint8_t event = downButton.poll(now);
  if (event == BTN_SHORT) moveAction(+1);
  else if (event == BTN_LONG) selectAction();

  event = upButton.poll(now);
  if (event == BTN_SHORT) moveAction(-1);
  else if (event == BTN_LONG) backAction();
}

void render() {
  oled.clearBuffer(); oled.setFont(u8g2_font_5x8_tf); oled.setContrast(BRIGHTNESS);
  if (screen == HOME) {
    oled.drawStr(2, 8, "VANSH OS");
    const char *status = !networkStarted || network.setupError ? "NET ERROR"
                       : network.provisioning ? "SETUP"
                       : !network.wifi ? "OFFLINE"
                       : network.feedOk ? "NEWS OK" : "ONLINE";
    oled.drawStr(126 - oled.getStrWidth(status), 8, status);
    for (uint8_t i = 0; i < MENU_COUNT; ++i) {
      int y = 21 + i * 10;
      if (i == menuIndex) oled.drawBox(3, y - 8, 122, 10);
      oled.setDrawColor(i == menuIndex ? 0 : 1);
      oled.drawStr(7, y, menuItems[i]); oled.setDrawColor(1);
    }
    centered("UP/DN move  DN hold:open", 63);
  } else if (screen == NEWS_LIST) {
    bool atEnd = !network.newsCount || newsPos + 1 >= network.newsFirst + network.newsCount;
    const char *head = "TECH HEADLINES";
    if (network.fetching) head = "LOADING...";
    else if (network.fetchFailed) head = "FETCH FAILED-RETRY";
    else if (atEnd && !network.wifi) head = "OFFLINE";
    else if (atEnd && network.exhausted) head = "NO MORE STORIES";
    oled.drawStr(2, 9, head);
    if (!network.newsCount) {
      centered(!network.wifi ? "Connect to Wi-Fi"
             : network.fetchFailed ? "Retrying..." : "Loading news...", 34);
    } else {
      char pos[16];
      snprintf(pos, sizeof(pos), "%lu/%lu", (unsigned long)(newsPos - network.newsFirst + 1),
               (unsigned long)network.newsCount);
      oled.drawStr(126 - oled.getStrWidth(pos), 9, pos);
      size_t rel = newsPos - network.newsFirst;
      size_t top = rel > 3 ? rel - 3 : 0;
      char title[vbot::TITLE_SIZE];
      for (size_t i = top; i < network.newsCount && i < top + 5; ++i) {
        int y = 19 + (int)(i - top) * 9;
        oled.drawStr(2, y, network.newsFirst + i == newsPos ? ">" : " ");
        if (newsTitle(network.newsFirst + i, title, sizeof(title))) {
          char rows[2][25] = {};
          vbot::wrap(title, rows, 2);
          oled.drawStr(12, y, rows[0]);
        }
      }
    }
    centered("DN hold:read UP hold:back", 63);
  } else if (screen == NEWS_READ) {
    size_t pages = (headlineLines + ROWS_PER_PAGE - 1) / ROWS_PER_PAGE;
    char title[24];
    snprintf(title, sizeof(title), "TECH HEADLINE %u/%u", (unsigned)(readPage + 1), (unsigned)(pages ? pages : 1));
    oled.drawStr(2, 8, title);
    for (size_t i = 0; i < ROWS_PER_PAGE && (size_t)readPage * ROWS_PER_PAGE + i < headlineLines; ++i)
      oled.drawStr(2, 20 + i * 9, headlineRows[readPage * ROWS_PER_PAGE + i]);
    centered("UP/DN:page  UP hold:back", 63);
  } else if (screen == WIFI_INFO) {
    centered("WI-FI STATUS", 10);
    centered(network.wifi ? "Connected" : network.provisioning ? "Setup active" : "Offline", 27);
    if (network.wifi) centered(WiFi.SSID().c_str(), 40);
    centered(network.feedOk ? "Tech news ready" : "News not available", 54);
    centered("UP hold: back", 63);
  } else if (screen == SETUP_INFO) {
    centered("PHONE WI-FI SETUP", 8); centered("ESP Provisioning app", 21);
    centered(network.name, 33);
    char pin[24]; snprintf(pin, sizeof(pin), "PIN: %s", network.pin); centered(pin, 45);
    centered("2.4GHz  UP hold:back", 60);
  } else {
    centered("VANSH OS", 15); centered("Internet mini computer", 30);
    centered("D4 down  D27 up", 45); centered("Hold D4 OK, D27 back", 58);
  }
  oled.sendBuffer();
}

void setup() {
  Serial.begin(115200);
  downButton.begin(DOWN_BUTTON);
  upButton.begin(UP_BUTTON);
  Wire.begin(21, 22); oled.setI2CAddress(0x3C << 1); oled.setBusClock(400000);
  oled.begin(); oled.setContrast(BRIGHTNESS); render();
  networkStarted = startNetwork();
  Serial.println(networkStarted ? "Vansh OS ready." : "Network worker failed.");
}

void loop() {
  uint32_t now = millis(); updateOS(now);
  if (now - frameAt >= 100) { frameAt = now; render(); }
  delay(1);
}
