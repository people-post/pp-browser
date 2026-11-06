#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>

namespace pbr {

/** The container a sound file really is, by its first bytes (never by its name). */
enum class AudioContainer { Unknown, Mp3, Mp4, Wav, Flac, Ogg, Aiff };

/** Reads the first 16 bytes (or fewer: `n`). Unknown for a short or unrecognised buffer. */
AudioContainer SniffAudioContainer(const uint8_t* bytes, size_t n);

/** SniffAudioContainer on the head of the file; Unknown when it cannot be read. */
AudioContainer SniffAudioFile(const std::filesystem::path& path);

/** The extension that names the container for a system that types by extension ("mp3", "m4a", "wav", "flac", "ogg",
 *  "aiff"); nullptr for Unknown. */
const char* AudioContainerExtension(AudioContainer container);

} // namespace pbr
