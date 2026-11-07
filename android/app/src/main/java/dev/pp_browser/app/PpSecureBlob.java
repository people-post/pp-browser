package dev.pp_browser.app;

import android.app.KeyguardManager;
import android.content.Context;
import android.os.Build;
import android.security.keystore.KeyGenParameterSpec;
import android.security.keystore.KeyPermanentlyInvalidatedException;
import android.security.keystore.KeyProperties;
import android.security.keystore.UserNotAuthenticatedException;

import androidx.annotation.ChecksSdkIntAtLeast;
import androidx.annotation.RequiresApi;

import java.io.File;
import java.io.FileInputStream;
import java.io.FileOutputStream;
import java.io.IOException;
import java.nio.charset.StandardCharsets;
import java.security.Key;
import java.security.KeyStore;
import java.security.MessageDigest;
import java.security.UnrecoverableKeyException;
import java.util.Arrays;
import java.util.Locale;

import javax.crypto.AEADBadTagException;
import javax.crypto.Cipher;
import javax.crypto.KeyGenerator;
import javax.crypto.SecretKey;
import javax.crypto.spec.GCMParameterSpec;

/**
 * Native OsSecureBlob_Android.cpp (through MainActivity's statics): one small blob per name, the Android stand-in for
 * the iOS Keychain item. The blob is sealed with AES-256-GCM under an AndroidKeyStore key (the key never leaves the
 * Keystore) and the ciphertext is a file in {@code noBackupFilesDir/secure_blob/<hex sha256 of name>}: never in a
 * backup or a device transfer. Unlike the Keychain an uninstall deletes both (the Keystore keys go with the app).
 *
 * <p>Two keys, one per access level: {@code WHEN_UNLOCKED} is generated with {@code setUnlockedDeviceRequired} (the
 * PIN grace key: the Keystore refuses it while the lock screen is up), {@code AFTER_FIRST_UNLOCK} without (usable once
 * the credential-encrypted storage it lives in is open). API 28 is the first with that flag; below it nothing is
 * stored and native behaves like the Default stub.
 *
 * <p>File: level (1 byte) | IV (12) | ciphertext + GCM tag; AAD = level | name, so a file renamed to another name, or
 * its level byte changed, does not open.
 */
final class PpSecureBlob {
    /** Lookup's answer, byte 0 (native OsSecureBlob_Android.cpp reads the same values). */
    static final byte FOUND = 0;
    static final byte MISSING = 1;
    static final byte UNAVAILABLE = 2;
    static final byte ERROR = 3;
    /** Unavailable, byte 1: the lock screen is up (asked before the key is touched). */
    static final byte CODE_DEVICE_LOCKED = 1;
    /** Unavailable, byte 1: the Keystore refused the key as locked / not authenticated. */
    static final byte CODE_KEYSTORE_LOCKED = 2;

    private static final String KEYSTORE = "AndroidKeyStore";
    private static final String DIR = "secure_blob";
    private static final byte LEVEL_AFTER_FIRST_UNLOCK = 1;
    private static final byte LEVEL_WHEN_UNLOCKED = 2;
    private static final String ALIAS_AFTER_FIRST_UNLOCK = "pp_secure_blob_after_first_unlock";
    private static final String ALIAS_WHEN_UNLOCKED = "pp_secure_blob_when_unlocked";
    private static final String TRANSFORMATION = "AES/GCM/NoPadding";
    private static final int IV_BYTES = 12;
    private static final int TAG_BITS = 128;

    private PpSecureBlob() {}

    @ChecksSdkIntAtLeast(api = Build.VERSION_CODES.P)
    static boolean supported() {
        return Build.VERSION.SDK_INT >= Build.VERSION_CODES.P;
    }

    /** The lock screen is up. False when it cannot be asked (the Keystore still refuses a WHEN_UNLOCKED key then). */
    static boolean deviceLocked(Context context) {
        if (context == null) {
            return false;
        }
        final KeyguardManager keyguard = context.getSystemService(KeyguardManager.class);
        return keyguard != null && keyguard.isDeviceLocked();
    }

