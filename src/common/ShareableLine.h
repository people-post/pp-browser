#pragma once

#include "common/Error.h"
#include "common/Logger.h"

#include <chrono>
#include <concepts>
#include <cstdint>
#include <initializer_list>
#include <string>
#include <string_view>
#include "common/PbrCompat.h"

namespace pbr {

/**
 * A word written in the source code: only a string literal (or another compile-time constant) converts,
 * never a runtime std::string or char*. Event names, keys and enum-like values of a shareable line are
 * these, so nothing typed, received or read from disk can reach a file that may leave the device.
 * Letters, digits and `_ . - : < >`; no spaces or `=`, at most 48 bytes.
 */
class ShareWord {
public:
  consteval ShareWord(const char* text) : text_(text) {  // NOLINT(google-explicit-constructor): the point
    std::size_t size = 0;
    for (const char* c = text; *c != '\0'; ++c, ++size) {
      const bool ok = (*c >= 'a' && *c <= 'z') || (*c >= 'A' && *c <= 'Z') || (*c >= '0' && *c <= '9') ||
                      *c == '_' || *c == '.' || *c == '-' || *c == ':' || *c == '<' || *c == '>';
      if (!ok) {
        throw "ShareWord: letters, digits and _ . - : < > only";  // a compile error: consteval
      }
    }
    if (size == 0 || size > 48) {
      throw "ShareWord: 1 to 48 bytes";
    }
  }
  constexpr std::string_view view() const { return text_; }
  constexpr const char* c_str() const { return text_; }

private:
  const char* text_;
};

/**
 * One `event=<name> k=v k=v…` line for a file that may leave the device (docs/contracts/DIAGNOSTICS.md).
 * The values it takes cannot carry content, names or raw ids: numbers, durations, booleans, words from the
 * source (ShareWord), a runtime string only when it is one of a fixed list (Pick), ids only as a short keyed
 * hash (Ref), errors only as category and code (Fail). There is deliberately no way to add free text — a
 * detail that needs one belongs in the developer log, which never leaves the device.
 *
 * Built by MetricsLine (common/Metrics.h) and DiagLine (common/Diag.h); each writes its own file.
 */
class ShareableLine {
public:
  ShareableLine& Add(ShareWord key, ShareWord value);
  template <std::integral T>
    requires(!std::same_as<T, bool>)
  ShareableLine& Add(ShareWord key, T value) {
    if constexpr (std::is_signed_v<T>) {
      return AddInt(key, static_cast<int64_t>(value));
    } else {
      return AddUint(key, static_cast<uint64_t>(value));
    }
  }
  /** 1 or 0. */
  template <std::same_as<bool> B>
  ShareableLine& Add(ShareWord key, B value) {
    return AddInt(key, value ? 1 : 0);
  }
  /** One decimal place (rates, seconds). */
  template <std::floating_point F>
  ShareableLine& Add(ShareWord key, F value) {
    return AddDouble(key, static_cast<double>(value));
  }
  /** Whole milliseconds. */
  template <class Rep, class Period>
  ShareableLine& Add(ShareWord key, std::chrono::duration<Rep, Period> value) {
    return AddInt(key, static_cast<int64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(value).count()));
  }

  /** `value` when it is one of `words`, else `other` (`-` when empty): a runtime string from a fixed vocabulary. */
  ShareableLine& Pick(ShareWord key, std::string_view value, std::initializer_list<ShareWord> words);

  /**
   * An id (account, device, peer, relay user, thread, message) as 8 hex of a keyed hash. The key is random per
   * process and never written anywhere, so lines of one run can be joined and nothing else: not the id, not
   * another run, not another device. A leading `account:` / `peer:` / `relay:` / `call:` is dropped first so
   * both spellings of one id give the same ref. Empty id: `-`.
   */
  ShareableLine& Ref(ShareWord key, std::string_view id);

  /** A random call id's first 8 hex: both sides of a call log the same value, so their lines can be joined. */
  ShareableLine& Call(ShareWord key, std::string_view call_id);

  /** An error as `category.code` (foundation/error/AppError) — never its message. */
  ShareableLine& Fail(ShareWord key, const Error& error);

  const std::string& str() const { return text_; }
  /** At INFO on the sink's channel. */
  void Emit() const;

protected:
  ShareableLine(logging::Logger& sink, ShareWord event);

private:
  ShareableLine& AddInt(ShareWord key, int64_t value);
  ShareableLine& AddUint(ShareWord key, uint64_t value);
  ShareableLine& AddDouble(ShareWord key, double value);
  ShareableLine& AddRaw(ShareWord key, std::string_view value);

  logging::Logger* sink_;
  std::string text_;
};

/** The first 8 hex of a random call id, without its `call:` scheme. */
std::string ShortCallId(std::string_view call_id);

/** Ref's value for `id` in this process (tests; the same function Ref uses). */
std::string ShareableRef(std::string_view id);

} // namespace pbr
