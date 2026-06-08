#pragma once

#include "app/Config.h"

namespace Rml {
class Context;
}

namespace ppbrowser {

bool SetupChatDemo(Rml::Context* context, const AppConfig& config);
void UpdateChatDemo();
void ShutdownChatDemo();

} // namespace ppbrowser
