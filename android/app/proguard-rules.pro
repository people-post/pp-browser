# Keep SDL JNI entry points.
-keep class org.libsdl.app.** { *; }

# Keep MainActivity methods called by name from native JNI helpers.
-keepclassmembers class dev.pp_browser.app.MainActivity {
    public void setAppAppearance(java.lang.String);
    public void showLocalNotification(java.lang.String, java.lang.String, java.lang.String);
    public void clearLocalNotification(java.lang.String);
    public java.lang.String getStableDeviceId();
}
