#pragma once

#include <cstddef>
#include <iosfwd>
#include <string>
#include <string_view>
#include <utility>

namespace pbr {

/**
 * A secret held as text — a PIN, a recovery phrase, a private key or seed (base64), an API key. It cannot reach
 * a log, a toast or any other text by accident: there is no `operator<<` (streaming one into a logger does not
 * compile), no conversion to a string and no `c_str()`. `Reveal()` is the one way to the text, for the code that
 * really uses the secret (a KDF, a cipher, a codec, an HTTP header); `rg 'Reveal\(\)'` lists every such place.
 *
 * The held text is wiped when the value is destroyed or replaced (best effort: what a caller copies out of
 * `Reveal()` is the caller's to keep short-lived).
 */
class SecretText {
public:
  // Implicit from text: a string may become a secret freely; only the way back out is guarded.
  SecretText() = default;
  SecretText(std::string text) : text_(std::move(text)) {}  // NOLINT(google-explicit-constructor)
  SecretText(std::string_view text) : text_(text) {}        // NOLINT(google-explicit-constructor)
  SecretText(const char* text) : text_(text ? text : "") {}  // NOLINT(google-explicit-constructor)

  SecretText(const SecretText& other) = default;
  SecretText(SecretText&& other) noexcept : text_(std::move(other.text_)) { other.Wipe(); }
  SecretText& operator=(const SecretText& other) {
    if (this != &other) {
      Wipe();
      text_ = other.text_;
    }
    return *this;
  }
  SecretText& operator=(SecretText&& other) noexcept {
    if (this != &other) {
      Wipe();
      text_ = std::move(other.text_);
      other.Wipe();
    }
    return *this;
  }
  ~SecretText() { Wipe(); }

  /** The secret itself — only where it is used, never where it could be shown or logged. */
  const std::string& Reveal() const { return text_; }

  bool empty() const { return text_.empty(); }
  std::size_t size() const { return text_.size(); }
  void clear() { Wipe(); }

  /** Constant time for equal lengths (a typed PIN against its confirmation). */
  friend bool operator==(const SecretText& a, const SecretText& b) { return EqualTexts(a.text_, b.text_); }

  friend std::ostream& operator<<(std::ostream&, const SecretText&) = delete;

private:
  static bool EqualTexts(const std::string& a, const std::string& b) {
    if (a.size() != b.size()) {
      return false;
    }
    unsigned char diff = 0;
    for (std::size_t i = 0; i < a.size(); ++i) {
      diff = static_cast<unsigned char>(diff | (static_cast<unsigned char>(a[i]) ^ static_cast<unsigned char>(b[i])));
    }
    return diff == 0;
  }

  void Wipe() noexcept {
    // volatile: the compiler may not drop stores to memory that is about to be freed.
    volatile char* bytes = text_.data();
    for (std::size_t i = 0; i < text_.size(); ++i) {
      bytes[i] = 0;
    }
    text_.clear();
  }

  std::string text_;
};

} // namespace pbr
