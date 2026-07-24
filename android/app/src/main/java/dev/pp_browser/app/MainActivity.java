package dev.pp_browser.app;

import android.app.ActivityManager;
import android.app.NotificationChannel;
import android.app.NotificationManager;
import android.app.PendingIntent;
import android.content.Intent;
import android.content.SharedPreferences;
import android.content.res.Configuration;
import android.graphics.Bitmap;
import android.os.Build;
import android.os.Handler;
import android.os.HandlerThread;
import android.provider.Settings;
import android.util.Log;
import android.view.PixelCopy;
import android.view.View;
import android.view.Window;
import android.view.WindowManager;

import androidx.core.app.NotificationCompat;
import androidx.core.app.NotificationManagerCompat;
import androidx.core.content.ContextCompat;
import androidx.core.view.WindowCompat;
import androidx.core.view.WindowInsetsControllerCompat;

import org.libsdl.app.SDLActivity;
import org.libsdl.app.SDLSurface;

import java.util.ArrayList;
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

    /** App appearance preference from native Theme ("system" / "light" / "dark"). */
    private volatile String mAppAppearance = "system";

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
        if (mPixelCopyThread != null) {
            mPixelCopyThread.quitSafely();
            mPixelCopyThread = null;
            mPixelCopyHandler = null;
        }
        super.onDestroy();
    }

    @Override
    protected void onResume() {
        mThumbnailCapturedForPause.set(false);
        super.onResume();
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
    protected void onPause() {
        captureRecentsThumbnailOnce();
        super.onPause();
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
