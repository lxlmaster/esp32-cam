#pragma once
#include <stdint.h>

#if __has_include("config.local.h")
#include "config.local.h"
#endif

#define CAMERA_ROLE_HYBRID 0
#define CAMERA_ROLE_STREAM 1
#define CAMERA_ROLE_DETECTOR 2
#ifndef CAMERA_ROLE
#define CAMERA_ROLE CAMERA_ROLE_HYBRID
#endif
#if CAMERA_ROLE < 0 || CAMERA_ROLE > 2
#error "CAMERA_ROLE must be 0 (hybrid), 1 (stream), or 2 (detector)"
#endif
#ifndef CAMERA_WIFI_SSID
#define CAMERA_WIFI_SSID "1001"
#endif
#ifndef CAMERA_WIFI_PASSWORD
#define CAMERA_WIFI_PASSWORD "YOUR_WIFI_PASSWORD"
#endif
#ifndef CAMERA_AUTH_USER
#define CAMERA_AUTH_USER "cam"
#endif
#ifndef CAMERA_AUTH_PASSWORD
#define CAMERA_AUTH_PASSWORD "YOUR_CAM_PASSWORD"
#endif

inline const char *cameraRoleName() {
  return CAMERA_ROLE == CAMERA_ROLE_STREAM ? "stream" :
         CAMERA_ROLE == CAMERA_ROLE_DETECTOR ? "detector" : "hybrid";
}
inline bool cameraDetectionEnabled(bool enabled) {
  return enabled && CAMERA_ROLE != CAMERA_ROLE_STREAM;
}
inline bool cameraContinuousEncoding() { return CAMERA_ROLE != CAMERA_ROLE_DETECTOR; }
inline bool cameraShouldEncode(bool motion, bool previewRequested) {
  return cameraContinuousEncoding() || motion || previewRequested;
}
inline bool cameraPreviewActive(uint32_t now, uint32_t requestedAt, bool requested) {
  return requested && uint32_t(now - requestedAt) < 2000;
}
