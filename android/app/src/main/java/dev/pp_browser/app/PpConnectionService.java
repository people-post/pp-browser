package dev.pp_browser.app;

import android.app.Notification;
import android.app.NotificationChannel;
import android.app.NotificationManager;
import android.app.PendingIntent;
import android.app.Service;
import android.content.Context;
import android.content.Intent;
import android.content.SharedPreferences;
import android.content.pm.ServiceInfo;
import android.net.ConnectivityManager;
import android.net.Network;
import android.net.NetworkCapabilities;
import android.os.Build;
import android.os.Handler;
import android.os.IBinder;
import android.os.Looper;
import android.os.Process;
import android.os.SystemClock;
import android.util.Log;
import androidx.core.app.NotificationCompat;
import androidx.core.content.ContextCompat;

/**
 * P011 — "Stay connected in the background" (opt-in): a foreground service whose ongoing
 * notification keeps PP's process working while it is in the background, so calls and messages
 * reach phones without a push service. Started / stopped from native (AppLifecycle's setting) via
 * MainActivity.setStayConnected.
 *
 * Diagnostics (logs only, no account, contact or address): while it runs, one keep-alive line a minute (process id,
 * how long the process has run, the network type); the time of the last one is kept in a small preferences file, and
 * the next process start logs how long ago that was — a gap much longer than a minute means the process was frozen or
 * killed meanwhile (DEV/PLAN-android-incoming-call-screen.md step 3).
 */
public final class PpConnectionService extends Service {
    private static final String TAG = "PpConnectionService";
    private static final String CHANNEL_CONNECTION = "connection";
    private static final int NOTIFICATION_ID = 0x5050;
    private static final long KEEP_ALIVE_MS = 60_000;
    private static final String KEEP_ALIVE_PREFS = "pp_keep_alive";
    private static final String KEEP_ALIVE_AT = "last_ms";
    private static boolean sLastKeepAliveLogged;

    private final Handler mHandler = new Handler(Looper.getMainLooper());
    private final Runnable mKeepAlive = this::keepAlive;
    private boolean mKeepAliveRunning;

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
        logLastKeepAlive(this);
        if (!mKeepAliveRunning) {
            mKeepAliveRunning = true;
            mHandler.post(mKeepAlive);
        }
        // Not sticky: a restarted service without the app's native side running would hold nothing.
        return START_NOT_STICKY;
    }

    @Override
    public void onDestroy() {
        mHandler.removeCallbacks(mKeepAlive);
        mKeepAliveRunning = false;
        Log.i(TAG, "keep-alive stopped (service destroyed)");
        super.onDestroy();
    }

    /** Once per process: how long ago the last keep-alive line was written (by this process or one before it). */
    static synchronized void logLastKeepAlive(Context context) {
        if (sLastKeepAliveLogged) {
            return;
        }
        sLastKeepAliveLogged = true;
        final long last = prefs(context).getLong(KEEP_ALIVE_AT, 0);
        if (last == 0) {
            Log.i(TAG, "process start pid=" + Process.myPid() + " no keep-alive seen before");
            return;
        }
        final long agoS = (System.currentTimeMillis() - last) / 1000;
        Log.i(TAG, "process start pid=" + Process.myPid() + " last keep-alive " + agoS + " s ago");
    }

    private void keepAlive() {
        final long uptimeS = (SystemClock.elapsedRealtime() - Process.getStartElapsedRealtime()) / 1000;
        Log.i(TAG, "keep-alive pid=" + Process.myPid() + " process_up_s=" + uptimeS + " net=" + networkType());
        prefs(this).edit().putLong(KEEP_ALIVE_AT, System.currentTimeMillis()).apply();
        mHandler.postDelayed(mKeepAlive, KEEP_ALIVE_MS);
    }

    private static SharedPreferences prefs(Context context) {
        return context.getApplicationContext().getSharedPreferences(KEEP_ALIVE_PREFS, Context.MODE_PRIVATE);
    }

    /** wifi / cellular / ethernet / vpn / other / none. */
    private String networkType() {
        try {
            final ConnectivityManager cm = getSystemService(ConnectivityManager.class);
            final Network network = cm == null ? null : cm.getActiveNetwork();
            final NetworkCapabilities caps = network == null ? null : cm.getNetworkCapabilities(network);
            if (caps == null) {
                return "none";
            }
            if (caps.hasTransport(NetworkCapabilities.TRANSPORT_VPN)) {
                return "vpn";
            }
            if (caps.hasTransport(NetworkCapabilities.TRANSPORT_WIFI)) {
                return "wifi";
            }
            if (caps.hasTransport(NetworkCapabilities.TRANSPORT_CELLULAR)) {
                return "cellular";
            }
            if (caps.hasTransport(NetworkCapabilities.TRANSPORT_ETHERNET)) {
                return "ethernet";
            }
            return "other";
        } catch (RuntimeException e) {
            return "unknown";
        }
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
