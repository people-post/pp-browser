package dev.pp_browser.app;

import android.app.ActivityManager;
import android.app.NotificationChannel;
import android.app.NotificationManager;
import android.app.PendingIntent;
import android.content.Intent;
import android.content.SharedPreferences;
import android.content.pm.PackageManager;
import android.content.res.Configuration;
import android.graphics.Bitmap;
import android.graphics.Rect;
import android.media.AudioAttributes;
import android.media.AudioDeviceInfo;
import android.media.AudioFocusRequest;
import android.media.AudioManager;
import android.net.ConnectivityManager;
import android.net.Network;
import android.net.NetworkCapabilities;
import android.os.Build;
import android.os.Handler;
import android.os.HandlerThread;
import android.provider.Settings;
import android.util.Log;
import android.view.PixelCopy;
import android.view.View;
import android.view.ViewTreeObserver;
import android.view.Window;
import android.view.WindowManager;

import androidx.core.app.NotificationCompat;
import androidx.core.app.NotificationManagerCompat;
import androidx.core.content.ContextCompat;
import androidx.core.graphics.Insets;
import androidx.core.view.ViewCompat;
import androidx.core.view.WindowCompat;
import androidx.core.view.WindowInsetsAnimationCompat;
import androidx.core.view.WindowInsetsCompat;
import androidx.core.view.WindowInsetsControllerCompat;

import org.libsdl.app.SDLActivity;
import org.libsdl.app.SDLSurface;

import java.util.ArrayList;
import java.util.List;
import java.util.concurrent.CountDownLatch;
import java.util.concurrent.TimeUnit;
import java.util.concurrent.atomic.AtomicBoolean;
import java.util.concurrent.atomic.AtomicReference;

/**
 * SDL3 and other native deps are statically linked into libmain.so (see root CMakeLists.txt).
 *
 * Captures a last-good SurfaceView frame into the task description before the EGL surface is
 * torn down, so Recents / app-switcher is less likely to show a black tile.
 *
 * On API 24+, SDL pauses from {@code onStop} after {@code surfaceDestroyed}; capture must happen
 * earlier from {@code onPause} / focus loss while {@code mIsSurfaceReady} is still true.
 *
 * Soft keyboard: {@code windowSoftInputMode=adjustNothing} keeps the EGL surface full-bleed
 * (same as iOS — do not pan/resize the GL view). IME + system-bar insets are published into
 * SDL safe-area bottom so {@code ShellHost::RefreshSafeAreaInsets} reflows chrome above the
 * keyboard. {@link SDLSurface} alone often misses IME inset dispatch, so this activity also
 * listens on {@code mLayout}.
 *
 * Native argv (via {@link #getArguments()}):
 * <ul>
 *   <li>{@code --ez startup_timing true} → {@code --startup-timing}</li>
 *   <li>{@code --ez debug true} → {@code --debug}</li>
 *   <li>{@code -e pp_native_args "--startup-timing --debug"} → split on whitespace</li>
 * </ul>
 */
public class MainActivity extends SDLActivity {
    private static final String TAG = "pp-browser";
    private static final int THUMBNAIL_MAX_EDGE = 512;
    private static final long PIXEL_COPY_TIMEOUT_MS = 80;
    private static final String CHANNEL_MESSAGES = "messages";
    private static final String PREFS = "pp_browser_device";

    private HandlerThread mPixelCopyThread;
    private Handler mPixelCopyHandler;
    private final AtomicBoolean mThumbnailCapturedForPause = new AtomicBoolean(false);
    private AudioFocusRequest mCallAudioFocusRequest;
    private final AudioManager.OnAudioFocusChangeListener mCallAudioFocusListener = focusChange -> { };

    /** App appearance preference from native Theme ("system" / "light" / "dark"). */
    private volatile String mAppAppearance = "system";

