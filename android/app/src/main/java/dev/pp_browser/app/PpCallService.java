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
import androidx.core.app.Person;
import androidx.core.app.ServiceCompat;
import androidx.core.content.ContextCompat;

/**
 * The call this device is in (native OngoingCallService): a foreground service of the microphone type, with an
 * ongoing-call notification (who with; Hang up). Without it Android gives an app in the background silence from the
 * microphone (9+) and may stop its process. Started when the call starts, with the app in front (Android 12+ does not
 * let a background app start one), and stopped when it ends. The camera is not kept: a call pauses it in the
 * background (V060).
 */
public final class PpCallService extends Service {
    private static final String TAG = "PpCallService";
    private static final String CHANNEL_ONGOING = "calls_ongoing";
    private static final int NOTIFICATION_ID = 0x5054;
    private static final String EXTRA_TITLE = "title";
    static final String ACTION_HANG_UP = "dev.pp_browser.app.action.HANG_UP";

    static native void nativeOnHangUp();

    /** Starts the service, or updates its notification (`title`: who with). */
    static void start(Context context, String title) {
        Intent intent = new Intent(context, PpCallService.class).putExtra(EXTRA_TITLE, title);
        try {
            ContextCompat.startForegroundService(context, intent);
        } catch (RuntimeException e) {
            // Android 12+: not while the app is in the background (a call that connected after the user left it).
            Log.w(TAG, "start refused: " + e);
        }
    }

    static void stop(Context context) {
        context.stopService(new Intent(context, PpCallService.class));
    }

    /** Hang up on the notification (PpCallActionReceiver). */
    static void onHangUp() {
        try {
            nativeOnHangUp();
        } catch (UnsatisfiedLinkError ignored) {
            // The app's native side is gone: so is its call.
        }
    }

    @Override
    public int onStartCommand(Intent intent, int flags, int startId) {
        final String title = intent == null ? null : intent.getStringExtra(EXTRA_TITLE);
        try {
            ServiceCompat.startForeground(this, NOTIFICATION_ID, notification(title),
                    Build.VERSION.SDK_INT >= Build.VERSION_CODES.R
                            ? ServiceInfo.FOREGROUND_SERVICE_TYPE_MICROPHONE : 0);
        } catch (RuntimeException e) {
            // No microphone permission, or not allowed now: the call goes on as before (foreground only).
            Log.w(TAG, "foreground refused: " + e);
            stopSelf();
        }
        // Not sticky: a restarted service without the app's call would hold nothing.
        return START_NOT_STICKY;
    }

    @Override
    public IBinder onBind(Intent intent) {
        return null;
    }

    private Notification notification(String title) {
        ensureChannel(this);
        final String name = title == null || title.isEmpty() ? getString(R.string.app_name) : title;
        Intent open = new Intent(this, MainActivity.class);
        open.setFlags(Intent.FLAG_ACTIVITY_NEW_TASK | Intent.FLAG_ACTIVITY_SINGLE_TOP);
        PendingIntent openPi = PendingIntent.getActivity(
                this, 4, open, PendingIntent.FLAG_UPDATE_CURRENT | PendingIntent.FLAG_IMMUTABLE);
        Intent hangUp = new Intent(this, PpCallActionReceiver.class).setAction(ACTION_HANG_UP);
        PendingIntent hangUpPi = PendingIntent.getBroadcast(
                this, 5, hangUp, PendingIntent.FLAG_UPDATE_CURRENT | PendingIntent.FLAG_IMMUTABLE);
        Person caller = new Person.Builder().setName(name).build();
        return new NotificationCompat.Builder(this, CHANNEL_ONGOING)
                .setSmallIcon(android.R.drawable.sym_call_outgoing)
                .setContentIntent(openPi)
                .setOngoing(true)
                .setOnlyAlertOnce(true)
                .setCategory(NotificationCompat.CATEGORY_CALL)
                .setStyle(NotificationCompat.CallStyle.forOngoingCall(caller, hangUpPi))
                .build();
    }

    private static void ensureChannel(Context c) {
        if (Build.VERSION.SDK_INT < Build.VERSION_CODES.O) {
            return;
        }
        NotificationChannel channel = new NotificationChannel(
                CHANNEL_ONGOING, c.getString(R.string.channel_calls_ongoing), NotificationManager.IMPORTANCE_LOW);
        channel.setDescription(c.getString(R.string.channel_calls_ongoing_description));
        NotificationManager nm = c.getSystemService(NotificationManager.class);
        if (nm != null) {
            nm.createNotificationChannel(channel);
        }
    }
}
