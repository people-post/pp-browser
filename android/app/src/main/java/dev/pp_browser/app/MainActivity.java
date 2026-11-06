package dev.pp_browser.app;

import android.Manifest;
import android.app.ActivityManager;
import android.app.NotificationChannel;
import android.app.NotificationManager;
import android.app.PendingIntent;
import android.content.Context;
import android.content.Intent;
import android.content.SharedPreferences;
import android.content.pm.ApplicationInfo;
import android.content.pm.PackageManager;
import android.content.res.Configuration;
import android.graphics.Bitmap;
import android.graphics.Rect;
import android.annotation.TargetApi;
import android.media.AudioAttributes;
import android.media.AudioDeviceInfo;
import android.media.AudioFocusRequest;
import android.media.AudioManager;
import android.net.ConnectivityManager;
import android.net.Network;
import android.net.NetworkCapabilities;
import android.net.Uri;
import android.os.Build;
import android.os.Handler;
import android.os.HandlerThread;
import android.provider.Settings;
import android.util.Log;
import android.view.PixelCopy;
import android.view.View;
import android.view.ViewGroup;
import android.view.ViewTreeObserver;
import android.view.Window;
import android.view.WindowManager;
import android.webkit.MimeTypeMap;

import androidx.core.app.NotificationCompat;
import androidx.core.app.NotificationManagerCompat;
import androidx.core.content.ContextCompat;
import androidx.core.content.FileProvider;
import androidx.core.graphics.Insets;
import androidx.core.view.ViewCompat;
import androidx.core.view.WindowCompat;
import androidx.core.view.WindowInsetsAnimationCompat;
import androidx.core.view.WindowInsetsCompat;
import androidx.core.view.WindowInsetsControllerCompat;

import org.libsdl.app.SDLActivity;
import org.libsdl.app.SDLSurface;