    /**
     * Replaces the blob (whatever level it was stored under) with {@code bytes} sealed under the level's key: the new
     * file is complete before it takes the old one's place. Null on success, else what failed. {@code bytes} is wiped.
     */
    static synchronized String write(Context context, byte[] name, byte[] bytes, boolean whenUnlocked) {
        try {
            if (!supported()) {
                return "Secure blob store is not supported on this platform";
            }
            if (context == null || name == null || bytes == null) {
                return "no activity";
            }
            final byte level = whenUnlocked ? LEVEL_WHEN_UNLOCKED : LEVEL_AFTER_FIRST_UNLOCK;
            final File file = fileFor(context, name);
            final File dir = file.getParentFile();
            if (dir == null || (!dir.isDirectory() && !dir.mkdirs())) {
                return "cannot create " + DIR;
            }
            final byte[] sealed = seal(level, name, bytes);
            final File tmp = new File(dir, file.getName() + ".tmp");
            try (FileOutputStream out = new FileOutputStream(tmp)) {
                out.write(sealed);
                out.getFD().sync();
            }
            if (!tmp.renameTo(file)) {
                tmp.delete();
                return "rename failed";
            }
            return null;
        } catch (Exception e) {
            return "write failed: " + e;
        } finally {
            if (bytes != null) {
                Arrays.fill(bytes, (byte) 0);
            }
        }
    }

    /**
     * Byte 0 FOUND (then the blob), MISSING, UNAVAILABLE (then a CODE_ byte) or ERROR (then a UTF-8 message). Missing is
     * only said when the blob can never be opened again (no file, no key, the key invalidated, a bad tag): the caller
     * deletes on Missing. Locked, or anything not understood, keeps it.
     */
    static synchronized byte[] lookup(Context context, byte[] name) {
        if (!supported()) {
            return new byte[] {MISSING};
        }
        if (context == null || name == null) {
            return error("no activity");
        }
        if (deviceLocked(context)) {
            return new byte[] {UNAVAILABLE, CODE_DEVICE_LOCKED};
        }
        final File file = fileFor(context, name);
        if (!file.isFile()) {
            return new byte[] {MISSING};
        }
        final byte[] sealed;
        try {
            sealed = readAll(file);
        } catch (IOException e) {
            return error("read failed: " + e);
        }
        if (sealed.length < 1 + IV_BYTES + TAG_BITS / 8
                || (sealed[0] != LEVEL_AFTER_FIRST_UNLOCK && sealed[0] != LEVEL_WHEN_UNLOCKED)) {
            return new byte[] {MISSING};  // not a file this class wrote: it will never open
        }
        byte[] plain = null;
        try {
            final SecretKey key = keyFor(sealed[0], false);
            if (key == null) {
                return new byte[] {MISSING};
            }
            final Cipher cipher = Cipher.getInstance(TRANSFORMATION);
            cipher.init(Cipher.DECRYPT_MODE, key, new GCMParameterSpec(TAG_BITS, sealed, 1, IV_BYTES));
            cipher.updateAAD(aad(sealed[0], name));
            plain = cipher.doFinal(sealed, 1 + IV_BYTES, sealed.length - 1 - IV_BYTES);
            final byte[] out = new byte[1 + plain.length];
            out[0] = FOUND;
            System.arraycopy(plain, 0, out, 1, plain.length);
            return out;
        } catch (Exception e) {
            // Locked first: a refusal that also looks like a lost key must not delete the blob.
            if (isLocked(e)) {
                return new byte[] {UNAVAILABLE, CODE_KEYSTORE_LOCKED};
            }
            if (isGone(e)) {
                return new byte[] {MISSING};
            }
            return error("lookup failed: " + e);
        } finally {
            if (plain != null) {
                Arrays.fill(plain, (byte) 0);
            }
        }
    }

    /** Null on success (also when nothing is stored), else what failed. The keys stay: other names use them. */
    static synchronized String delete(Context context, byte[] name) {
        if (!supported()) {
            return null;
        }
        if (context == null || name == null) {
            return "no activity";
        }
        final File file = fileFor(context, name);
        if (file.exists() && !file.delete()) {
            return "delete failed";
        }
        return null;
    }

    @RequiresApi(Build.VERSION_CODES.P)
    private static byte[] seal(byte level, byte[] name, byte[] bytes) throws Exception {
        final Cipher cipher = Cipher.getInstance(TRANSFORMATION);
        try {
            cipher.init(Cipher.ENCRYPT_MODE, keyFor(level, true));
        } catch (KeyPermanentlyInvalidatedException e) {
            // That key is gone for good (what it sealed already reads as Missing): a fresh one, once.
            deleteKey(level);
            cipher.init(Cipher.ENCRYPT_MODE, keyFor(level, true));
        }
        cipher.updateAAD(aad(level, name));
        final byte[] iv = cipher.getIV();
        if (iv == null || iv.length != IV_BYTES) {
            throw new IllegalStateException("unexpected GCM IV length");
        }
        final byte[] sealed = cipher.doFinal(bytes);
        final byte[] out = new byte[1 + IV_BYTES + sealed.length];
        out[0] = level;
        System.arraycopy(iv, 0, out, 1, IV_BYTES);
        System.arraycopy(sealed, 0, out, 1 + IV_BYTES, sealed.length);
        return out;
    }

