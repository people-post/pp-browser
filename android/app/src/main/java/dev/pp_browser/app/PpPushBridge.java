package dev.pp_browser.app;

/**
 * JNI bridge for FCM token + wake. Native methods live in libmain.so.
 */
public final class PpPushBridge {
    private PpPushBridge() {}

    public static native void nativeSetFcmToken(String token);

    public static native void nativeOnPushWake();

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
}
