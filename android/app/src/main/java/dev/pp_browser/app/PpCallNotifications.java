package dev.pp_browser.app;

import android.app.Notification;
import android.app.NotificationChannel;
import android.app.NotificationManager;
import android.app.PendingIntent;
import android.content.Context;
import android.content.Intent;
import android.media.AudioAttributes;
import android.media.AudioManager;
import android.media.RingtoneManager;
import android.os.Build;
import android.os.Handler;
import android.os.Looper;
import android.os.SystemClock;
import android.util.Log;

import androidx.core.app.NotificationCompat;
import androidx.core.app.NotificationManagerCompat;
import androidx.core.app.Person;

/**
 * The phone's own call screen on Android (native AndroidCallSystemUi, ICallSystemUi): an incoming-call
 * notification that rings (the calls channel's ringtone, until answered), goes full screen on a locked phone and
 * answers or declines from the notification.
 *
 * It also holds the push's call: a call push that reached the phone before the app found the ring it announced.
 * The push can come with the app not running at all (the process started for it), so its "Incoming call" rings
 * from here and starts the app; once the app finds the ring, the same notification names the caller. Its timeouts
 * are CallKit's (CallSystemUi_Ios.mm).
 *
 * Every method may be called from any thread (native's UI thread, Firebase's, the main one): they lock the class.
 * The native callbacks only queue for the app's UI thread, so they are called under the lock.
 */
final class PpCallNotifications {
    private static final String TAG = "pp-browser-call";
    static final String ACTION_ANSWER = "dev.pp_browser.app.action.ANSWER_CALL";
    static final String ACTION_OPEN = "dev.pp_browser.app.action.OPEN_CALL";
    static final String ACTION_DECLINE = "dev.pp_browser.app.action.DECLINE_CALL";
    static final String EXTRA_CALL_ID = "call_id";
    /** A channel's sound cannot change once created: a new sound takes a new id. */
    private static final String CHANNEL_CALLS = "calls_incoming";
    private static final int NOTIFICATION_ID = 0x5051;

    /** With the app unlocked, the push's call waits this long for its ring (from the push or the unlock); answered, this long. */
    private static final long PENDING_RING_MS = 20_000;
    private static final long PENDING_ANSWERED_MS = 45_000;
    /** Locked (no ring can be found): an unanswered push's call waits as long as an invite lives. */
    private static final long PENDING_LOCKED_RING_MS = 75_000;
    private static final long PENDING_MAX_MS = 180_000;
    /** A ring the app no longer reports on (its process died) stops ringing after this. */
    private static final long RING_TIMEOUT_MS = 90_000;
    private static final long[] VIBRATION = {0, 800, 800};

    private static final Handler sMain = new Handler(Looper.getMainLooper());
    private static Context sContext;
    /** The push's call is ringing (or was answered), its ring not found yet. */
    private static boolean sPending;
    private static boolean sPendingAnswered;
    private static long sPendingSinceMs;
    private static int sPendingGeneration;
    /** The app can find rings (messaging up, after the PIN); false again when the app's activity goes. */
    private static boolean sAppReady;
    /** The ring the notification is for; empty: none, or the push's call. */
    private static String sShownCallId = "";

    private PpCallNotifications() {}

    static native void nativeOnAnswer(String callId);

    static native void nativeOnEnd(String callId);

    static native void nativeOnPendingGone();

    static native void nativeOnPendingChanged();

    static synchronized void init(Context context) {
        if (sContext == null && context != null) {
            sContext = context.getApplicationContext();
            ensureChannel(sContext);
        }
    }

