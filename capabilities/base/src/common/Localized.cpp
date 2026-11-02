#include "common/Localized.h"

#include <memory>
#include <mutex>
#include <utility>

namespace pbr {

namespace {

std::mutex& TranslatorMutex() {
  static std::mutex mutex;
  return mutex;
}

std::shared_ptr<const Translator>& Installed() {
  static std::shared_ptr<const Translator> translator;
  return translator;
}

} // namespace

void SetTranslator(Translator translator) {
  auto next = translator ? std::make_shared<const Translator>(std::move(translator)) : nullptr;
  std::lock_guard lock(TranslatorMutex());
  Installed() = std::move(next);
}

std::string Localized(std::string_view key, const TextArgs& args) {
  std::shared_ptr<const Translator> translator;
  {
    std::lock_guard lock(TranslatorMutex());
    translator = Installed();
  }
  return translator ? (*translator)(key, args) : std::string(key);
}

} // namespace pbr
