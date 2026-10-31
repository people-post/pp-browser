package dev.pp_browser.app;

import android.app.Activity;
import android.content.ClipData;
import android.content.ContentResolver;
import android.content.Context;
import android.content.Intent;
import android.content.pm.PackageManager;
import android.media.MediaExtractor;
import android.media.MediaFormat;
import android.media.MediaMetadataRetriever;
import android.media.MediaMuxer;
import android.net.Uri;
import android.provider.MediaStore;
import android.util.Log;

import androidx.core.content.FileProvider;

import java.io.File;
import java.io.FileOutputStream;
import java.io.IOException;
import java.io.InputStream;
import java.io.OutputStream;
import java.nio.ByteBuffer;
import java.util.ArrayList;
import java.util.UUID;
import java.util.concurrent.ExecutorService;
import java.util.concurrent.Executors;

/**
 * Photo / video capture through the system camera app (native CameraCapture_Android.cpp).
 *
 * Files live in {@code <cacheDir>/capture/} (FileProvider path "capture"). A video is remuxed without
 * re-encoding into a fresh mp4, so the camera app's metadata (location included) is not carried over; if
 * that fails nothing is handed back. State is static: the activity may be recreated while the camera is up.
 * Native method lives in libmain.so.
 */
final class PpCameraCapture {
    private static final String TAG = "pp-browser";

    static final int REQ_PERMISSIONS = 0x5050;
    static final int REQ_PHOTO = 0x5051;
    static final int REQ_VIDEO = 0x5052;

    // Shared with CameraCapture_Android.cpp.
    static final int OUTCOME_PICKED = 0;
    static final int OUTCOME_CANCELLED = 1;
    static final int OUTCOME_UNAVAILABLE = 2;
    static final int OUTCOME_UNPROCESSABLE = 3;
    static final int KIND_PHOTO = 0;
    static final int KIND_VIDEO = 1;

    private static final String ENV_FAIL_VIDEO_CLEAN = "PP_BROWSER_FAIL_VIDEO_CLEAN";
    private static final int MIN_SAMPLE_BUFFER = 4 * 1024 * 1024;

    private static final Object LOCK = new Object();
    private static boolean sActive;
    private static int sKind;
    private static int sMaxSeconds;
    private static long sMaxBytes;
    private static File sOutput;

    /** Remux, copies and cleanup run here, one at a time, never on the main thread. */
    private static final ExecutorService WORKER = Executors.newSingleThreadExecutor(r -> {
        final Thread t = new Thread(r, "pp-camera-capture");
        t.setDaemon(true);
        return t;
    });

    private PpCameraCapture() {}

    public static native void nativeCameraCaptureResult(int outcome, String path);

    static File captureDir(Context context) {
        return new File(context.getCacheDir(), "capture");
    }

    static void deliver(int outcome, String path) {
        try {
            nativeCameraCaptureResult(outcome, path);
        } catch (UnsatisfiedLinkError ignored) {
            // Native not loaded: nobody is waiting.
        }
    }

    /** Process-death leftovers count as cancelled: empty the folder unless a capture is in flight. */
    static void clearScratch(Context context) {
        final Context app = context.getApplicationContext();
        WORKER.execute(() -> {
            synchronized (LOCK) {
                if (sActive) {
                    return;
                }
            }
            deleteChildren(captureDir(app));
        });
    }

    /** UI thread. Starts the flow: permissions first, then the camera intent. */
    static void start(Activity activity, int kind, int maxSeconds, long maxBytes) {
        File stale = null;
        synchronized (LOCK) {
            if (sActive) {
                // The native side allows one capture at a time, so this is a leftover whose result never came.
                stale = sOutput;
            }
            sActive = true;
            sKind = kind;
            sMaxSeconds = maxSeconds;
            sMaxBytes = maxBytes;
            sOutput = null;
        }
        if (stale != null) {
            stale.delete();
        }
        final ArrayList<String> missing = new ArrayList<>();
        addIfMissing(activity, android.Manifest.permission.CAMERA, missing);
        if (kind == KIND_VIDEO) {
            addIfMissing(activity, android.Manifest.permission.RECORD_AUDIO, missing);
        }
        if (missing.isEmpty()) {
            launch(activity);
        } else {
            activity.requestPermissions(missing.toArray(new String[0]), REQ_PERMISSIONS);
        }
    }

    private static void addIfMissing(Activity activity, String permission, ArrayList<String> missing) {
        if (activity.checkSelfPermission(permission) != PackageManager.PERMISSION_GRANTED) {
            missing.add(permission);
        }
    }

