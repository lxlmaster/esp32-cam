#include "../firmware/esp32_cam_final/camera_role.h"
#include <assert.h>
#include <string.h>
#include <stdio.h>

int main() {
  assert(!cameraDetectionEnabled(false));
#if CAMERA_ROLE == CAMERA_ROLE_STREAM
  assert(!cameraDetectionEnabled(true));
  assert(cameraShouldEncode(false, false));
  assert(strcmp(cameraRoleName(), "stream") == 0);
#elif CAMERA_ROLE == CAMERA_ROLE_DETECTOR
  assert(cameraDetectionEnabled(true));
  assert(!cameraShouldEncode(false, false));
  assert(cameraShouldEncode(true, false));
  assert(cameraShouldEncode(false, true));
  assert(strcmp(cameraRoleName(), "detector") == 0);
#else
  assert(cameraDetectionEnabled(true));
  assert(cameraShouldEncode(false, false));
  assert(strcmp(cameraRoleName(), "hybrid") == 0);
#endif
  assert(!cameraPreviewActive(1, 0, false));
  assert(cameraPreviewActive(2000, 1, true));
  assert(!cameraPreviewActive(2001, 1, true));
  assert(cameraPreviewActive(500, UINT32_MAX - 1000, true));
  assert(!cameraPreviewActive(1000, UINT32_MAX - 1000, true));
  printf("camera role %s: encoding/detection/preview expiry passed\n", cameraRoleName());
}
