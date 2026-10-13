#pragma once

#include "domain/people/ProfileIdentityView.h"
#include "common/Error.h"

#include <functional>
#include <string>
#include "common/PbrCompat.h"

namespace pbr {

/** App-filled ports for the first-run wizard. Application fills them; clear via BindPorts({}). */
struct OnboardingPorts {
  std::function<ProfileIdentityView()> load_identity;
  /** Register on the network with this nickname (the same call Me → Profile uses). */
  std::function<Roe<void>(const std::string& nickname)> register_identity;
  /** Run blocking work off the UI thread; on_done runs on the UI thread. */
  std::function<void(std::function<Roe<void>()> work, std::function<void(Roe<void>)> on_done)> run_heavy;
  std::function<void(const std::string& text)> copy_text;
  /** Step 5: opens the people picker so the user can search for friends. */
  std::function<void()> open_people_picker;
  std::function<bool()> is_completed;
  std::function<void(bool completed)> set_completed;
  /** Mount the wizard markup into the shell overlay; an empty body clears it. */
  std::function<void(const std::string& body)> mount;
};

} // namespace pbr
