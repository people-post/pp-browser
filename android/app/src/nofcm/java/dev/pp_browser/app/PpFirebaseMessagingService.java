package dev.pp_browser.app;

import android.app.Service;
import android.content.Intent;
import android.os.IBinder;

/**
 * Placeholder when google-services.json is absent so the manifest component resolves.
 * Real FCM implementation lives under src/fcm when ENABLE_FCM is true.
 */
public class PpFirebaseMessagingService extends Service {
    @Override
    public IBinder onBind(Intent intent) {
        return null;
    }
}
