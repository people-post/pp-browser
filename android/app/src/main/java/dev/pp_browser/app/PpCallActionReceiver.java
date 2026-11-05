package dev.pp_browser.app;

import android.content.BroadcastReceiver;
import android.content.Context;
import android.content.Intent;

/** Decline on an incoming-call notification: no need to open (or unlock) the phone for it. */
public final class PpCallActionReceiver extends BroadcastReceiver {
    @Override
    public void onReceive(Context context, Intent intent) {
        if (intent == null || !PpCallNotifications.ACTION_DECLINE.equals(intent.getAction())) {
            return;
        }
        PpCallNotifications.init(context);
        PpCallNotifications.onDecline(intent.getStringExtra(PpCallNotifications.EXTRA_CALL_ID));
    }
}