    /** True when the request code is ours (then the caller must not pass it to super). */
    static boolean onPermissionsResult(Activity activity, int requestCode, int[] grantResults) {
        if (requestCode != REQ_PERMISSIONS) {
            return false;
        }
        synchronized (LOCK) {
            if (!sActive) {
                return true;
            }
        }
        boolean granted = grantResults != null && grantResults.length > 0;
        if (granted) {
            for (int result : grantResults) {
                if (result != PackageManager.PERMISSION_GRANTED) {
                    granted = false;
                }
            }
        }
        if (granted) {
            launch(activity);
        } else {
            finish(OUTCOME_UNAVAILABLE, null);
        }
        return true;
    }

    private static void launch(Activity activity) {
        final int kind;
        final int maxSeconds;
        final long maxBytes;
        synchronized (LOCK) {
            kind = sKind;
            maxSeconds = sMaxSeconds;
            maxBytes = sMaxBytes;
        }
        File out = null;
        try {
            final File dir = captureDir(activity);
            if (!dir.isDirectory() && !dir.mkdirs() && !dir.isDirectory()) {
                throw new IOException("cannot create " + dir);
            }
            out = new File(dir, UUID.randomUUID() + (kind == KIND_VIDEO ? ".mp4" : ".jpg"));
            final Uri uri = FileProvider.getUriForFile(activity, activity.getPackageName() + ".capture", out);
            final Intent intent = new Intent(kind == KIND_VIDEO ? MediaStore.ACTION_VIDEO_CAPTURE
                                                                : MediaStore.ACTION_IMAGE_CAPTURE);
            intent.putExtra(MediaStore.EXTRA_OUTPUT, uri);
            intent.setClipData(ClipData.newRawUri("", uri));
            intent.addFlags(Intent.FLAG_GRANT_WRITE_URI_PERMISSION | Intent.FLAG_GRANT_READ_URI_PERMISSION);
            if (kind == KIND_VIDEO) {
                intent.putExtra(MediaStore.EXTRA_DURATION_LIMIT, maxSeconds);
                intent.putExtra(MediaStore.EXTRA_SIZE_LIMIT, maxBytes);
                intent.putExtra(MediaStore.EXTRA_VIDEO_QUALITY, 0);
            }
            synchronized (LOCK) {
                sOutput = out;
            }
            activity.startActivityForResult(intent, kind == KIND_VIDEO ? REQ_VIDEO : REQ_PHOTO);
        } catch (android.content.ActivityNotFoundException | SecurityException | IllegalArgumentException
                 | IOException e) {
            Log.w(TAG, "Camera capture could not start: " + e);
            if (out != null) {
                out.delete();
            }
            finish(OUTCOME_UNAVAILABLE, null);
        }
    }

    /** True when the request code is ours. The work happens off the main thread. */
    static boolean onActivityResult(Activity activity, int requestCode, int resultCode, Intent data) {
        if (requestCode != REQ_PHOTO && requestCode != REQ_VIDEO) {
            return false;
        }
        final File out;
        synchronized (LOCK) {
            if (!sActive) {
                return true;
            }
            out = sOutput;
        }
        final Context app = activity.getApplicationContext();
        final Uri returned = data != null ? data.getData() : null;
        final boolean video = requestCode == REQ_VIDEO;
        WORKER.execute(() -> {
            try {
                process(app, video, resultCode, returned, out);
            } catch (RuntimeException e) {
                Log.w(TAG, "Camera capture result failed: " + e);
                if (out != null) {
                    out.delete();
                }
                finish(OUTCOME_UNAVAILABLE, null);
            }
        });
        return true;
    }

    private static void process(Context app, boolean video, int resultCode, Uri returned, File out) {
        if (out == null) {
            finish(OUTCOME_UNAVAILABLE, null);
            return;
        }
        if (resultCode != Activity.RESULT_OK) {
            out.delete();
            finish(OUTCOME_CANCELLED, null);
            return;
        }
        if (!out.isFile() || out.length() == 0) {
            out.delete();
            if (returned == null || !copyUri(app.getContentResolver(), returned, out)) {
                out.delete();
                finish(OUTCOME_UNAVAILABLE, null);
                return;
            }
        }
        if (!video) {
            finish(OUTCOME_PICKED, out.getAbsolutePath());
            return;
        }
        final File clean = new File(out.getParentFile(), stripExtension(out.getName()) + "-clean.mp4");
        try {
            if ("1".equals(System.getenv(ENV_FAIL_VIDEO_CLEAN))) {
                throw new IOException(ENV_FAIL_VIDEO_CLEAN + " is set");
            }
            remux(out, clean);
        } catch (Exception e) {
            // Never hand back the original: it still carries the camera app's metadata.
            Log.w(TAG, "Video metadata strip failed: " + e);
            out.delete();
            clean.delete();
            finish(OUTCOME_UNPROCESSABLE, null);
            return;
        }
        out.delete();
        finish(OUTCOME_PICKED, clean.getAbsolutePath());
    }

