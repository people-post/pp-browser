#pragma once

#include <functional>
#include <string>

namespace pbr {

/** Open a local file with the OS default handler (R012). */
bool PlatformOpenFile(const std::string& path);

/**
 * iOS shows a file inside the app (QuickLook), so a sound or video there plays through the app's own audio session,
 * which a call or a voice recording may have left on the earpiece (or which the silent switch mutes). `begin` runs
 * when such a preview opens and `end` when it closes; the app sets them once. Elsewhere the system app plays it and
 * these are never called.
 */
void SetPlatformOpenFileSoundHooks(std::function<void()> begin, std::function<void()> end);

/**
 * Removes the copies PlatformOpenFile handed to other apps outside the attachment view cache (Android: the links in
 * cache/open/ behind the FileProvider). Run with every view-cache wipe. Elsewhere nothing is copied: does nothing.
 */
void PlatformForgetOpenedFiles();

} // namespace pbr
