package dev.pp_browser.app;

import android.app.Notification;
import android.app.NotificationChannel;
import android.app.NotificationManager;
import android.app.PendingIntent;
import android.app.Service;
import android.content.Context;
import android.content.Intent;
import android.content.pm.ServiceInfo;
import android.os.Build;
import android.os.IBinder;
import android.util.Log;
import androidx.core.app.NotificationCompat;
import androidx.core.content.ContextCompat;

/**
 * P011 — "Stay connected in the background" (opt-in): a foreground service whose ongoing
 * notification keeps PP's process working while it is in the background, so calls and messages
 * reach phones without a push service. Started / stopped from native (AppLifecycle's setting) via
 * MainActivity.setStayConnected.
 */
public final class PpConnectionService extends Service {
    private static final String TAG = "PpConnectionService";
    private static final String CHANNEL_CONNECTION = "connection";
    private static final int NOTIFICATION_ID = 0x5050;

    static void start(Context context) {
        try {
            ContextCompat.startForegroundService(context, new Intent(context, PpConnectionService.class));
        } catch (RuntimeException e) {
            // Android 12+: not allowed while the app is in the background; the next start (the app
            // in front, the setting re-applied at unlock) brings it up.
            Log.w(TAG, "start refused: " + e);
        }
    }

    static void stop(Context context) {
        context.stopService(new Intent(context, PpConnectionService.class));
    }

    @Override
    public int onStartCommand(Intent intent, int flags, int startId) {
        ensureChannel();
        Intent open = new Intent(this, MainActivity.class);
        open.setFlags(Intent.FLAG_ACTIVITY_SINGLE_TOP | Intent.FLAG_ACTIVITY_CLEAR_TOP);
        PendingIntent pi = PendingIntent.getActivity(
                this, 0, open, PendingIntent.FLAG_UPDATE_CURRENT | PendingIntent.FLAG_IMMUTABLE);
        Notification notification = new NotificationCompat.Builder(this, CHANNEL_CONNECTION)
                .setSmallIcon(android.R.drawable.stat_notify_sync_noanim)
                .setContentTitle("PP is connected")
                .setContentText("Calls and messages arrive without push")
                .setContentIntent(pi)
                .setOngoing(true)
                .setCategory(NotificationCompat.CATEGORY_SERVICE)
                .setPriority(NotificationCompat.PRIORITY_MIN)
                .build();
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.UPSIDE_DOWN_CAKE) {
            startForeground(NOTIFICATION_ID, notification, ServiceInfo.FOREGROUND_SERVICE_TYPE_SPECIAL_USE);
        } else {
            startForeground(NOTIFICATION_ID, notification);
        }
        // Not sticky: a restarted service without the app's native side running would hold nothing.
        return START_NOT_STICKY;
    }

    @Override
    public IBinder onBind(Intent intent) {
        return null;
    }

    private void ensureChannel() {
        if (Build.VERSION.SDK_INT < Build.VERSION_CODES.O) {
            return;
        }
        NotificationChannel channel = new NotificationChannel(
                CHANNEL_CONNECTION, "Connection", NotificationManager.IMPORTANCE_MIN);
        channel.setDescription("Shown while PP stays connected in the background");
        NotificationManager nm = getSystemService(NotificationManager.class);
        if (nm != null) {
            nm.createNotificationChannel(channel);
        }
    }
}