import java.io.File;
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
 * torn down, so Recents / app-switcher is less likely to show a black tile. Never while a secret is
 * on screen ({@link #setSecureScreen}): then the window is secure and the task's icon is the app's.
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
 * Native argv (via {@link #getArguments()}), for a developer's {@code adb shell am start}:
 * <ul>
 *   <li>{@code --ez startup_timing true} → {@code --startup-timing}</li>
 *   <li>{@code --ez debug true} → {@code --debug}</li>
 *   <li>{@code -e pp_native_args "--startup-timing --debug"} → split on whitespace; debuggable builds only</li>
 * </ul>
 * A launch from a link (contact invites: VIEW / BROWSABLE) carries none — a web page can put extras on
 * an {@code intent:} URI, and argv takes {@code --config}, {@code --profile}, {@code --pin} (PB-026).
 */
public class MainActivity extends SDLActivity {
    private static final String TAG = "pp-browser";
    private static final int THUMBNAIL_MAX_EDGE = 512;
    private static final long PIXEL_COPY_TIMEOUT_MS = 80;
    private static final String CHANNEL_MESSAGES = "messages";
    private static final String PREFS = "pp_browser_device";
    /** Me → Show notifications, kept here for a push that comes while the app is not running. */
    private static final String PREF_MESSAGE_BANNERS = "message_banners_on";
    /** The "New message" a push posts while the app is not running (it cannot say who or what). */
    private static final int PUSH_MESSAGE_NOTIFICATION_ID = 0x5052;
    /** Our own permission request (SDL's and the camera flow's codes go to them). */
    private static final int REQ_NOTIFICATIONS = 0x5053;
    /** Between onResume and onPause: a call push is rung by the app itself. */
    private static volatile boolean sInFront;
    /** Main thread: the call this device is in (native OngoingCallService), and whether PpCallService runs for it. */
    private static boolean sOngoingCall;
    private static String sOngoingCallTitle;
    private static boolean sOngoingCallServiceStarted;

    private HandlerThread mPixelCopyThread;
    private Handler mPixelCopyHandler;
    private final AtomicBoolean mThumbnailCapturedForPause = new AtomicBoolean(false);
    /** A secret is on screen (native SecureScreen): no screenshot of it anywhere, ours included. */
    private volatile boolean mSecureScreen;
    private AudioFocusRequest mCallAudioFocusRequest;
    /**
     * A phone call (or another app's call) takes the call's audio focus: the call is interrupted until focus is back
     * (native AudioSession: its peer is told it is on hold, and its devices reopen at the end). A duck is not a loss.
     */
    private final AudioManager.OnAudioFocusChangeListener mCallAudioFocusListener = focusChange -> {
        if (focusChange == AudioManager.AUDIOFOCUS_LOSS || focusChange == AudioManager.AUDIOFOCUS_LOSS_TRANSIENT) {
            onCallAudioFocus(true);
        } else if (focusChange == AudioManager.AUDIOFOCUS_GAIN) {
            onCallAudioFocus(false);
        }
    };

    private static native void nativeOnCallAudioFocus(boolean lost);

    private static void onCallAudioFocus(boolean lost) {
        Log.i(TAG, lost ? "call audio focus lost" : "call audio focus back");
        try {
            nativeOnCallAudioFocus(lost);
        } catch (UnsatisfiedLinkError ignored) {
        }
    }

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
        if (intent == null || launchedFromLink(intent)) {
            return new String[0];
        }
        final ArrayList<String> args = new ArrayList<>();
        if (intent.getBooleanExtra("startup_timing", false)) {
            args.add("--startup-timing");
        }
        if (intent.getBooleanExtra("debug", false)) {
            args.add("--debug");
        }
        // Any argv (another app could send it, explicitly addressed): a developer build only.
        final String raw = isDebuggableBuild() ? intent.getStringExtra("pp_native_args") : null;
        if (raw != null && !raw.trim().isEmpty()) {
            for (String part : raw.trim().split("\\s+")) {
                if (!part.isEmpty()) {
                    args.add(part);
                }
            }
        }
        return args.toArray(new String[0]);
    }

    private static boolean launchedFromLink(final Intent intent) {
        return Intent.ACTION_VIEW.equals(intent.getAction()) || intent.hasCategory(Intent.CATEGORY_BROWSABLE);
    }

    private boolean isDebuggableBuild() {
        return (getApplicationInfo().flags & ApplicationInfo.FLAG_DEBUGGABLE) != 0;
    }

    @Override
    protected void onCreate(android.os.Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);
        installKeyboardSafeAreaPublisher();
        applyNavigationBarColor();
        ensureNotificationChannel();
        InboxSyncWorker.schedule(this, true);
        PpLinkBridge.deliver(getIntent());
        PpCameraCapture.clearScratch(this);
        clearOpenDir(this);
        PpCallNotifications.init(this);
        if (savedInstanceState == null) {
            PpCallNotifications.onActivityIntent(getIntent());  // Answer on a call notification started the app
        }
        PpFirebaseMessagingService.requestToken();
        requestNotificationPermission();
    }

    /** Like iOS at launch: calls ring by notification, and messages show by it. Android 13+ asks. */
    private void requestNotificationPermission() {
        if (Build.VERSION.SDK_INT < Build.VERSION_CODES.TIRAMISU
                || checkSelfPermission(Manifest.permission.POST_NOTIFICATIONS) == PackageManager.PERMISSION_GRANTED) {
            return;
        }
        requestPermissions(new String[] {Manifest.permission.POST_NOTIFICATIONS}, REQ_NOTIFICATIONS);
    }

    /** The app's native side is running (its SDL main thread). */
    static boolean isAppRunning() {
        final Thread thread = mSDLThread;
        return thread != null && thread.isAlive() && !mSDLMainFinished;
    }

    static boolean isAppInFront() {
        return sInFront && isAppRunning();
    }

    /** A message push while the app is not running: "New message", unless Me → Show notifications is off. */
    static void showPushMessageNotification(Context context) {
        final Context c = context.getApplicationContext();
        if (!c.getSharedPreferences(PREFS, MODE_PRIVATE).getBoolean(PREF_MESSAGE_BANNERS, true)) {
            return;
        }
        ensureMessagesChannel(c);
        Intent open = new Intent(c, MainActivity.class);
        open.setFlags(Intent.FLAG_ACTIVITY_NEW_TASK | Intent.FLAG_ACTIVITY_SINGLE_TOP);
        PendingIntent pi = PendingIntent.getActivity(
                c, PUSH_MESSAGE_NOTIFICATION_ID, open, PendingIntent.FLAG_UPDATE_CURRENT | PendingIntent.FLAG_IMMUTABLE);
        NotificationCompat.Builder builder = new NotificationCompat.Builder(c, CHANNEL_MESSAGES)
                .setSmallIcon(android.R.drawable.stat_notify_chat)
                .setContentTitle(c.getString(R.string.message_incoming_title))
                .setContentText(c.getString(R.string.message_incoming_body))
                .setContentIntent(pi)
                .setAutoCancel(true)
                .setPriority(NotificationCompat.PRIORITY_HIGH);
        try {
            NotificationManagerCompat.from(c).notify(PUSH_MESSAGE_NOTIFICATION_ID, builder.build());
        } catch (SecurityException e) {
            Log.w(TAG, "message notification refused: " + e);  // notifications not allowed
        }
    }

    /** Called from native (CameraCapture_Android.cpp) from any thread: opens the system camera. */
    public static void startCameraCapture(final int kind, final int maxSeconds, final long maxBytes) {
        final SDLActivity activity = mSingleton;
        if (!(activity instanceof MainActivity)) {
            PpCameraCapture.deliver(PpCameraCapture.OUTCOME_UNAVAILABLE, null);
            return;
        }
        activity.runOnUiThread(() -> PpCameraCapture.start(activity, kind, maxSeconds, maxBytes));
    }

    /** Called from native: {@code <cacheDir>/capture}, where captured files are written. */
    public static String captureScratchDir() {
        final SDLActivity activity = mSingleton;
        return activity == null ? null : PpCameraCapture.captureDir(activity).getAbsolutePath();
    }

    /** The only folder shared with other apps (pp_file_paths.xml "open"): links to attachments being opened. */
    private static final String OPEN_DIR = "open";
    /** A link older than this is gone at the next open (the viewer it was for has read it by then). */
    private static final long OPEN_KEEP_MS = 10 * 60 * 1000;

    /**
     * Called from native (PlatformOpenFile_Android.cpp): shows a downloaded attachment in an app that can open it.
     * Only an attachment view under {@code <filesDir>/pp-browser/.../blobs_view/} is accepted (the profile's
     * databases and keys never are). It is linked (else copied) into {@code <cacheDir>/open/} under its own name and
     * that one file goes as a FileProvider address with read permission. A file:// address is refused by Android 7+.
     */
    public static boolean openFile(final String path) {
        final SDLActivity activity = mSingleton;
        if (activity == null || path == null) {
            return false;
        }
        try {
            final File source = new File(path).getCanonicalFile();
            final String root = new File(activity.getFilesDir(), "pp-browser").getCanonicalPath() + File.separator;
            final String views = File.separator + "blobs_view" + File.separator;
            if (!source.getPath().startsWith(root) || !source.getPath().contains(views) || !source.isFile()) {
                Log.w(TAG, "openFile: refused, not an attachment view");
                return false;
            }
            final File dir = new File(activity.getCacheDir(), OPEN_DIR);
            if (!dir.isDirectory() && !dir.mkdirs()) {
                Log.w(TAG, "openFile: no share folder");
                return false;
            }
            final long now = System.currentTimeMillis();
            final File[] old = dir.listFiles();
            if (old != null) {
                for (File f : old) {
                    if (now - f.lastModified() > OPEN_KEEP_MS) {
                        f.delete();
                    }
                }
            }
            final File shared = new File(dir, source.getName());
            shared.delete();
            try {
                android.system.Os.link(source.getPath(), shared.getPath());
            } catch (Exception linkFailed) {
                java.nio.file.Files.copy(source.toPath(), shared.toPath());
            }
            shared.setLastModified(now);
            final Uri uri;
            try {
                uri = FileProvider.getUriForFile(activity, activity.getPackageName() + ".capture", shared);
            } catch (IllegalArgumentException e) {
                Log.w(TAG, "openFile: the share folder is not in the provider's paths");
                return false;
            }
            final String name = shared.getName();
            final int dot = name.lastIndexOf('.');
            final String ext = dot < 0 ? "" : name.substring(dot + 1).toLowerCase(java.util.Locale.ROOT);
            final String mime = MimeTypeMap.getSingleton().getMimeTypeFromExtension(ext);
            final Intent intent = new Intent(Intent.ACTION_VIEW);
            intent.setDataAndType(uri, mime != null ? mime : "*/*");
            intent.addFlags(Intent.FLAG_GRANT_READ_URI_PERMISSION);
            activity.startActivity(intent);
            return true;
        } catch (Exception e) {  // no app for it (ActivityNotFoundException), an unreadable file
            Log.w(TAG, "openFile: " + e.getClass().getSimpleName());
            return false;
        }
    }

    /** At start: the links of the last session go (the views they point at were wiped too). */
    private static void clearOpenDir(Context context) {
        final File[] files = new File(context.getCacheDir(), OPEN_DIR).listFiles();
        if (files != null) {
            for (File f : files) {
                f.delete();
            }
        }
    }

    /** Called from native (AudioFilePlayer_Android.cpp): plays a sound file attachment in the app. */
    public static boolean playAudioFile(final String path, final long token) {
        return PpAudioFilePlayer.play(path, token);
    }

    public static void pauseAudioFile() {
        PpAudioFilePlayer.pause();
    }

    public static void resumeAudioFile() {
        PpAudioFilePlayer.resume();
    }

    public static void stopAudioFile() {
        PpAudioFilePlayer.stop();
    }

    /** Only the camera flow's fixed request codes are handled here; SDL's pickers depend on super for the rest. */
    @Override
    protected void onActivityResult(int requestCode, int resultCode, Intent data) {
        if (PpCameraCapture.onActivityResult(this, requestCode, resultCode, data)) {
            return;
        }
        super.onActivityResult(requestCode, resultCode, data);
    }

    /** Same rule: SDL's call permission requests depend on super for every code that is not ours. */
    @Override
    public void onRequestPermissionsResult(int requestCode, String[] permissions, int[] grantResults) {
        if (requestCode == REQ_NOTIFICATIONS) {
            return;  // nothing waits on it: notifications are posted or refused as they come
        }
        if (PpCameraCapture.onPermissionsResult(this, requestCode, grantResults)) {
            return;
        }
        super.onRequestPermissionsResult(requestCode, permissions, grantResults);
    }

    @Override
    protected void onNewIntent(Intent intent) {
        super.onNewIntent(intent);
        setIntent(intent);
        PpLinkBridge.deliver(intent);
        PpCallNotifications.onActivityIntent(intent);
    }

    @Override
    protected void onDestroy() {
        PpCallNotifications.appGone();
        if (sOngoingCallServiceStarted) {
            PpCallService.stop(getApplicationContext());  // the call goes with the app's native side
        }
        sOngoingCall = false;
        sOngoingCallServiceStarted = false;
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
     * While this publisher is installed it is the only one: {@link SDLSurface} skips its own
     * publish ({@code sActivityPublishesInsets}) because it runs after this listener in the same
     * frame and some OEM IMEs (vivo OriginOS, {@code adjustNothing}) report
     * {@code WindowInsets.Type.ime()} as 0 while the keyboard is visible, which overwrote the
     * keyboard-aware value with 0. Static safe area: machine-prefs (API &lt; 30) or the same
     * system-inset types SDLSurface used (API 30+); the soft keyboard is max'd into bottom.
     */
    private void installKeyboardSafeAreaPublisher() {
        if (mLayout == null) {
            Log.w(TAG, "Keyboard safe-area: mLayout null after onCreate");
            return;
        }
        final View root = mLayout;
        org.libsdl.app.SDLSurface.sActivityPublishesInsets = true;

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
        org.libsdl.app.SDLSurface.sActivityPublishesInsets = false;
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
            // Same type set SDLSurface.onApplyWindowInsets publishes, minus ime (handled below).
            final Insets sys = windowInsets.getInsets(
                    WindowInsetsCompat.Type.systemBars()
                            | WindowInsetsCompat.Type.systemGestures()
                            | WindowInsetsCompat.Type.mandatorySystemGestures()
                            | WindowInsetsCompat.Type.tappableElement()
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
        // WindowInsets measure the IME from the window's bottom edge, but the SDL surface is a
        // fitted view that already ends above the navigation bar (vivo S15e, 3-button nav:
        // surface is 2182 of 2404 px). Only the part above that gap covers the surface:
        // 979 - 126 = 853.
        final int surfaceGap = surfaceGapBelowWindowPx(root);
        int imeFromInsets = Math.max(0, imeBottomFromInsets);
        final WindowInsetsCompat rootInsets = ViewCompat.getRootWindowInsets(root);
        if (rootInsets != null) {
            imeFromInsets = Math.max(imeFromInsets,
                    rootInsets.getInsets(WindowInsetsCompat.Type.ime()).bottom);
        }
        int keyboardBottom = Math.max(0, imeFromInsets - surfaceGap);

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

        // Hidden IMM height only when WindowInsets reported no keyboard (API < 30 / broken
        // OEM dispatch): its reference edge varies by OEM (vivo: surface-relative 853, others:
        // the full IME window), so it cannot be corrected by the gap.
        if (imeFromInsets == 0) {
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
        }
        return keyboardBottom;
    }

    /**
     * Distance from the SDL surface's bottom edge to the window's (decor view's) bottom edge:
     * the navigation bar on a fitted window, 0 edge-to-edge. Window-relative, so split-screen
     * and freeform windows are handled the same as full screen.
     */
    private int surfaceGapBelowWindowPx(View root) {
        final View surface = mSurface;
        final View decor = root.getRootView();
        if (surface == null || decor == null || surface.getHeight() <= 0) {
            return 0;
        }
        final int[] surfaceLoc = new int[2];
        final int[] decorLoc = new int[2];
        surface.getLocationOnScreen(surfaceLoc);
        decor.getLocationOnScreen(decorLoc);
        return Math.max(0, (decorLoc[1] + decor.getHeight()) - (surfaceLoc[1] + surface.getHeight()));
    }

    /**
     * SDL's text input is up and the input method takes it — SDL 3.2's isScreenKeyboardShown(),
     * which SDL 3.4 dropped (its own keyboard tracking runs on Android 11+ only). SDL hides its edit
     * by sizing it 0x0, not by visibility.
     */
    private boolean isScreenKeyboardShown() {
        if (mTextEdit == null || mTextEdit.getVisibility() != View.VISIBLE) {
            return false;
        }
        final ViewGroup.LayoutParams params = mTextEdit.getLayoutParams();
        if (params == null || (params.width == 0 && params.height == 0)) {
            return false;
        }
        final android.view.inputmethod.InputMethodManager imm =
                (android.view.inputmethod.InputMethodManager) getSystemService(INPUT_METHOD_SERVICE);
        return imm != null && imm.isAcceptingText();
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
        sInFront = true;
        if (sOngoingCall && !sOngoingCallServiceStarted) {
            startOngoingCallServiceIfAllowed();  // the call started while the app was in the background
        }
        // The app is open: what the push's "New message" stood for shows in the app.
        NotificationManagerCompat.from(this).cancel(PUSH_MESSAGE_NOTIFICATION_ID);
        mKeyboardProbeHandler.removeCallbacks(mKeyboardProbeRunnable);
        mKeyboardProbeHandler.post(mKeyboardProbeRunnable);
    }

    @Override
    protected void onPause() {
        sInFront = false;
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
     * Called from native {@code AudioSession} when a VoIP call starts/ends.
     * Phones: {@link AudioManager#MODE_IN_COMMUNICATION} for earpiece/speaker routing.
     * Speaker-only tablets (no earpiece): stay in {@link AudioManager#MODE_NORMAL} so
     * SDL AAudio MEDIA streams are not ducked (API &lt; 28 cannot set
     * {@code AAUDIO_USAGE_VOICE_COMMUNICATION} — SM-T380 whisper-quiet dogfood).
     */
    public void setAudioSessionActive(boolean active) {
        AudioManager am = (AudioManager) getSystemService(AUDIO_SERVICE);
        if (am == null) {
            return;
        }
        if (active) {
            final boolean speakerOnly = !hasEarpieceRoute();
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
     * Called from native {@code AudioSession.SupportsSpeakerToggle}.
     * Phones expose {@link AudioDeviceInfo#TYPE_BUILTIN_EARPIECE}; tablets (e.g. SM-T380)
     * only have the loudspeaker, so FORCE_SPEAKER vs FORCE_NONE is inaudible.
     */
    public boolean hasEarpieceRoute() {
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
     * Called from native {@code AudioSession} for in-call speaker / earpiece.
     * Synchronous: ToggleSpeaker reopens SDL capture immediately after this returns.
     */
    public void setVoipSpeakerphoneOn(boolean on) {
        AudioManager am = (AudioManager) getSystemService(AUDIO_SERVICE);
        if (am == null) {
            return;
        }
        // Speaker-only tablets: no earpiece route; keep MODE_NORMAL (see setAudioSessionActive).
        if (!hasEarpieceRoute()) {
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
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.S) {
            // One communication device. setSpeakerphoneOn is mapped onto it there, so calling it after
            // setCommunicationDevice would undo a headset choice.
            setCommunicationRoute(am, on);
            if (on) {
                ensureStreamVolumeFloor(am, AudioManager.STREAM_VOICE_CALL);
            }
            return;
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

    /**
     * API 31+: the call's communication device — the loudspeaker when chosen, else a connected
     * headset, else the earpiece, as the phone's own calls do. A no-op when already there:
     * re-applying a route retriggers OEM device changes (Moto MotSpeakerHelper, see
     * setVoipSpeakerphoneOn).
     */
    @TargetApi(Build.VERSION_CODES.S)
    private void setCommunicationRoute(AudioManager am, boolean speakerOn) {
        // When several headsets are connected, the first type here wins.
        final int[] headsetTypes = {
            AudioDeviceInfo.TYPE_HEARING_AID,
            AudioDeviceInfo.TYPE_WIRED_HEADSET,
            AudioDeviceInfo.TYPE_USB_HEADSET,
            AudioDeviceInfo.TYPE_WIRED_HEADPHONES,
            AudioDeviceInfo.TYPE_BLE_HEADSET,
            AudioDeviceInfo.TYPE_BLUETOOTH_SCO,
        };
        final List<AudioDeviceInfo> devices = am.getAvailableCommunicationDevices();
        AudioDeviceInfo want = null;
        if (speakerOn) {
            want = findDeviceOfType(devices, AudioDeviceInfo.TYPE_BUILTIN_SPEAKER);
        } else {
            for (int type : headsetTypes) {
                want = findDeviceOfType(devices, type);
                if (want != null) {
                    break;
                }
            }
            if (want == null) {
                want = findDeviceOfType(devices, AudioDeviceInfo.TYPE_BUILTIN_EARPIECE);
            }
        }
        if (want == null) {
            Log.w(TAG, "No communication device speaker=" + speakerOn);
            return;
        }
        final AudioDeviceInfo current = am.getCommunicationDevice();
        if (current != null && current.getId() == want.getId()) {
            return;
        }
        if (am.setCommunicationDevice(want)) {
            Log.i(TAG, "Call audio route type=" + want.getType());
        } else {
            Log.w(TAG, "setCommunicationDevice failed type=" + want.getType());
        }
    }

    private static AudioDeviceInfo findDeviceOfType(List<AudioDeviceInfo> devices, int type) {
        for (AudioDeviceInfo device : devices) {
            if (device.getType() == type) {
                return device;
            }
        }
        return null;
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

    private PpNetworkMonitor mNetworkMonitor;

    /** Called from native NetworkMonitor (call-path-resilience k5). */
    public void startNetworkMonitor() {
        synchronized (this) {
            if (mNetworkMonitor == null) {
                mNetworkMonitor = new PpNetworkMonitor(this);
            }
            mNetworkMonitor.start();
        }
    }

    /** Called from native NetworkMonitor. */
    public void stopNetworkMonitor() {
        synchronized (this) {
            if (mNetworkMonitor != null) {
                mNetworkMonitor.stop();
            }
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

    /** P011 — called from native (StayConnectedService_Android) when "Stay connected" changes. */
    public void setStayConnected(boolean on) {
        if (on) {
            PpConnectionService.start(getApplicationContext());
        } else {
            PpConnectionService.stop(getApplicationContext());
        }
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

    /**
     * Called from native SecureScreen when a secret comes on screen or goes (the PIN gate; Me → Account &
     * security, where the recovery phrase shows). Secure: no screenshot, screen recording, cast or Recents
     * preview of the window, and the task's icon goes back to the app's (it may hold an earlier screenshot).
     */
    public void setSecureScreen(boolean on) {
        mSecureScreen = on;
        runOnUiThread(() -> {
            final Window window = getWindow();
            if (on) {
                window.addFlags(WindowManager.LayoutParams.FLAG_SECURE);
                setPlainTaskDescription();
            } else {
                window.clearFlags(WindowManager.LayoutParams.FLAG_SECURE);
            }
        });
    }

    @SuppressWarnings("deprecation")
    private void setPlainTaskDescription() {
        setTaskDescription(new ActivityManager.TaskDescription(getString(R.string.app_name)));
    }

    /**
     * Called from native OngoingCallService (the app's UI thread): the call this device is in, or none. The service
     * starts only with the app in front and the microphone allowed — a refused start after the launch would crash the
     * app (startForegroundService without startForeground) — so a call that started in the background gets it at the
     * next resume.
     */
    public void setOngoingCall(boolean on, String title) {
        runOnUiThread(() -> {
            sOngoingCall = on;
            sOngoingCallTitle = title;
            if (on) {
                startOngoingCallServiceIfAllowed();
            } else if (sOngoingCallServiceStarted) {
                sOngoingCallServiceStarted = false;
                PpCallService.stop(getApplicationContext());
            }
        });
    }

    private void startOngoingCallServiceIfAllowed() {
        if (!sOngoingCall || !sInFront
                || checkSelfPermission(Manifest.permission.RECORD_AUDIO) != PackageManager.PERMISSION_GRANTED) {
            return;
        }
        sOngoingCallServiceStarted = true;
        PpCallService.start(getApplicationContext(), sOngoingCallTitle);  // a running one takes the new title
    }

    /** Called from native AndroidLocalNotifier when Me → Show notifications changes (any thread). */
    public void setMessageBannersOn(boolean on) {
        getSharedPreferences(PREFS, MODE_PRIVATE).edit().putBoolean(PREF_MESSAGE_BANNERS, on).apply();
    }

    // Called from native AndroidCallSystemUi (the app's UI thread): the incoming-call notification.
    public boolean callNotificationsAvailable() {
        return PpCallNotifications.available();
    }

    public boolean hasPendingPushCall() {
        return PpCallNotifications.hasPendingPushCall();
    }

    public void callMessagingReady() {
        PpCallNotifications.messagingReady();
    }

    public void endPendingPushCall() {
        PpCallNotifications.endPendingPushCall();
    }

    public void showIncomingCall(String callId, String callerName, boolean video) {
        PpCallNotifications.showIncoming(callId, callerName, video);
    }

    public void endIncomingCall(String callId) {
        PpCallNotifications.endIncoming(callId);
    }

    public void clearLocalNotification(String threadId) {
        int req = threadId == null || threadId.isEmpty() ? 1 : threadId.hashCode();
        NotificationManagerCompat.from(this).cancel(req);
    }

    /** Ordered BCP-47 tags from Settings → System → Languages, comma-separated (B80: PP follows the first one it ships). */
    public String getPreferredLanguageTags() {
        return android.os.LocaleList.getDefault().toLanguageTags();
    }

    /**
     * For DeviceVitals_Android.cpp, read every couple of seconds in a call: {power saver on (1/0), thermal state
     * 0 nominal .. 3 critical (Apple's four; -1 before API 29), battery percent (-1 unknown), charging (1/0)}.
     */
    public int[] getDeviceVitals() {
        int powerSave = 0;
        int thermal = -1;
        android.os.PowerManager pm = (android.os.PowerManager) getSystemService(POWER_SERVICE);
        if (pm != null) {
            powerSave = pm.isPowerSaveMode() ? 1 : 0;
            if (Build.VERSION.SDK_INT >= 29) {
                // NONE, LIGHT, MODERATE / SEVERE, CRITICAL and above.
                final int status = pm.getCurrentThermalStatus();
                thermal = status <= 0 ? 0 : status == 1 ? 1 : status <= 3 ? 2 : 3;
            }
        }
        int battery = -1;
        int charging = 0;
        android.os.BatteryManager bm = (android.os.BatteryManager) getSystemService(BATTERY_SERVICE);
        if (bm != null) {
            battery = bm.getIntProperty(android.os.BatteryManager.BATTERY_PROPERTY_CAPACITY);
            if (battery < 0 || battery > 100) {
                battery = -1;
            }
            charging = bm.isCharging() ? 1 : 0;
        }
        return new int[] {powerSave, thermal, battery, charging};
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
        ensureMessagesChannel(this);
    }

    private static void ensureMessagesChannel(Context c) {
        if (Build.VERSION.SDK_INT < Build.VERSION_CODES.O) {
            return;
        }
        NotificationChannel channel = new NotificationChannel(
                CHANNEL_MESSAGES, "Messages", NotificationManager.IMPORTANCE_HIGH);
        channel.setDescription("Incoming chat messages");
        NotificationManager nm = c.getSystemService(NotificationManager.class);
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
        if (mSecureScreen || !mThumbnailCapturedForPause.compareAndSet(false, true)) {
            return;
        }
        captureRecentsThumbnail();
    }

    private void captureRecentsThumbnail() {
        final SDLSurface surface = mSurface;
        if (surface == null || !surface.getHolder().getSurface().isValid()) {
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
