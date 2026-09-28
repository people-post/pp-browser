package dev.pp_browser.app;

import android.content.Context;
import android.net.ConnectivityManager;
import android.net.LinkAddress;
import android.net.LinkProperties;
import android.net.Network;
import android.net.NetworkCapabilities;
import android.text.TextUtils;

import java.util.ArrayList;
import java.util.Collections;
import java.util.List;

/**
 * Default-network watcher for native NetworkMonitor (call-path-resilience k5). Every callback reports
 * the full current state; native keeps the last one and reacts only to material changes.
 */
final class PpNetworkMonitor extends ConnectivityManager.NetworkCallback {
    /** Must match NetworkMonitor_Android.cpp. */
    static final int TRANSPORT_UNKNOWN = 0;
    static final int TRANSPORT_WIFI = 1;
    static final int TRANSPORT_CELLULAR = 2;
    static final int TRANSPORT_OTHER = 3;

    static native void nativeOnNetworkState(boolean online, int transport, boolean expensive, String fingerprint);

    private final ConnectivityManager mConnectivity;
    private boolean mRegistered;
    /** The default network last reported; a loss of any other one (the previous default after a switch) is ignored. */
    private Network mCurrent;

    PpNetworkMonitor(Context context) {
        mConnectivity = context.getSystemService(ConnectivityManager.class);
    }

    synchronized void start() {
        if (mRegistered || mConnectivity == null) {
            return;
        }
        mConnectivity.registerDefaultNetworkCallback(this);  // reports the current network at once
        mRegistered = true;
    }

    synchronized void stop() {
        if (!mRegistered) {
            return;
        }
        mRegistered = false;
        try {
            mConnectivity.unregisterNetworkCallback(this);
        } catch (IllegalArgumentException ignored) {
            // Already unregistered.
        }
    }

    @Override
    public void onAvailable(Network network) {
        report(network, mConnectivity.getNetworkCapabilities(network), mConnectivity.getLinkProperties(network));
    }

    @Override
    public void onCapabilitiesChanged(Network network, NetworkCapabilities caps) {
        report(network, caps, mConnectivity.getLinkProperties(network));
    }

    @Override
    public void onLinkPropertiesChanged(Network network, LinkProperties props) {
        report(network, mConnectivity.getNetworkCapabilities(network), props);
    }

    @Override
    public void onLost(Network network) {
        synchronized (this) {
            if (mCurrent != null && !mCurrent.equals(network)) {
                return;
            }
            mCurrent = null;
        }
        send(false, TRANSPORT_UNKNOWN, false, "");
    }

    private void report(Network network, NetworkCapabilities caps, LinkProperties props) {
        synchronized (this) {
            mCurrent = network;
        }
        int transport = TRANSPORT_UNKNOWN;
        boolean expensive = false;
        if (caps != null) {
            if (caps.hasTransport(NetworkCapabilities.TRANSPORT_WIFI)) {
                transport = TRANSPORT_WIFI;
            } else if (caps.hasTransport(NetworkCapabilities.TRANSPORT_CELLULAR)) {
                transport = TRANSPORT_CELLULAR;
            } else {
                transport = TRANSPORT_OTHER;
            }
            expensive = !caps.hasCapability(NetworkCapabilities.NET_CAPABILITY_NOT_METERED);
        }
        List<String> parts = new ArrayList<>();
        if (props != null) {
            String iface = props.getInterfaceName();
            for (LinkAddress address : props.getLinkAddresses()) {
                if (address.getAddress().isLinkLocalAddress()) {
                    continue;
                }
                parts.add(iface + "/" + address.getAddress().getHostAddress());
            }
        }
        Collections.sort(parts);
        String fingerprint = network + "|" + TextUtils.join(",", parts);
        send(true, transport, expensive, fingerprint);
    }

    private static void send(boolean online, int transport, boolean expensive, String fingerprint) {
        try {
            nativeOnNetworkState(online, transport, expensive, fingerprint);
        } catch (UnsatisfiedLinkError ignored) {
            // Native not loaded yet.
        }
    }
}