    /**
     * Last system-bar / cutout insets from WindowInsets (API 30+ / edge-to-edge).
     * Keyboard height is merged into bottom separately — never from visible.top.
     */
    private int mSysLeft;
    private int mSysRight;
    private int mSysTop;
    private int mSysBottom;
    private boolean mHaveSystemInsets;
    /** Bottom gap (rootH - visible.bottom) while IME is hidden; usually nav-bar overlay. */
    private int mBaselineBottomGap = -1;
    private int mPublishedLeft = -1;
    private int mPublishedRight = -1;
    private int mPublishedTop = -1;
    private int mPublishedBottom = -1;
    private ViewTreeObserver.OnGlobalLayoutListener mKeyboardLayoutListener;
    /** adjustNothing often skips layout passes when the IME opens — poll IMM height. */
    private final Handler mKeyboardProbeHandler = new Handler(android.os.Looper.getMainLooper());
    private final Runnable mKeyboardProbeRunnable = new Runnable() {
        @Override
        public void run() {
            if (mLayout != null) {
                publishSafeAreaFromVisibleFrame(mLayout);
            }
            final long delayMs = isScreenKeyboardShown() ? 100L : 500L;
            mKeyboardProbeHandler.postDelayed(this, delayMs);
        }
    };

    @Override
    protected String[] getLibraries() {
        return new String[] { "main" };
    }

    /**
     * Forward Intent extras as native argv for {@code main()}.
     * Example: {@code adb shell am start -n dev.pp_browser.app/.MainActivity --ez startup_timing true}
     */
    @Override
    protected String[] getArguments() {
        final Intent intent = getIntent();
        if (intent == null) {
            return new String[0];
        }
        final ArrayList<String> args = new ArrayList<>();
        if (intent.getBooleanExtra("startup_timing", false)) {
            args.add("--startup-timing");
        }
        if (intent.getBooleanExtra("debug", false)) {
            args.add("--debug");
        }
        final String raw = intent.getStringExtra("pp_native_args");
        if (raw != null && !raw.trim().isEmpty()) {
            for (String part : raw.trim().split("\\s+")) {
                if (!part.isEmpty()) {
                    args.add(part);
                }
            }
        }
        return args.toArray(new String[0]);
    }

