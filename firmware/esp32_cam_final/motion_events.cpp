#include "motion_events.h"
#include <HTTPClient.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <time.h>
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

namespace {
constexpr size_t CAPACITY = 256 * 1024;
struct Event { uint8_t *jpeg; size_t len; uint32_t id; uint32_t at; uint8_t score; };
SemaphoreHandle_t lock = nullptr;
QueueHandle_t queue = nullptr;
uint8_t *photo = nullptr;
size_t photoLen = 0;
uint32_t events = 0, lastAt = 0, sent = 0, failed = 0, dropped = 0;
uint8_t lastScore = 0;
int lastHttp = 0;

int postEvent(const Event &event) {
  if (WiFi.status() != WL_CONNECTED) return -1;
  String url(MOTION_WEBHOOK_URL);
  HTTPClient http;
  WiFiClient plain;
  WiFiClientSecure secure;
  http.setConnectTimeout(3000);
  http.setTimeout(5000);
  if (url.startsWith("https://")) {
    if (!strlen(MOTION_WEBHOOK_CA)) return -2;
    static bool clockStarted = false;
    if (!clockStarted) {
      configTime(0, 0, "pool.ntp.org", "time.cloudflare.com");
      clockStarted = true;
    }
    if (time(nullptr) < 1704067200) return -5; // wait for SNTP certificate time
    secure.setCACert(MOTION_WEBHOOK_CA);
    secure.setHandshakeTimeout(5);
    if (!http.begin(secure, url)) return -3;
  } else if (url.startsWith("http://")) {
    if (!http.begin(plain, url)) return -3;
  } else return -4;
  http.addHeader("Content-Type", "image/jpeg");
  http.addHeader("X-Camera-Event", String(event.id));
  http.addHeader("X-Camera-Uptime-Ms", String(event.at));
  http.addHeader("X-Motion-Score", String(event.score));
  if (strlen(MOTION_WEBHOOK_TOKEN))
    http.addHeader("Authorization", String("Bearer ") + MOTION_WEBHOOK_TOKEN);
  int result = http.POST(event.jpeg, event.len);
  http.end();
  return result;
}

void pushTask(void *) {
  Event event;
  while (true) {
    if (xQueueReceive(queue, &event, portMAX_DELAY) != pdTRUE) continue;
    int result = postEvent(event);
    free(event.jpeg);
    xSemaphoreTake(lock, portMAX_DELAY);
    lastHttp = result;
    if (result >= 200 && result < 300) ++sent; else ++failed;
    xSemaphoreGive(lock);
    Serial.printf("[MOTION] event=%u HTTP=%d\n", event.id, result);
  }
}
}

bool motionEventsBegin() {
  photo = (uint8_t *)ps_malloc(CAPACITY);
  lock = xSemaphoreCreateMutex();
  if (!photo || !lock) return false;
  if (!strlen(MOTION_WEBHOOK_URL)) return true;
  queue = xQueueCreate(1, sizeof(Event));
  return queue && xTaskCreatePinnedToCore(pushTask, "motionPush", 8192, nullptr, 1,
                                          nullptr, 0) == pdPASS;
}

void motionEventsCapture(const uint8_t *jpeg, size_t len, uint8_t score, uint32_t now) {
  if (!jpeg || !len || len > CAPACITY) return;
  xSemaphoreTake(lock, portMAX_DELAY);
  memcpy(photo, jpeg, len);
  photoLen = len; lastAt = now; lastScore = score; ++events;
  uint32_t id = events;
  xSemaphoreGive(lock);
  Serial.printf("[MOTION] event=%u score=%u jpeg=%u\n", id, score, unsigned(len));
  if (!queue) return;
  if (uxQueueSpacesAvailable(queue) == 0) {
    xSemaphoreTake(lock, portMAX_DELAY); ++dropped; xSemaphoreGive(lock);
    return;
  }
  // Only event frames allocate; live viewing uses preallocated frame slots.
  Event event = { (uint8_t *)ps_malloc(len), len, id, now, score };
  if (event.jpeg) memcpy(event.jpeg, jpeg, len);
  if (!event.jpeg || xQueueSend(queue, &event, 0) != pdTRUE) {
    free(event.jpeg);
    xSemaphoreTake(lock, portMAX_DELAY); ++dropped; xSemaphoreGive(lock);
  }
}

bool motionEventsCopy(uint8_t **jpeg, size_t *len) {
  xSemaphoreTake(lock, portMAX_DELAY);
  *len = photoLen;
  *jpeg = photoLen ? (uint8_t *)ps_malloc(photoLen) : nullptr;
  if (*jpeg) memcpy(*jpeg, photo, photoLen);
  xSemaphoreGive(lock);
  return *jpeg != nullptr;
}

void motionEventsStatus(char *json, size_t capacity) {
  xSemaphoreTake(lock, portMAX_DELAY);
  snprintf(json, capacity,
    "{\"enabled\":%s,\"webhook_configured\":%s,\"events\":%u,\"last_uptime_ms\":%u,"
    "\"last_score\":%u,\"photo_bytes\":%u,\"sent\":%u,\"failed\":%u,\"dropped\":%u,\"last_http\":%d}",
    MOTION_ENABLED ? "true" : "false", strlen(MOTION_WEBHOOK_URL) ? "true" : "false",
    events, lastAt, lastScore, unsigned(photoLen), sent, failed, dropped, lastHttp);
  xSemaphoreGive(lock);
}
