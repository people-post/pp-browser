package dev.pp_browser.app;

import android.util.Log;

import androidx.annotation.NonNull;

import com.google.firebase.messaging.FirebaseMessaging;
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
            PpPushBridge.onCallPush(this);
        } else if ("inbox_wake".equals(type) || message.getData().isEmpty()) {
            PpPushBridge.onInboxPush(this);
        }
    }

    /**
     * The current token, each start (MainActivity, its native side loaded): onNewToken comes only when the token
     * changes, often before the app's native side is there to take it.
     */
    static void requestToken() {
        try {
            FirebaseMessaging.getInstance().getToken().addOnSuccessListener(PpPushBridge::onNewToken);
        } catch (IllegalStateException e) {
            Log.w(TAG, "FCM not initialised: " + e);
        }
    }
}
