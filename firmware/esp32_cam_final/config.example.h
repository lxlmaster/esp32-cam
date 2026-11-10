#pragma once
// Copy to config.local.h (git-ignored). Rebuild/upload after changing settings.
#define MOTION_ENABLED true
#define MOTION_WEBHOOK_URL ""  // e.g. http://192.168.1.10:8080/camera
#define MOTION_WEBHOOK_TOKEN ""
// For HTTPS, provide the server's trusted root CA PEM. Certificate checks stay on.
#define MOTION_WEBHOOK_CA ""
