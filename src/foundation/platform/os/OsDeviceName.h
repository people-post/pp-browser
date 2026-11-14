#pragma once

#include <string>

namespace pbr::os {

/**
 * The name the system gives this device (iOS: "iPhone"; macOS: the computer name; Android: the device name or
 * maker and model; Windows and Linux: the host name), already through NormalizeDeviceName. Empty when it cannot
 * be read. Needs no permission. Read it once at startup on the main thread and keep the answer: the Android
 * reader calls into Java and must not run on a thread the JVM does not know.
 */
std::string SystemDeviceName();

} // namespace pbr::os
