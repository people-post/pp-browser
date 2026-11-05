package dev.pp_browser.app;

import android.content.Context;

/**
 * JNI bridge for FCM token + wake. Native methods live in libmain.so, loaded only once the app's activity
 * started: a push can start the process without it.
 */
public final class PpPushBridge {
    private PpPushBridge() {}

    public static native void nativeSetFcmToken(String token);

    public static native void nativeOnPushWake();

    public static native void nativeOnCallWake();

    public static void onNewToken(String token) {
        if (token == null || token.isEmpty()) {
            return;
        }
        try {
            nativeSetFcmToken(token);
        } catch (UnsatisfiedLinkError ignored) {
            // Native not loaded yet; token will be re-fetched after start.
        }
    }

    public static void onInboxWake() {
        try {
            nativeOnPushWake();
        } catch (UnsatisfiedLinkError ignored) {
        }
    }

    /**
     * FCM {@code inbox_wake}. The app running: it syncs and shows what came. Not running (the process started for
     * this push, nothing unlocked): only a generic "New message" can show, as on iOS.
     */
    public static void onInboxPush(Context context) {
        if (MainActivity.isAppRunning()) {
            onInboxWake();
        } else {
            MainActivity.showPushMessageNotification(context);
        }
    }

    /**
     * FCM {@code call_wake}: "Incoming call" rings now (PpCallNotifications) unless the app is in front, and the app
     * syncs to find the ring when it runs; not running, the full-screen notification starts it.
     */
    public static void onCallPush(Context context) {
        PpCallNotifications.onCallPush(context, MainActivity.isAppInFront());
        if (MainActivity.isAppRunning()) {
            onCallWake();
        }
    }

    public static void onCallWake() {
        try {
            nativeOnCallWake();
        } catch (UnsatisfiedLinkError ignored) {
        }
    }
}
