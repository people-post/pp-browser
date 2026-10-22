#pragma once

namespace pbr {

/**
 * P011 — keeps the app alive in the background while it stays connected (opt-in): on Android a
 * foreground service with its ongoing notification, started / stopped as AppLifecycle's
 * stay-connected setting changes. Elsewhere nothing: a desktop app keeps working in the background
 * anyway, and iOS has no such mode.
 */
void InstallStayConnectedService();

} // namespace pbr
