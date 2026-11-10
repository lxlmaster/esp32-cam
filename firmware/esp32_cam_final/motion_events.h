#pragma once
#include <Arduino.h>

// Optional secrets/settings in ignored config.local.h; see config.example.h.
#if __has_include("config.local.h")
#include "config.local.h"
#endif
#ifndef MOTION_ENABLED
#define MOTION_ENABLED true
#endif
#ifndef MOTION_WEBHOOK_URL
#define MOTION_WEBHOOK_URL ""
#endif
#ifndef MOTION_WEBHOOK_TOKEN
#define MOTION_WEBHOOK_TOKEN ""
#endif
#ifndef MOTION_WEBHOOK_CA
#define MOTION_WEBHOOK_CA ""
#endif

bool motionEventsBegin();
void motionEventsCapture(const uint8_t *jpeg, size_t len, uint8_t score, uint32_t now);
bool motionEventsCopy(uint8_t **jpeg, size_t *len);
void motionEventsStatus(char *json, size_t capacity);
