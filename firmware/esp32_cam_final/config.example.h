#pragma once
// Copy to config.local.h (git-ignored). Rebuild/upload after changing settings.
// Board A: 1 = stream only. Board B: 2 = detect + event JPEG only.
// 0 = legacy combined mode.
#define CAMERA_ROLE 1
#define CAMERA_WIFI_SSID "YOUR_WIFI_SSID"
#define CAMERA_WIFI_PASSWORD "YOUR_WIFI_PASSWORD"
#define CAMERA_AUTH_USER "cam"
#define CAMERA_AUTH_PASSWORD "YOUR_CAM_PASSWORD"
#define MOTION_ENABLED true
#define MOTION_WEBHOOK_URL ""  // e.g. http://192.168.1.10:8080/camera
#define MOTION_WEBHOOK_TOKEN ""
// For HTTPS, provide the server's trusted root CA PEM. Certificate checks stay on.
#define MOTION_WEBHOOK_CA ""
