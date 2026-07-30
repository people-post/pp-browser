#include "base/media/CameraCaptureOrientation.h"

#if defined(__ANDROID__)

#include <camera/NdkCameraManager.h>
#include <camera/NdkCameraMetadata.h>
#include <camera/NdkCameraMetadataTags.h>
#include <jni.h>

#include <SDL3/SDL.h>

#include <cstdint>

namespace pbr {
namespace {

int DisplayRotationDegrees() {
  // Prefer JNI Surface.ROTATION_* — matches CameraX getRotationCompensation.
  JNIEnv* env = static_cast<JNIEnv*>(SDL_GetAndroidJNIEnv());
  jobject activity = static_cast<jobject>(SDL_GetAndroidActivity());
  if (!env || !activity) {
    return 0;
  }

  jclass activity_cls = env->GetObjectClass(activity);
  if (!activity_cls) {
    return 0;
  }

  int degrees = 0;
  jmethodID get_wm = env->GetMethodID(activity_cls, "getWindowManager",
                                      "()Landroid/view/WindowManager;");
  if (!get_wm) {
    env->ExceptionClear();
    env->DeleteLocalRef(activity_cls);
    return 0;
  }
  jobject wm = env->CallObjectMethod(activity, get_wm);
  if (!wm) {
    env->DeleteLocalRef(activity_cls);
    return 0;
  }
  jclass wm_cls = env->GetObjectClass(wm);
  jmethodID get_display =
      env->GetMethodID(wm_cls, "getDefaultDisplay", "()Landroid/view/Display;");
  if (!get_display) {
    env->ExceptionClear();
    env->DeleteLocalRef(wm_cls);
    env->DeleteLocalRef(wm);
    env->DeleteLocalRef(activity_cls);
    return 0;
  }
  jobject display = env->CallObjectMethod(wm, get_display);
  if (!display) {
    env->DeleteLocalRef(wm_cls);
    env->DeleteLocalRef(wm);
    env->DeleteLocalRef(activity_cls);
    return 0;
  }
  jclass display_cls = env->GetObjectClass(display);
  jmethodID get_rotation = env->GetMethodID(display_cls, "getRotation", "()I");
  if (get_rotation) {
    const jint rotation = env->CallIntMethod(display, get_rotation);
    // android.view.Surface.ROTATION_0/90/180/270
    switch (rotation) {
    case 1:
      degrees = 90;
      break;
    case 2:
      degrees = 180;
      break;
    case 3:
      degrees = 270;
      break;
    default:
      degrees = 0;
      break;
    }
  } else {
    env->ExceptionClear();
  }
  env->DeleteLocalRef(display_cls);
  env->DeleteLocalRef(display);
  env->DeleteLocalRef(wm_cls);
  env->DeleteLocalRef(wm);
  env->DeleteLocalRef(activity_cls);
  return degrees;
}

bool QuerySensorOrientation(uint8_t want_facing, int32_t* out_sensor_deg) {
  if (!out_sensor_deg) {
    return false;
  }
  ACameraManager* mgr = ACameraManager_create();
  if (!mgr) {
    return false;
  }
  ACameraIdList* list = nullptr;
  if (ACameraManager_getCameraIdList(mgr, &list) != ACAMERA_OK || !list) {
    ACameraManager_delete(mgr);
    return false;
  }

  bool found = false;
  for (int i = 0; i < list->numCameras && !found; ++i) {
    const char* id = list->cameraIds[i];
    ACameraMetadata* meta = nullptr;
    if (ACameraManager_getCameraCharacteristics(mgr, id, &meta) != ACAMERA_OK || !meta) {
      continue;
    }
    ACameraMetadata_const_entry facing_entry{};
    ACameraMetadata_const_entry orient_entry{};
    const bool have_facing =
        ACameraMetadata_getConstEntry(meta, ACAMERA_LENS_FACING, &facing_entry) == ACAMERA_OK &&
        facing_entry.count > 0;
    const bool have_orient =
        ACameraMetadata_getConstEntry(meta, ACAMERA_SENSOR_ORIENTATION, &orient_entry) ==
            ACAMERA_OK &&
        orient_entry.count > 0;
    if (have_facing && have_orient && facing_entry.data.u8[0] == want_facing) {
      *out_sensor_deg = orient_entry.data.i32[0];
      found = true;
    }
    ACameraMetadata_free(meta);
  }

  ACameraManager_deleteCameraIdList(list);
  ACameraManager_delete(mgr);
  return found;
}

int Normalize90(int deg) {
  deg %= 360;
  if (deg < 0) {
    deg += 360;
  }
  // Snap to nearest quarter-turn.
  const int snapped = ((deg + 45) / 90) * 90;
  return snapped % 360;
}

} // namespace

CameraCaptureTransform ResolveCameraCaptureTransform(SDL_CameraID camera_id) {
  CameraCaptureTransform t;
  t.encode_width = 360;
  t.encode_height = 640;

  const SDL_CameraPosition pos = SDL_GetCameraPosition(camera_id);
  const bool front = (pos != SDL_CAMERA_POSITION_BACK_FACING);
  const uint8_t want_facing =
      front ? ACAMERA_LENS_FACING_FRONT : ACAMERA_LENS_FACING_BACK;

  int32_t sensor_deg = front ? 270 : 90; // conventional phone fallback
  (void)QuerySensorOrientation(want_facing, &sensor_deg);
  sensor_deg = Normalize90(sensor_deg);

  const int display_deg = Normalize90(DisplayRotationDegrees());

  // CameraX / Camera2 rotation compensation for upright buffers.
  int rotate_cw = 0;
  if (front) {
    rotate_cw = (sensor_deg + display_deg) % 360;
  } else {
    rotate_cw = (sensor_deg - display_deg + 360) % 360;
  }
  t.rotate_cw = Normalize90(rotate_cw);

  // After a 90/270 rotate, portrait encode matches phone UI; otherwise keep landscape.
  if (t.rotate_cw == 0 || t.rotate_cw == 180) {
    t.encode_width = 640;
    t.encode_height = 360;
  }
  return t;
}

} // namespace pbr

#endif // __ANDROID__
