#pragma once

#include <string>
#include "common/PbrCompat.h"

namespace pbr {

/**
 * CallKit + PushKit (P013; builds with PP_BROWSER_IOS_CALLKIT). Called from
 * didFinishLaunching: a VoIP push that launched the app is delivered only once the push registry
 * exists, and must put a call on the CallKit screen before its handler returns.
 */
void InstallIosCallSystemUi();

/** The PushKit (VoIP) token, hex; empty until PushKit gave one. */
std::string IosCachedVoipToken();

} // namespace pbr
