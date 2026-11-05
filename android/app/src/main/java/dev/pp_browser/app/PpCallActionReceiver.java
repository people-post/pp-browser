package dev.pp_browser.app;

import android.content.BroadcastReceiver;
import android.content.Context;
import android.content.Intent;

/** Decline on an incoming-call notification, Hang up on the ongoing one: no need to open (or unlock) the phone. */
public final class PpCallActionReceiver extends BroadcastReceiver {
    @Override
    public void onReceive(Context context, Intent intent) {
        if (intent != null && PpCallService.ACTION_HANG_UP.equals(intent.getAction())) {
            PpCallService.onHangUp();
            return;
        }
        if (intent == null || !PpCallNotifications.ACTION_DECLINE.equals(intent.getAction())) {
            return;
        }
        PpCallNotifications.init(context);
        PpCallNotifications.onDecline(intent.getStringExtra(PpCallNotifications.EXTRA_CALL_ID));
    }
}