    @Override
    protected void onCreate(android.os.Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);
        installKeyboardSafeAreaPublisher();
        applyNavigationBarColor();
        ensureNotificationChannel();
        InboxSyncWorker.schedule(this, true);
    }

    @Override
    protected void onNewIntent(Intent intent) {
        super.onNewIntent(intent);
        setIntent(intent);
    }

    @Override
    protected void onDestroy() {
        uninstallKeyboardSafeAreaPublisher();
        if (mPixelCopyThread != null) {
            mPixelCopyThread.quitSafely();
            mPixelCopyThread = null;
            mPixelCopyHandler = null;
        }
        super.onDestroy();
    }

    /**
     * Mirror iOS keyboard-as-safe-area-bottom, without double-counting system bars.
     *
     * This activity is <em>not</em> edge-to-edge (see {@link #applyNavigationBarColor}): the
     * SurfaceView is already laid out below the status bar and above the nav bar. Publishing
     * {@code visible.top} / resting nav gap as SDL safe-area inset the shell a second time
     * (blank strip at top, black strip at bottom) — observed on moto g(7) play / API 28.
     *
     * Only the soft-keyboard occlusion is added to bottom; static safe area stays
     * machine-prefs (API &lt; 30) or {@link SDLSurface} system insets (API 30+).
     */
    private void installKeyboardSafeAreaPublisher() {
        if (mLayout == null) {
            Log.w(TAG, "Keyboard safe-area: mLayout null after onCreate");
            return;
        }
        final View root = mLayout;

        ViewCompat.setOnApplyWindowInsetsListener(root, (v, windowInsets) -> {
            publishSafeAreaFromWindowInsets(windowInsets);
            return windowInsets;
        });

        ViewCompat.setWindowInsetsAnimationCallback(root,
                new WindowInsetsAnimationCompat.Callback(
                        WindowInsetsAnimationCompat.Callback.DISPATCH_MODE_CONTINUE_ON_SUBTREE) {
                    @Override
                    public WindowInsetsCompat onProgress(WindowInsetsCompat insets,
                            List<WindowInsetsAnimationCompat> runningAnimations) {
                        publishSafeAreaFromWindowInsets(insets);
                        return insets;
                    }
                });

        mKeyboardLayoutListener = () -> publishSafeAreaFromVisibleFrame(root);
        root.getViewTreeObserver().addOnGlobalLayoutListener(mKeyboardLayoutListener);
        ViewCompat.requestApplyInsets(root);
        publishSafeAreaFromVisibleFrame(root);
        mKeyboardProbeHandler.removeCallbacks(mKeyboardProbeRunnable);
        mKeyboardProbeHandler.post(mKeyboardProbeRunnable);
    }

    private void uninstallKeyboardSafeAreaPublisher() {
        mKeyboardProbeHandler.removeCallbacks(mKeyboardProbeRunnable);
        if (mLayout != null) {
            ViewCompat.setOnApplyWindowInsetsListener(mLayout, null);
            ViewCompat.setWindowInsetsAnimationCallback(mLayout, null);
            if (mKeyboardLayoutListener != null) {
                final ViewTreeObserver observer = mLayout.getViewTreeObserver();
                if (observer.isAlive()) {
                    observer.removeOnGlobalLayoutListener(mKeyboardLayoutListener);
                }
            }
        }
        mKeyboardLayoutListener = null;
    }

    private void publishSafeAreaFromWindowInsets(WindowInsetsCompat windowInsets) {
        final Insets ime = windowInsets.getInsets(WindowInsetsCompat.Type.ime());
        final int imeBottom = Math.max(0, ime.bottom);
        // API < 30: content is window-fitted; system-bar WindowInsets would double-count
        // (moto g7 play blank top / black bottom). Only IME + visible-frame keyboard.
        if (Build.VERSION.SDK_INT >= 30) {
            final Insets sys = windowInsets.getInsets(
                    WindowInsetsCompat.Type.systemBars()
                            | WindowInsetsCompat.Type.displayCutout());
            mHaveSystemInsets = true;
            mSysLeft = Math.max(0, sys.left);
            mSysRight = Math.max(0, sys.right);
            mSysTop = Math.max(0, sys.top);
            mSysBottom = Math.max(0, sys.bottom);
        }
        if (mLayout != null) {
            publishSafeAreaFromVisibleFrame(mLayout, imeBottom);
        } else if (mHaveSystemInsets) {
            publishSafeAreaInsets(mSysLeft, mSysRight, mSysTop, Math.max(mSysBottom, imeBottom));
        } else {
            publishSafeAreaInsets(0, 0, 0, imeBottom);
        }
    }

    private void publishSafeAreaFromVisibleFrame(View root) {
        publishSafeAreaFromVisibleFrame(root, 0);
    }

    /**
     * Resolve soft-keyboard height in px. {@code adjustNothing} on API &lt; 30 often leaves
     * {@link View#getWindowVisibleDisplayFrame} unchanged (moto g7 play), so also probe
     * {@code InputMethodManager#getInputMethodWindowVisibleHeight} and SDL's keyboard flag.
     */
    private int probeKeyboardHeightPx(View root, int imeBottomFromInsets) {
        int keyboardBottom = Math.max(0, imeBottomFromInsets);

        final WindowInsetsCompat rootInsets = ViewCompat.getRootWindowInsets(root);
        if (rootInsets != null) {
            keyboardBottom = Math.max(keyboardBottom,
                    rootInsets.getInsets(WindowInsetsCompat.Type.ime()).bottom);
        }

        final Rect visible = new Rect();
        root.getWindowVisibleDisplayFrame(visible);
        // Prefer display height: DecorView height can already exclude the nav bar.
        final int screenH = getResources().getDisplayMetrics().heightPixels;
        final int rootH = Math.max(screenH, root.getRootView().getHeight());
        final int gapBottom = Math.max(0, rootH - visible.bottom);
        final int keyboardThreshold = Math.max(rootH / 6, dpToPx(100));
        if (gapBottom < keyboardThreshold) {
            mBaselineBottomGap = gapBottom;
        } else {
            final int baseline = Math.max(0, mBaselineBottomGap);
            keyboardBottom = Math.max(keyboardBottom, Math.max(0, gapBottom - baseline));
        }

        final int immHeight = probeImmKeyboardHeightPx();
        if (immHeight > keyboardThreshold) {
            keyboardBottom = Math.max(keyboardBottom, immHeight);
        } else if (immHeight > 0 && isScreenKeyboardShown()) {
            keyboardBottom = Math.max(keyboardBottom, immHeight);
        }

        // Last resort when visible-frame is sticky under adjustNothing but SDL says IME is up.
        if (keyboardBottom == 0 && isScreenKeyboardShown() && immHeight > 0) {
            keyboardBottom = immHeight;
        }
        return keyboardBottom;
    }

    private int probeImmKeyboardHeightPx() {
        try {
            final android.view.inputmethod.InputMethodManager imm =
                    (android.view.inputmethod.InputMethodManager) getSystemService(INPUT_METHOD_SERVICE);
            if (imm == null) {
                return 0;
            }
            final java.lang.reflect.Method method =
                    imm.getClass().getMethod("getInputMethodWindowVisibleHeight");
            final Object value = method.invoke(imm);
            if (value instanceof Integer) {
                return Math.max(0, (Integer) value);
            }
        } catch (ReflectiveOperationException | RuntimeException ignored) {
            // Hidden API; absence is fine — visible-frame / WindowInsets paths remain.
        }
        return 0;
    }

    /**
     * Visible-frame + IMM keyboard detection for {@code adjustNothing} / API &lt; 30.
     * {@code imeBottomFromInsets} is max'd in when WindowInsets.Type.ime is available.
     */
    private void publishSafeAreaFromVisibleFrame(View root, int imeBottomFromInsets) {
        final int keyboardBottom = probeKeyboardHeightPx(root, imeBottomFromInsets);

        if (mHaveSystemInsets) {
            // Edge-to-edge / API 30+: keep system bars; replace bottom with max(sys, IME).
            publishSafeAreaInsets(mSysLeft, mSysRight, mSysTop, Math.max(mSysBottom, keyboardBottom));
        } else {
            // Fitted window (moto g7 play): do not republish status/nav — only IME lift.
            publishSafeAreaInsets(0, 0, 0, keyboardBottom);
        }
    }

    private int dpToPx(int dp) {
        final float density = getResources().getDisplayMetrics().density;
        return Math.round(dp * density);
    }

    private void publishSafeAreaInsets(int left, int right, int top, int bottom) {
        if (left == mPublishedLeft && right == mPublishedRight && top == mPublishedTop
                && bottom == mPublishedBottom) {
            return;
        }
        mPublishedLeft = left;
        mPublishedRight = right;
        mPublishedTop = top;
        mPublishedBottom = bottom;
        Log.i(TAG, "Safe-area insets LRTB=" + left + "," + right + "," + top + "," + bottom);
        onNativeInsetsChanged(left, right, top, bottom);
    }

    @Override
    protected void onResume() {
        mThumbnailCapturedForPause.set(false);
        super.onResume();
        mKeyboardProbeHandler.removeCallbacks(mKeyboardProbeRunnable);
        mKeyboardProbeHandler.post(mKeyboardProbeRunnable);
    }

    @Override
    protected void onPause() {
        mKeyboardProbeHandler.removeCallbacks(mKeyboardProbeRunnable);
        captureRecentsThumbnailOnce();
        super.onPause();
    }

    @Override
    public void onConfigurationChanged(Configuration newConfig) {
        super.onConfigurationChanged(newConfig);
        applyNavigationBarColor();
    }

    @Override
    protected boolean sendCommand(int command, Object data) {
        final boolean result = super.sendCommand(command, data);
        if (command == COMMAND_CHANGE_WINDOW_STYLE) {
            // SDL may toggle immersive mode; re-assert nav color when bars return.
            runOnUiThread(this::applyNavigationBarColor);
        }
        return result;
    }

    /**
     * Called from native {@code AndroidSystemChrome} when Me → Theme (or system
     * sync) changes the app appearance preference.
     */
    public void setAppAppearance(String appearance) {
        final String normalized = normalizeAppearance(appearance);
        runOnUiThread(() -> {
            mAppAppearance = normalized;
            applyNavigationBarColor();
        });
    }

    private static String normalizeAppearance(String appearance) {
        if ("light".equals(appearance) || "dark".equals(appearance)) {
            return appearance;
        }
        return "system";
    }

    private boolean resolveLightNavigationBars() {
        if ("light".equals(mAppAppearance)) {
            return true;
        }
        if ("dark".equals(mAppAppearance)) {
            return false;
        }
        return (getResources().getConfiguration().uiMode & Configuration.UI_MODE_NIGHT_MASK)
                != Configuration.UI_MODE_NIGHT_YES;
    }

    /**
     * Theme the system navigation bar only. Does not touch layout-fullscreen /
     * decor-fits flags (those raced SurfaceView attach and delayed first paint).
     * Status bar stays on the theme splash color; coloring it reliably needs an
     * opaque non-SurfaceView window on some OEMs.
     */
    private void applyNavigationBarColor() {
        final Window window = getWindow();
        if (window == null) {
            return;
        }

        window.clearFlags(WindowManager.LayoutParams.FLAG_TRANSLUCENT_NAVIGATION);
        window.addFlags(WindowManager.LayoutParams.FLAG_DRAWS_SYSTEM_BAR_BACKGROUNDS);

        final boolean lightUi = resolveLightNavigationBars();
        final int navColor = ContextCompat.getColor(
                this, lightUi ? R.color.window_background_light : R.color.window_background);
        window.setNavigationBarColor(navColor);

        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.Q) {
            window.setNavigationBarContrastEnforced(false);
        }

        final View decor = window.getDecorView();
        final WindowInsetsControllerCompat insetsController =
                WindowCompat.getInsetsController(window, decor);
        insetsController.setAppearanceLightNavigationBars(lightUi);
    }

    @Override
    public void onWindowFocusChanged(boolean hasFocus) {
        if (!hasFocus) {
            captureRecentsThumbnailOnce();
        }
        super.onWindowFocusChanged(hasFocus);
    }

    @Override
    protected void pauseNativeThread() {
        captureRecentsThumbnailOnce();
        super.pauseNativeThread();
    }

    /**
     * Called from native {@code CallAudioSession} when a VoIP call starts/ends.
     * Phones: {@link AudioManager#MODE_IN_COMMUNICATION} for earpiece/speaker routing.
     * Speaker-only tablets (no earpiece): stay in {@link AudioManager#MODE_NORMAL} so
     * SDL AAudio MEDIA streams are not ducked (API &lt; 28 cannot set
     * {@code AAUDIO_USAGE_VOICE_COMMUNICATION} — SM-T380 whisper-quiet dogfood).
     */
    public void setCallAudioSessionActive(boolean active) {
        AudioManager am = (AudioManager) getSystemService(AUDIO_SERVICE);
        if (am == null) {
            return;
        }
        if (active) {
            final boolean speakerOnly = !hasCallEarpieceRoute();
            if (speakerOnly) {
                am.setMode(AudioManager.MODE_NORMAL);
                am.setSpeakerphoneOn(true);
                requestCallAudioFocus(am, true);
                ensureStreamVolumeFloor(am, AudioManager.STREAM_MUSIC);
                Log.i(TAG, "Speaker-only call audio: MODE_NORMAL + STREAM_MUSIC floor");
            } else {
                am.setMode(AudioManager.MODE_IN_COMMUNICATION);
                requestCallAudioFocus(am, false);
                ensureStreamVolumeFloor(am, AudioManager.STREAM_VOICE_CALL);
            }
        } else {
            if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.S) {
                am.clearCommunicationDevice();
            }
            am.setSpeakerphoneOn(false);
            am.setMode(AudioManager.MODE_NORMAL);
            abandonCallAudioFocus(am);
        }
    }

    /**
     * Called from native {@code CallAudioSession.SupportsSpeakerToggle}.
     * Phones expose {@link AudioDeviceInfo#TYPE_BUILTIN_EARPIECE}; tablets (e.g. SM-T380)
     * only have the loudspeaker, so FORCE_SPEAKER vs FORCE_NONE is inaudible.
     */
    public boolean hasCallEarpieceRoute() {
        AudioManager am = (AudioManager) getSystemService(AUDIO_SERVICE);
        if (am == null) {
            return false;
        }
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.M) {
            for (AudioDeviceInfo device : am.getDevices(AudioManager.GET_DEVICES_OUTPUTS)) {
                if (device.getType() == AudioDeviceInfo.TYPE_BUILTIN_EARPIECE) {
                    return true;
                }
            }
            return false;
        }
        return getPackageManager().hasSystemFeature(PackageManager.FEATURE_TELEPHONY);
    }

    /**
     * Called from native {@code CallAudioSession} for in-call speaker / earpiece.
     * Synchronous: ToggleSpeaker reopens SDL capture immediately after this returns.
     */
    public void setCallSpeakerphoneOn(boolean on) {
        AudioManager am = (AudioManager) getSystemService(AUDIO_SERVICE);
        if (am == null) {
            return;
        }
        // Speaker-only tablets: no earpiece route; keep MODE_NORMAL (see setCallAudioSessionActive).
        if (!hasCallEarpieceRoute()) {
            am.setSpeakerphoneOn(true);
            ensureStreamVolumeFloor(am, AudioManager.STREAM_MUSIC);
            return;
        }
        // Speakerphone routing only works in communication mode; toggling without it
        // can leave AudioRecord/SDL capture on a silent path (PreferLocal dogfood).
        final boolean wasInComm = am.getMode() == AudioManager.MODE_IN_COMMUNICATION;
        if (!wasInComm) {
            am.setMode(AudioManager.MODE_IN_COMMUNICATION);
        }
        // No-op when already on the requested route in communication mode. Re-applying
        // setSpeakerphoneOn(true) during SoftMigrate/reopen retriggers OEM device changes
        // (Moto MotSpeakerHelper) and races AAudio capture open → mic never comes back.
        if (wasInComm && am.isSpeakerphoneOn() == on) {
            if (on) {
                ensureStreamVolumeFloor(am, AudioManager.STREAM_VOICE_CALL);
            }
            return;
        }
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.S) {
            setCommunicationRoute(am, on);
        }
        am.setSpeakerphoneOn(on);
        if (on) {
            ensureStreamVolumeFloor(am, AudioManager.STREAM_VOICE_CALL);
        }
    }

    private void requestCallAudioFocus(AudioManager am, boolean mediaPath) {
        if (Build.VERSION.SDK_INT < Build.VERSION_CODES.O) {
            final int stream = mediaPath ? AudioManager.STREAM_MUSIC : AudioManager.STREAM_VOICE_CALL;
            @SuppressWarnings("deprecation")
            int ignored = am.requestAudioFocus(mCallAudioFocusListener, stream,
                    AudioManager.AUDIOFOCUS_GAIN_TRANSIENT);
            return;
        }
        if (mCallAudioFocusRequest != null) {
            return;
        }
        final int usage = mediaPath
                ? AudioAttributes.USAGE_MEDIA
                : AudioAttributes.USAGE_VOICE_COMMUNICATION;
        final int content = mediaPath
                ? AudioAttributes.CONTENT_TYPE_MUSIC
                : AudioAttributes.CONTENT_TYPE_SPEECH;
        AudioAttributes attrs = new AudioAttributes.Builder()
                .setUsage(usage)
                .setContentType(content)
                .build();
        mCallAudioFocusRequest = new AudioFocusRequest.Builder(AudioManager.AUDIOFOCUS_GAIN_TRANSIENT)
                .setAudioAttributes(attrs)
                .setAcceptsDelayedFocusGain(true)
                .setOnAudioFocusChangeListener(mCallAudioFocusListener)
                .build();
        am.requestAudioFocus(mCallAudioFocusRequest);
    }

    private void abandonCallAudioFocus(AudioManager am) {
        if (Build.VERSION.SDK_INT < Build.VERSION_CODES.O) {
            @SuppressWarnings("deprecation")
            int ignored = am.abandonAudioFocus(mCallAudioFocusListener);
            return;
        }
        if (mCallAudioFocusRequest != null) {
            am.abandonAudioFocusRequest(mCallAudioFocusRequest);
            mCallAudioFocusRequest = null;
        }
    }

    /** API 31+: prefer explicit communication device over deprecated speakerphone flag alone. */
    private void setCommunicationRoute(AudioManager am, boolean speakerOn) {
        final int wantType = speakerOn
                ? AudioDeviceInfo.TYPE_BUILTIN_SPEAKER
                : AudioDeviceInfo.TYPE_BUILTIN_EARPIECE;
        for (AudioDeviceInfo device : am.getAvailableCommunicationDevices()) {
            if (device.getType() == wantType) {
                if (!am.setCommunicationDevice(device)) {
                    Log.w(TAG, "setCommunicationDevice failed type=" + wantType);
                }
                return;
            }
        }
        Log.w(TAG, "No communication device for type=" + wantType);
    }

    /**
     * Raise a stream toward ~70% of max if near mute so call playout isn't whisper-quiet
     * (user can still turn it down). Phones: STREAM_VOICE_CALL; speaker-only tablets: MUSIC.
     */
    private void ensureStreamVolumeFloor(AudioManager am, int streamType) {
        final int max = am.getStreamMaxVolume(streamType);
        if (max <= 0) {
            return;
        }
        final int cur = am.getStreamVolume(streamType);
        final int floor = Math.max(1, (max * 7) / 10);
        if (cur < floor) {
            am.setStreamVolume(streamType, floor, 0);
            Log.i(TAG, "Raised stream " + streamType + " volume " + cur + " -> " + floor
                    + " (max=" + max + ")");
        }
    }

    /** Called from native NetworkConnectivity (N025 Wi‑Fi gate). */
    public boolean isActiveNetworkWifi() {
        ConnectivityManager cm = getSystemService(ConnectivityManager.class);
        if (cm == null) {
            return false;
        }
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.M) {
            Network network = cm.getActiveNetwork();
            if (network == null) {
                return false;
            }
            NetworkCapabilities caps = cm.getNetworkCapabilities(network);
            if (caps == null) {
                return false;
            }
            return caps.hasTransport(NetworkCapabilities.TRANSPORT_WIFI);
        }
        @SuppressWarnings("deprecation")
        android.net.NetworkInfo info = cm.getActiveNetworkInfo();
        if (info == null || !info.isConnected()) {
            return false;
        }
        return info.getType() == ConnectivityManager.TYPE_WIFI;
    }

    /** Called from native AndroidLocalNotifier. */
    public void showLocalNotification(String title, String body, String threadId) {
        ensureNotificationChannel();
        Intent open = new Intent(this, MainActivity.class);
        open.setFlags(Intent.FLAG_ACTIVITY_SINGLE_TOP | Intent.FLAG_ACTIVITY_CLEAR_TOP);
        if (threadId != null && !threadId.isEmpty()) {
            open.putExtra("thread_id", threadId);
        }
        int req = threadId == null ? 0 : threadId.hashCode();
        PendingIntent pi = PendingIntent.getActivity(
                this, req, open, PendingIntent.FLAG_UPDATE_CURRENT | PendingIntent.FLAG_IMMUTABLE);
        NotificationCompat.Builder builder = new NotificationCompat.Builder(this, CHANNEL_MESSAGES)
                .setSmallIcon(android.R.drawable.stat_notify_chat)
                .setContentTitle(title != null ? title : "New message")
                .setContentText(body != null ? body : "You have a new message")
                .setContentIntent(pi)
                .setAutoCancel(true)
                .setPriority(NotificationCompat.PRIORITY_HIGH);
        NotificationManagerCompat.from(this).notify(req == 0 ? 1 : req, builder.build());
    }

    public void clearLocalNotification(String threadId) {
        int req = threadId == null || threadId.isEmpty() ? 1 : threadId.hashCode();
        NotificationManagerCompat.from(this).cancel(req);
    }

    public String getStableDeviceId() {
        SharedPreferences prefs = getSharedPreferences(PREFS, MODE_PRIVATE);
        String id = prefs.getString("device_id", null);
        if (id == null || id.isEmpty()) {
            id = Settings.Secure.getString(getContentResolver(), Settings.Secure.ANDROID_ID);
            if (id == null || id.isEmpty()) {
                id = "android-" + System.currentTimeMillis();
            }
            prefs.edit().putString("device_id", id).apply();
        }
        return id;
    }

    private void ensureNotificationChannel() {
        if (Build.VERSION.SDK_INT < Build.VERSION_CODES.O) {
            return;
        }
        NotificationChannel channel = new NotificationChannel(
                CHANNEL_MESSAGES, "Messages", NotificationManager.IMPORTANCE_HIGH);
        channel.setDescription("Incoming chat messages");
        NotificationManager nm = getSystemService(NotificationManager.class);
        if (nm != null) {
            nm.createNotificationChannel(channel);
        }
    }

    private Handler ensurePixelCopyHandler() {
        if (mPixelCopyHandler == null) {
            mPixelCopyThread = new HandlerThread("pp-browser-pixelcopy");
            mPixelCopyThread.start();
            mPixelCopyHandler = new Handler(mPixelCopyThread.getLooper());
        }
        return mPixelCopyHandler;
    }

    private void captureRecentsThumbnailOnce() {
        if (!mThumbnailCapturedForPause.compareAndSet(false, true)) {
            return;
        }
        captureRecentsThumbnail();
    }

    private void captureRecentsThumbnail() {
        final SDLSurface surface = mSurface;
        if (surface == null || !surface.mIsSurfaceReady) {
            Log.v(TAG, "Recents thumbnail: surface not ready");
            return;
        }

        final int width = surface.getWidth();
        final int height = surface.getHeight();
        if (width <= 0 || height <= 0) {
            Log.v(TAG, "Recents thumbnail: invalid size " + width + "x" + height);
            return;
        }

        final Bitmap source;
        try {
            source = Bitmap.createBitmap(width, height, Bitmap.Config.ARGB_8888);
        } catch (OutOfMemoryError e) {
            Log.w(TAG, "Recents thumbnail: bitmap alloc failed", e);
            return;
        }

        final AtomicReference<Bitmap> result = new AtomicReference<>();
        final CountDownLatch latch = new CountDownLatch(1);
        final Handler handler = ensurePixelCopyHandler();

        try {
            PixelCopy.request(surface, source, copyResult -> {
                if (copyResult == PixelCopy.SUCCESS) {
                    result.set(source);
                } else {
                    source.recycle();
                    Log.v(TAG, "Recents thumbnail: PixelCopy failed (" + copyResult + ")");
                }
                latch.countDown();
            }, handler);
        } catch (IllegalArgumentException e) {
            source.recycle();
            Log.w(TAG, "Recents thumbnail: PixelCopy request rejected", e);
            return;
        }

        try {
            if (!latch.await(PIXEL_COPY_TIMEOUT_MS, TimeUnit.MILLISECONDS)) {
                Log.v(TAG, "Recents thumbnail: PixelCopy timed out");
                handler.postDelayed(() -> {
                    if (result.get() == null && !source.isRecycled()) {
                        source.recycle();
                    }
                }, 500);
                return;
            }
        } catch (InterruptedException e) {
            Thread.currentThread().interrupt();
            if (result.get() == null && !source.isRecycled()) {
                source.recycle();
            }
            return;
        }

        final Bitmap captured = result.get();
        if (captured == null) {
            return;
        }

        Bitmap icon = scaleForTaskDescription(captured);
        if (icon != captured) {
            captured.recycle();
        }

        try {
            applyTaskDescriptionIcon(icon);
        } catch (Exception e) {
            Log.w(TAG, "Recents thumbnail: setTaskDescription failed", e);
            if (!icon.isRecycled()) {
                icon.recycle();
            }
        }
    }

    private static Bitmap scaleForTaskDescription(Bitmap source) {
        final int maxEdge = Math.max(source.getWidth(), source.getHeight());
        if (maxEdge <= THUMBNAIL_MAX_EDGE) {
            return source;
        }
        final float scale = (float) THUMBNAIL_MAX_EDGE / (float) maxEdge;
        final int w = Math.max(1, Math.round(source.getWidth() * scale));
        final int h = Math.max(1, Math.round(source.getHeight() * scale));
        return Bitmap.createScaledBitmap(source, w, h, true);
    }

    @SuppressWarnings("deprecation")
    private void applyTaskDescriptionIcon(Bitmap icon) {
        setTaskDescription(new ActivityManager.TaskDescription(getString(R.string.app_name), icon));
        Log.v(TAG, "Recents thumbnail updated (" + icon.getWidth() + "x" + icon.getHeight() + ")");
    }
}