    /** The level's key; with {@code create}, one is made when there is none (or it cannot be recovered). */
    @RequiresApi(Build.VERSION_CODES.P)
    private static SecretKey keyFor(byte level, boolean create) throws Exception {
        final KeyStore store = KeyStore.getInstance(KEYSTORE);
        store.load(null);
        final String alias = alias(level);
        Key key;
        try {
            key = store.getKey(alias, null);
        } catch (UnrecoverableKeyException e) {
            if (!create) {
                throw e;
            }
            store.deleteEntry(alias);
            key = null;
        }
        if (key instanceof SecretKey) {
            return (SecretKey) key;
        }
        if (!create) {
            return null;
        }
        final KeyGenParameterSpec.Builder spec = new KeyGenParameterSpec.Builder(
                alias, KeyProperties.PURPOSE_ENCRYPT | KeyProperties.PURPOSE_DECRYPT)
                .setBlockModes(KeyProperties.BLOCK_MODE_GCM)
                .setEncryptionPaddings(KeyProperties.ENCRYPTION_PADDING_NONE)
                .setKeySize(256);
        if (level == LEVEL_WHEN_UNLOCKED) {
            spec.setUnlockedDeviceRequired(true);
        }
        final KeyGenerator generator = KeyGenerator.getInstance(KeyProperties.KEY_ALGORITHM_AES, KEYSTORE);
        generator.init(spec.build());
        return generator.generateKey();
    }

    private static void deleteKey(byte level) throws Exception {
        final KeyStore store = KeyStore.getInstance(KEYSTORE);
        store.load(null);
        store.deleteEntry(alias(level));
    }

    private static String alias(byte level) {
        return level == LEVEL_WHEN_UNLOCKED ? ALIAS_WHEN_UNLOCKED : ALIAS_AFTER_FIRST_UNLOCK;
    }

    private static byte[] aad(byte level, byte[] name) {
        final byte[] out = new byte[1 + name.length];
        out[0] = level;
        System.arraycopy(name, 0, out, 1, name.length);
        return out;
    }

    /** UserNotAuthenticatedException, or a Keystore error that names the lock (the wording differs by release). */
    private static boolean isLocked(Throwable e) {
        for (Throwable t = e; t != null; t = t.getCause()) {
            if (t instanceof UserNotAuthenticatedException) {
                return true;
            }
            final String message = t.getMessage();
            if (t.getClass().getSimpleName().equals("KeyStoreException") && message != null
                    && message.toLowerCase(Locale.ROOT).contains("lock")) {
                return true;
            }
        }
        return false;
    }

    /**
     * The blob can never open again. Not UnrecoverableKeyException: the Keystore also throws it for a busy or failing
     * backend (keystore2), and a Missing deletes the blob; such a read is an error, which keeps it.
     */
    private static boolean isGone(Throwable e) {
        for (Throwable t = e; t != null; t = t.getCause()) {
            if (t instanceof KeyPermanentlyInvalidatedException || t instanceof AEADBadTagException) {
                return true;
            }
        }
        return false;
    }

    private static File fileFor(Context context, byte[] name) {
        return new File(new File(context.getNoBackupFilesDir(), DIR), hexSha256(name));
    }

    private static String hexSha256(byte[] name) {
        try {
            final byte[] digest = MessageDigest.getInstance("SHA-256").digest(name);
            final StringBuilder hex = new StringBuilder(digest.length * 2);
            for (byte b : digest) {
                hex.append(String.format(Locale.ROOT, "%02x", b & 0xff));
            }
            return hex.toString();
        } catch (java.security.NoSuchAlgorithmException e) {
            throw new IllegalStateException(e);  // SHA-256 is always there
        }
    }

    private static byte[] readAll(File file) throws IOException {
        final long length = file.length();
        if (length > 64 * 1024) {
            throw new IOException("blob file too large");
        }
        final byte[] out = new byte[(int) length];
        try (FileInputStream in = new FileInputStream(file)) {
            int read = 0;
            while (read < out.length) {
                final int n = in.read(out, read, out.length - read);
                if (n < 0) {
                    throw new IOException("short read");
                }
                read += n;
            }
        }
        return out;
    }

    private static byte[] error(String message) {
        final byte[] text = message.getBytes(StandardCharsets.UTF_8);
        final byte[] out = new byte[1 + text.length];
        out[0] = ERROR;
        System.arraycopy(text, 0, out, 1, text.length);
        return out;
    }
}
