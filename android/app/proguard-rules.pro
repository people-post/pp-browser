# Keep SDL JNI entry points.
-keep class org.libsdl.app.** { *; }

# Keep MainActivity methods called by name from native JNI helpers.
-keepclassmembers class dev.pp_browser.app.MainActivity {
    public void setAppAppearance(java.lang.String);
    public void showLocalNotification(java.lang.String, java.lang.String, java.lang.String);
    public void clearLocalNotification(java.lang.String);
    public java.lang.String getStableDeviceId();
    public boolean isActiveNetworkWifi();
    public void startNetworkMonitor();
    public void stopNetworkMonitor();
}

# Native NetworkMonitor calls back through this JNI entry point.
-keepclassmembers class dev.pp_browser.app.PpNetworkMonitor {
    static native void nativeOnNetworkState(boolean, int, boolean, java.lang.String);
}
