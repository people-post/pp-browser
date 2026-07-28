package dev.pp_browser.app;

import android.util.Log;

import androidx.annotation.NonNull;

import com.google.firebase.messaging.FirebaseMessagingService;
import com.google.firebase.messaging.RemoteMessage;

/**
 * Receives opaque FCM data wakes ({@code type=inbox_wake} / {@code type=call_wake}) and token refreshes.
 * Enabled only when google-services.json is present (see app/build.gradle).
 */
public class PpFirebaseMessagingService extends FirebaseMessagingService {
    private static final String TAG = "pp-browser-fcm";

    @Override
    public void onNewToken(@NonNull String token) {
        Log.i(TAG, "FCM token refreshed");
        PpPushBridge.onNewToken(token);
    }

    @Override
    public void onMessageReceived(@NonNull RemoteMessage message) {
        String type = message.getData().get("type");
        if ("call_wake".equals(type)) {
            PpPushBridge.onCallWake();
        } else if ("inbox_wake".equals(type) || message.getData().isEmpty()) {
            PpPushBridge.onInboxWake();
        }
    }
}
