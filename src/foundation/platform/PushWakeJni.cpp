#include "foundation/runtime/BackgroundSyncScheduler.h"

#if defined(__ANDROID__)
#include <jni.h>

extern "C" JNIEXPORT void JNICALL Java_dev_pp_1browser_app_PpPushBridge_nativeOnPushWake(JNIEnv*, jclass) {
  pbr::BackgroundSyncScheduler::Instance().RequestWakeSync();
}

extern "C" JNIEXPORT void JNICALL Java_dev_pp_1browser_app_PpPushBridge_nativeOnCallWake(JNIEnv*, jclass) {
  pbr::BackgroundSyncScheduler::Instance().RequestCallWakeSync();
}
#endif
