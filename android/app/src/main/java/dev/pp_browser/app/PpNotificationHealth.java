package dev.pp_browser.app;

import android.app.NotificationChannel;
import android.app.NotificationManager;
import android.content.ComponentName;
import android.content.Context;
import android.content.Intent;
import android.content.pm.PackageManager;
import android.net.Uri;
import android.os.Build;
import android.os.PowerManager;
import android.provider.Settings;
import android.util.Log;
import androidx.core.app.NotificationManagerCompat;
import java.util.Locale;

/**
 * What the phone's settings do to PP's alerts, and the system screens that fix it (Me → Notifications tips; native
 * PlatformNotificationSettings_Android.cpp, through MainActivity.notificationHealth / openNotificationFix).
 * Every check stands alone: one that throws or cannot tell leaves its bit unset and the others are kept.
 */
final class PpNotificationHealth {
    private static final String TAG = "PpNotificationHealth";
    private static final String CHANNEL_MESSAGES = "messages";
    private static final String GMS_PACKAGE = "com.google.android.gms";

    // Bits of health(); the same numbers as PlatformNotificationSettings_Android.cpp.
    private static final int BLOCKED = 1;
    private static final int SILENCED = 2;
    private static final int BATTERY_RESTRICTED = 4;
    private static final int NO_PUSH_IN_BUILD = 8;
    private static final int NO_GOOGLE_SERVICES = 16;
    private static final int AUTOSTART_VENDOR = 32;

    // Targets of open(); the same numbers as the C++ side.
    private static final int FIX_APP_NOTIFICATIONS = 0;
    private static final int FIX_DO_NOT_DISTURB = 1;
    private static final int FIX_BATTERY = 2;
    private static final int FIX_AUTOSTART = 3;

    private PpNotificationHealth() {}

    /** The bit mask, -1 when there is no context. */
    static int health(Context context) {
        if (context == null) {
            return -1;
        }
        int bits = 0;
        try {
            if (blocked(context)) {
                bits |= BLOCKED;
            }
        } catch (RuntimeException e) {
            Log.w(TAG, "notifications check failed: " + e);
        }
        try {
            NotificationManager nm = context.getSystemService(NotificationManager.class);
            // INTERRUPTION_FILTER_NONE: total silence. Priority-only and alarms-only let some alerts through.
            if (nm != null && nm.getCurrentInterruptionFilter() == NotificationManager.INTERRUPTION_FILTER_NONE) {
                bits |= SILENCED;
            }
        } catch (RuntimeException e) {
            Log.w(TAG, "do-not-disturb check failed: " + e);
        }
        try {
            PowerManager pm = context.getSystemService(PowerManager.class);
            if (pm != null && !pm.isIgnoringBatteryOptimizations(context.getPackageName())) {
                bits |= BATTERY_RESTRICTED;
            }
        } catch (RuntimeException e) {
            Log.w(TAG, "battery check failed: " + e);
        }
        try {
            if (!BuildConfig.ENABLE_FCM) {
                bits |= NO_PUSH_IN_BUILD;
            } else if (!hasGoogleServices(context)) {
                bits |= NO_GOOGLE_SERVICES;
            }
        } catch (RuntimeException e) {
            Log.w(TAG, "google services check failed: " + e);
        }
        if (autostartVendor()) {
            bits |= AUTOSTART_VENDOR;
        }
        return bits;
    }

    /** Notifications are off for the app, or its messages channel is. A channel that is not there says nothing. */
    private static boolean blocked(Context context) {
        if (!NotificationManagerCompat.from(context).areNotificationsEnabled()) {
            return true;
        }
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.O) {
            NotificationManager nm = context.getSystemService(NotificationManager.class);
            NotificationChannel channel = nm != null ? nm.getNotificationChannel(CHANNEL_MESSAGES) : null;
            return channel != null && channel.getImportance() == NotificationManager.IMPORTANCE_NONE;
        }
        return false;
    }

    /** Needs the &lt;package&gt; entry in the manifest's &lt;queries&gt; (Android 11+), or it would always say no. */
    private static boolean hasGoogleServices(Context context) {
        try {
            context.getPackageManager().getPackageInfo(GMS_PACKAGE, 0);
            return true;
        } catch (PackageManager.NameNotFoundException e) {
            return false;
        }
    }

    /** Xiaomi family (Xiaomi / Redmi / POCO) and vivo family (vivo / iQOO): they have an auto-start switch. */
    static boolean autostartVendor() {
        final String maker = (Build.MANUFACTURER + " " + Build.BRAND).toLowerCase(Locale.ROOT);
        return maker.contains("xiaomi") || maker.contains("redmi") || maker.contains("poco") || maker.contains("vivo")
                || maker.contains("iqoo");
    }

    /** Opens the screen for a target; a missing or refused screen falls back to the app's details page. */
    static boolean open(Context context, int target) {
        if (context == null) {
            return false;
        }
        final String pkg = context.getPackageName();
        switch (target) {
            case FIX_APP_NOTIFICATIONS:
                if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.O
                        && start(context, new Intent(Settings.ACTION_APP_NOTIFICATION_SETTINGS)
                                .putExtra(Settings.EXTRA_APP_PACKAGE, pkg))) {
                    return true;
                }
                break;
            case FIX_DO_NOT_DISTURB:
                // The first is not a public constant (AOSP's Settings has it); the second is the sound page.
                if (start(context, new Intent("android.settings.ZEN_MODE_SETTINGS"))
                        || start(context, new Intent(Settings.ACTION_SOUND_SETTINGS))) {
                    return true;
                }
                break;
            case FIX_BATTERY:
                if (start(context, new Intent(Settings.ACTION_IGNORE_BATTERY_OPTIMIZATION_SETTINGS))) {
                    return true;
                }
                break;
            case FIX_AUTOSTART:
                // Private screens of the makers: any of them may be missing on a model, then the details page.
                if (start(context, component("com.miui.securitycenter",
                                "com.miui.permcenter.autostart.AutoStartManagementActivity"))
                        || start(context, component("com.vivo.permissionmanager",
                                "com.vivo.permissionmanager.activity.BgStartUpManagerActivity"))
                        || start(context, component("com.iqoo.secure",
                                "com.iqoo.secure.ui.phoneoptimize.AddWhiteListActivity"))) {
                    return true;
                }
                break;
            default:
                return false;
        }
        return start(context, new Intent(Settings.ACTION_APPLICATION_DETAILS_SETTINGS, Uri.parse("package:" + pkg)));
    }

    private static Intent component(String pkg, String cls) {
        return new Intent().setComponent(new ComponentName(pkg, cls));
    }

    /** ActivityNotFoundException (no such screen), SecurityException (not exported to us), anything else: false. */
    private static boolean start(Context context, Intent intent) {
        try {
            intent.addFlags(Intent.FLAG_ACTIVITY_NEW_TASK);
            context.startActivity(intent);
            return true;
        } catch (RuntimeException e) {
            Log.w(TAG, "screen not opened: " + intent + ": " + e);
            return false;
        }
    }
}
