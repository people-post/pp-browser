package dev.pp_browser.app;

import android.content.Intent;
import android.net.Uri;

/**
 * Hands the app the invite links it registered for (QR codes, tapped links) — native IncomingLink.
 * Native methods live in libmain.so.
 */
public final class PpLinkBridge {
    /** The scheme of a contact invite (lib/messaging/ContactLocator.h kContactLocatorPrefix). */
    static final String CONTACT_INVITE_SCHEME = "pp-browser-contact-v1";

    private PpLinkBridge() {}

    public static native void nativeOnOpenLink(String link);

    /** An activity intent carrying an invite link: hand it over (the app shows it when it can). */
    public static void deliver(Intent intent) {
        if (intent == null || !Intent.ACTION_VIEW.equals(intent.getAction())) {
            return;
        }
        final Uri data = intent.getData();
        if (data == null || !CONTACT_INVITE_SCHEME.equals(data.getScheme())) {
            return;
        }
        try {
            nativeOnOpenLink(data.toString());
        } catch (UnsatisfiedLinkError ignored) {
            // Native not loaded: nothing to hand it to.
        }
    }
}
