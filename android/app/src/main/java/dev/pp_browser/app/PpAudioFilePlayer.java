package dev.pp_browser.app;

import android.media.AudioAttributes;
import android.media.MediaPlayer;
import android.os.Handler;
import android.os.Looper;
import android.util.Log;

/**
 * A sound file sent as an attachment (MP3, M4A, WAV ...), played in the app through MediaPlayer with the media usage
 * (the loudspeaker at media volume, like voice messages). One at a time. Called from native
 * (AudioFilePlayer_Android.cpp) through MainActivity; reports progress about ten times a second and the end on the
 * main thread, tagged with the native side's token.
 */
final class PpAudioFilePlayer {
    private static final String TAG = "pp-browser";
    private static final Handler MAIN = new Handler(Looper.getMainLooper());
    private static MediaPlayer player;
    private static long token;

    private PpAudioFilePlayer() {}

    static native void nativeAudioFileProgress(long token, int positionMs, int durationMs);

    static native void nativeAudioFileEnded(long token, boolean finished);

    private static final Runnable TICK = new Runnable() {
        @Override
        public void run() {
            synchronized (PpAudioFilePlayer.class) {
                if (player == null) {
                    return;
                }
                if (player.isPlaying()) {
                    report(token, player.getCurrentPosition(), player.getDuration());
                }
            }
            MAIN.postDelayed(this, 100);
        }
    };

    static synchronized boolean play(String path, long playToken) {
        stop();
        final MediaPlayer p = new MediaPlayer();
        try {
            p.setAudioAttributes(new AudioAttributes.Builder()
                    .setUsage(AudioAttributes.USAGE_MEDIA)
                    .setContentType(AudioAttributes.CONTENT_TYPE_MUSIC)
                    .build());
            p.setDataSource(path);
            p.prepare();
        } catch (Exception e) {
            Log.w(TAG, "audio file: cannot prepare: " + e.getClass().getSimpleName());
            p.release();
            return false;
        }
        p.setOnCompletionListener(mp -> end(playToken, true));
        p.setOnErrorListener((mp, what, extra) -> {
            end(playToken, false);
            return true;
        });
        p.start();
        player = p;
        token = playToken;
        MAIN.removeCallbacks(TICK);
        MAIN.post(TICK);
        return true;
    }

    static synchronized void pause() {
        if (player != null && player.isPlaying()) {
            player.pause();
        }
    }

    static synchronized void resume() {
        if (player != null && !player.isPlaying()) {
            player.start();
        }
    }

    /** Jumps to `positionMs` (playing or paused). */
    static synchronized void seek(int positionMs) {
        if (player != null) {
            try {
                player.seekTo(Math.max(0, positionMs));
            } catch (IllegalStateException ignored) {
                // released meanwhile
            }
        }
    }

    static synchronized void stop() {
        MAIN.removeCallbacks(TICK);
        if (player != null) {
            try {
                player.stop();
            } catch (IllegalStateException ignored) {
                // never started, or already ended
            }
            player.release();
            player = null;
        }
        token = 0;
    }

    private static void end(long playToken, boolean finished) {
        synchronized (PpAudioFilePlayer.class) {
            if (playToken != token) {
                return;
            }
            stop();
        }
        try {
            nativeAudioFileEnded(playToken, finished);
        } catch (UnsatisfiedLinkError ignored) {
            // native not loaded: nobody is listening
        }
    }

    private static void report(long playToken, int positionMs, int durationMs) {
        try {
            nativeAudioFileProgress(playToken, positionMs, durationMs);
        } catch (UnsatisfiedLinkError ignored) {
            // native not loaded
        }
    }
}
