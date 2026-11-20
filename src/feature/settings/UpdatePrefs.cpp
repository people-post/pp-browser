#include "feature/settings/UpdatePrefs.h"

#include "foundation/data/UserPreferences.h"

namespace pbr {

UpdateCardState UpdatePrefs::Read(const std::string& data_dir) {
  UpdateCardState state;
  if (auto machine = UserPreferences::LoadMachine(data_dir)) {
    state.version = machine->update_card_version;
    state.dismissals = machine->update_card_dismissals;
    state.dismissed_ms = machine->update_card_dismissed_ms;
  }
  return state;
}

Roe<void> UpdatePrefs::Write(const std::string& data_dir, const UpdateCardState& state) {
  auto machine = UserPreferences::LoadMachine(data_dir);
  if (!machine) {
    return machine.error();
  }
  machine->update_card_version = state.version;
  machine->update_card_dismissals = state.dismissals;
  machine->update_card_dismissed_ms = state.dismissed_ms;
  return UserPreferences::SaveMachine(data_dir, *machine);
}

} // namespace pbr