    /** Notifications are allowed and the calls channel is not turned off: rings go here. */
    static synchronized boolean available() {
        if (sContext == null) {
            return false;
        }
        NotificationManagerCompat nm = NotificationManagerCompat.from(sContext);
        if (!nm.areNotificationsEnabled()) {
            return false;
        }
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.O) {
            NotificationChannel channel = nm.getNotificationChannel(CHANNEL_CALLS);
            return channel == null || channel.getImportance() != NotificationManager.IMPORTANCE_NONE;
        }
        return true;
    }

    static synchronized boolean hasPendingPushCall() {
        return sPending;
    }

    // --- the push -------------------------------------------------------------------------------

    /**
     * A call push: "Incoming call" now, unless the app is in front (it rings there) or a call already rings here.
     * The app finds the ring when it syncs (it is woken, or the full-screen notification starts it).
     */
    static synchronized void onCallPush(Context context, boolean appInFront) {
        init(context);
        if (sPending || !sShownCallId.isEmpty() || appInFront || !available()) {
            return;
        }
        sPending = true;
        sPendingAnswered = false;
        sPendingSinceMs = SystemClock.elapsedRealtime();
        Log.i(TAG, sAppReady ? "push call" : "push call, app locked or not running");
        post("", null, false, PENDING_MAX_MS);
        rearmPending();
        pendingChanged();
    }

    // --- from native (ICallSystemUi) ------------------------------------------------------------

    static synchronized void messagingReady() {
        sAppReady = true;
        if (sPending) {
            rearmPending();
        }
    }

    /** The app's activity went (its native side with it): rings cannot be found until it is back up, and its ring is over. */
    static synchronized void appGone() {
        sAppReady = false;
        if (!sShownCallId.isEmpty()) {
            sShownCallId = "";
            cancel();
        }
    }

    /** Ring `callId` here; the push's call becomes it (named now), or the answered push's call is it. */
    static synchronized void showIncoming(String callId, String callerName, boolean video) {
        if (sContext == null || callId == null || callId.isEmpty()) {
            return;
        }
        if (sPending) {
            final boolean answered = sPendingAnswered;
            sPending = false;
            sPendingAnswered = false;
            ++sPendingGeneration;
            sShownCallId = callId;
            pendingChanged();
            if (answered) {
                return;  // answered on the push's call: the app answers this ring, nothing more rings
            }
        }
        sShownCallId = callId;
        post(callId, callerName, video, RING_TIMEOUT_MS);
    }

    static synchronized void endIncoming(String callId) {
        if (callId == null || callId.isEmpty() || !callId.equals(sShownCallId)) {
            return;
        }
        sShownCallId = "";
        cancel();
    }

    /** The app joined a call no ring announced: no ring will match the push's call. */
    static synchronized void endPendingPushCall() {
        if (!sPending) {
            return;
        }
        cancel();
        clearPending();
    }

    // --- what the user does ---------------------------------------------------------------------

    /** Answer: the notification's action opened the app with this (empty: the push's call). */
    static synchronized void onAnswer(String callId) {
        final String id = callId == null ? "" : callId;
        if (id.isEmpty()) {
            if (!sPending) {
                return;
            }
            cancel();
            sPendingAnswered = true;
            rearmPending();
            toNative(() -> nativeOnAnswer(""));
            return;
        }
        if (!id.equals(sShownCallId)) {
            return;
        }
        cancel();  // the app's call screen shows the call; endIncoming comes when it is over
        toNative(() -> nativeOnAnswer(id));
    }

    /** Decline (PpCallActionReceiver). */
    static synchronized void onDecline(String callId) {
        final String id = callId == null ? "" : callId;
        if (id.isEmpty()) {
            if (!sPending) {
                return;
            }
            cancel();
            clearPending();  // gone first, then the decline: the app declines the ring once it shows
            toNative(() -> nativeOnEnd(""));
            return;
        }
        if (!id.equals(sShownCallId)) {
            return;
        }
        sShownCallId = "";
        cancel();
        toNative(() -> nativeOnEnd(id));
    }

    /** An activity intent from a call notification: the answer it carries. */
    static void onActivityIntent(Intent intent) {
        if (intent != null && ACTION_ANSWER.equals(intent.getAction())) {
            onAnswer(intent.getStringExtra(EXTRA_CALL_ID));
        }
    }

    // --- internals ------------------------------------------------------------------------------

    private static void clearPending() {
        sPending = false;
        sPendingAnswered = false;
        ++sPendingGeneration;
        pendingChanged();
        toNative(PpCallNotifications::nativeOnPendingGone);
    }

    /** (Re)starts the push's call's countdown: it goes if its ring has not shown by then (CallKit's rule). */
    private static void rearmPending() {
        final long age = SystemClock.elapsedRealtime() - sPendingSinceMs;
        long wait = PENDING_MAX_MS - age;
        if (sAppReady) {
            wait = Math.min(wait, sPendingAnswered ? PENDING_ANSWERED_MS : PENDING_RING_MS);
        } else if (!sPendingAnswered) {
            wait = Math.min(wait, PENDING_LOCKED_RING_MS - age);
        }
        final int generation = ++sPendingGeneration;
        sMain.postDelayed(() -> expirePending(generation), Math.max(wait, 0));
    }

    private static synchronized void expirePending(int generation) {
        if (generation != sPendingGeneration || !sPending) {
            return;
        }
        Log.i(TAG, "the push's call found no ring");
        cancel();
        clearPending();
    }

    private static void pendingChanged() {
        toNative(PpCallNotifications::nativeOnPendingChanged);
    }

    /** The app's native side is not loaded (the process started for a push): nothing there to tell. */
    private static void toNative(Runnable call) {
        try {
            call.run();
        } catch (UnsatisfiedLinkError ignored) {
        }
    }

    private static void cancel() {
        if (sContext != null) {
            NotificationManagerCompat.from(sContext).cancel(NOTIFICATION_ID);
        }
    }

    /** `callId` empty: the push's call (no caller known: the app's name). */
    private static void post(String callId, String callerName, boolean video, long timeoutMs) {
        final Context c = sContext;
        if (c == null) {
            return;
        }
        ensureChannel(c);
        final String name = callerName == null || callerName.isEmpty() ? c.getString(R.string.app_name) : callerName;
        final Person caller = new Person.Builder().setName(name).setImportant(true).build();
        final PendingIntent open = activityIntent(c, ACTION_OPEN, callId, 2);
        final PendingIntent answer = activityIntent(c, ACTION_ANSWER, callId, 1);
        final PendingIntent decline = declineIntent(c, callId);
        NotificationCompat.Builder builder = new NotificationCompat.Builder(c, CHANNEL_CALLS)
                .setSmallIcon(android.R.drawable.sym_call_incoming)
                .setContentIntent(open)
                .setFullScreenIntent(open, true)
                .setCategory(NotificationCompat.CATEGORY_CALL)
                .setPriority(NotificationCompat.PRIORITY_MAX)
                .setVisibility(NotificationCompat.VISIBILITY_PUBLIC)
                .setOngoing(true)
                .setOnlyAlertOnce(true)  // naming the push's call does not restart the ring
                .setTimeoutAfter(timeoutMs)
                // Before Android 8 (no channels) the notification carries its own ring.
                .setSound(RingtoneManager.getDefaultUri(RingtoneManager.TYPE_RINGTONE), AudioManager.STREAM_RING)
                .setVibrate(VIBRATION);
        try {
            notify(c, builder.setStyle(NotificationCompat.CallStyle.forIncomingCall(caller, decline, answer)
                    .setIsVideo(video)));
        } catch (IllegalArgumentException e) {
            // The system refused the call style (it wants a full-screen permission or a call service): plain
            // Answer / Decline buttons ring the same.
            Log.w(TAG, "call style refused: " + e);
            notify(c, builder.setStyle(null)
                    .setContentTitle(name)
                    .addAction(0, c.getString(R.string.call_decline), decline)
                    .addAction(0, c.getString(R.string.call_answer), answer));
        }
    }

    private static void notify(Context c, NotificationCompat.Builder builder) {
        Notification notification = builder.build();
        notification.flags |= Notification.FLAG_INSISTENT;  // rings until answered, declined or gone
        try {
            NotificationManagerCompat.from(c).notify(NOTIFICATION_ID, notification);
        } catch (SecurityException e) {
            Log.w(TAG, "call notification refused: " + e);  // notifications not allowed
        }
    }

    private static PendingIntent activityIntent(Context c, String action, String callId, int requestCode) {
        Intent intent = new Intent(c, MainActivity.class).setAction(action).putExtra(EXTRA_CALL_ID, callId);
        intent.setFlags(Intent.FLAG_ACTIVITY_NEW_TASK | Intent.FLAG_ACTIVITY_SINGLE_TOP);
        return PendingIntent.getActivity(
                c, requestCode, intent, PendingIntent.FLAG_UPDATE_CURRENT | PendingIntent.FLAG_IMMUTABLE);
    }

    private static PendingIntent declineIntent(Context c, String callId) {
        Intent intent = new Intent(c, PpCallActionReceiver.class).setAction(ACTION_DECLINE).putExtra(EXTRA_CALL_ID, callId);
        return PendingIntent.getBroadcast(
                c, 3, intent, PendingIntent.FLAG_UPDATE_CURRENT | PendingIntent.FLAG_IMMUTABLE);
    }

    private static void ensureChannel(Context c) {
        if (Build.VERSION.SDK_INT < Build.VERSION_CODES.O) {
            return;
        }
        NotificationChannel channel = new NotificationChannel(
                CHANNEL_CALLS, c.getString(R.string.channel_calls), NotificationManager.IMPORTANCE_HIGH);
        channel.setDescription(c.getString(R.string.channel_calls_description));
        channel.setSound(RingtoneManager.getDefaultUri(RingtoneManager.TYPE_RINGTONE),
                new AudioAttributes.Builder()
                        .setUsage(AudioAttributes.USAGE_NOTIFICATION_RINGTONE)
                        .setContentType(AudioAttributes.CONTENT_TYPE_SONIFICATION)
                        .build());
        channel.enableVibration(true);
        channel.setVibrationPattern(VIBRATION);
        channel.setLockscreenVisibility(Notification.VISIBILITY_PUBLIC);
        NotificationManager nm = c.getSystemService(NotificationManager.class);
        if (nm != null) {
            nm.createNotificationChannel(channel);
        }
    }
}