    private static void finish(int outcome, String path) {
        synchronized (LOCK) {
            sActive = false;
            sOutput = null;
        }
        deliver(outcome, path);
    }

    private static boolean copyUri(ContentResolver resolver, Uri uri, File dest) {
        try (InputStream in = resolver.openInputStream(uri); OutputStream out = new FileOutputStream(dest)) {
            if (in == null) {
                return false;
            }
            final byte[] buf = new byte[64 * 1024];
            int n;
            while ((n = in.read(buf)) > 0) {
                out.write(buf, 0, n);
            }
        } catch (IOException | SecurityException e) {
            Log.w(TAG, "Camera result copy failed: " + e);
            return false;
        }
        return dest.length() > 0;
    }

    /** Copies the audio / video samples as they are into a new mp4: no re-encode, no udta, no location. */
    private static void remux(File src, File dst) throws IOException {
        final MediaExtractor extractor = new MediaExtractor();
        MediaMuxer muxer = null;
        try {
            extractor.setDataSource(src.getAbsolutePath());
            final int rotation = readRotation(src);
            muxer = new MediaMuxer(dst.getAbsolutePath(), MediaMuxer.OutputFormat.MUXER_OUTPUT_MPEG_4);
            final int count = extractor.getTrackCount();
            final int[] trackMap = new int[count];
            int bufferSize = MIN_SAMPLE_BUFFER;
            int copied = 0;
            for (int i = 0; i < count; i++) {
                trackMap[i] = -1;
                final MediaFormat format = extractor.getTrackFormat(i);
                final String mime = format.getString(MediaFormat.KEY_MIME);
                if (mime == null || !(mime.startsWith("video/") || mime.startsWith("audio/"))) {
                    continue;
                }
                trackMap[i] = muxer.addTrack(format);
                extractor.selectTrack(i);
                copied++;
                if (format.containsKey(MediaFormat.KEY_MAX_INPUT_SIZE)) {
                    bufferSize = Math.max(bufferSize, format.getInteger(MediaFormat.KEY_MAX_INPUT_SIZE));
                }
            }
            if (copied == 0) {
                throw new IOException("no audio / video track");
            }
            muxer.setOrientationHint(rotation);
            muxer.start();
            final ByteBuffer buffer = ByteBuffer.allocate(bufferSize);
            final android.media.MediaCodec.BufferInfo info = new android.media.MediaCodec.BufferInfo();
            while (true) {
                buffer.clear();
                final int size = extractor.readSampleData(buffer, 0);
                if (size < 0) {
                    break;
                }
                final int flags = (extractor.getSampleFlags() & MediaExtractor.SAMPLE_FLAG_SYNC) != 0
                        ? android.media.MediaCodec.BUFFER_FLAG_KEY_FRAME : 0;
                info.set(0, size, extractor.getSampleTime(), flags);
                muxer.writeSampleData(trackMap[extractor.getSampleTrackIndex()], buffer, info);
                extractor.advance();
            }
            muxer.stop();
        } finally {
            extractor.release();
            if (muxer != null) {
                try {
                    muxer.release();
                } catch (RuntimeException ignored) {
                    // Already failing or stopped; the caller deletes the output.
                }
            }
        }
    }

    private static int readRotation(File src) throws IOException {
        final MediaMetadataRetriever retriever = new MediaMetadataRetriever();
        try {
            retriever.setDataSource(src.getAbsolutePath());
            final String value = retriever.extractMetadata(MediaMetadataRetriever.METADATA_KEY_VIDEO_ROTATION);
            final int degrees = value == null ? 0 : Integer.parseInt(value.trim());
            return degrees == 90 || degrees == 180 || degrees == 270 ? degrees : 0;
        } catch (RuntimeException e) {
            // Losing the rotation would send a sideways video: treat it as a failed strip.
            throw new IOException("cannot read the video rotation", e);
        } finally {
            retriever.release();
        }
    }

    private static String stripExtension(String name) {
        final int dot = name.lastIndexOf('.');
        return dot > 0 ? name.substring(0, dot) : name;
    }

    private static void deleteChildren(File dir) {
        final File[] files = dir.listFiles();
        if (files == null) {
            return;
        }
        for (File f : files) {
            if (f.isDirectory()) {
                deleteChildren(f);
            }
            f.delete();
        }
    }
}
