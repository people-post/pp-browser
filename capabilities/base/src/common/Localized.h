#pragma once

#include <functional>
#include <map>
#include <string>
#include <string_view>

namespace pbr {

// User-facing text by catalog key (capability-repos CR015): capabilities name keys, the app's catalogs hold the
// text. The app registers its translator at start (foundation/i18n InstallAppTranslator); with none registered, a
// key reads as itself.

using TextArgs = std::map<std::string, std::string>;
using Translator = std::function<std::string(std::string_view key, const TextArgs& args)>;

/**
 * The running app's name ("PP"). The app's translator answers it, and fills `{product}` in every text when the
 * caller passes none: capabilities never name the product themselves (CR023 step 3).
 */
inline constexpr char kProductNameKey[] = "product.name";

/** The tip for a network that blocks peer-to-peer media: the app picks its platform's text. */
inline constexpr char kPlatformNetworkHintKey[] = "hints.network.platform";

/** Installs the app's translator (any thread reads it after). Empty: keys read as themselves. */
void SetTranslator(Translator translator);
/** The text for `key` in the user's language, `args` filled in. */
std::string Localized(std::string_view key, const TextArgs& args = {});

} // namespace pbr
