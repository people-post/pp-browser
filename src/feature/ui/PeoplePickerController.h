#pragma once

#include "common/Module.h"
#include "feature/ui/PeoplePickerLogic.h"

#include <RmlUi/Core/DataModelHandle.h>
#include <RmlUi/Core/Event.h>
#include <RmlUi/Core/Types.h>

#include <string>
#include <unordered_set>
#include <vector>

namespace Rml {
class Context;
}

namespace pbr {

class PeoplePickerController : public Module {
public:
  static PeoplePickerController& Instance();

  struct PickerRow {
    Rml::String id;
    Rml::String title;
    Rml::String subtitle;
    bool selected = false;
    bool locked = false;
  };

  bool RegisterModel(Rml::Context* context);

  /** Sessions + / Message a contact: 1→DM, 2+→group. */
  void OpenFree();

  /** From open DM: peer locked; create group when ≥1 extra selected. */
  void OpenFromDm(const std::string& locked_contact_id);

  void Close();

private:
  PeoplePickerController();

  static void ToggleContactCallback(Rml::DataModelHandle model, Rml::Event& ev, const Rml::VariantList& args);
  static void OnSearchChangedCallback(Rml::DataModelHandle model, Rml::Event& ev, const Rml::VariantList& args);
  static void ConfirmCallback(Rml::DataModelHandle model, Rml::Event& ev, const Rml::VariantList& args);
  static void CancelCallback(Rml::DataModelHandle model, Rml::Event& ev, const Rml::VariantList& args);

  void Open(PeoplePickerMode mode, std::unordered_set<std::string> locked_ids);
  void SyncRows();
  void UpdateCta();
  void DirtyAll();
  void OnToggleContact(const std::string& contact_id);
  void OnSearchChanged();
  void OnConfirm();
  void OnCancel();
  void FinishOpenThread();
  void StartDirectMessage(const std::string& contact_id);
  void StartGroup(const std::vector<std::string>& member_contact_ids);
  std::vector<std::string> SelectedContactIds() const;
  int FreeSelectedCount() const;

  Rml::Context* context_ = nullptr;
  int layer_id_ = -1;
  PeoplePickerMode mode_ = PeoplePickerMode::Free;
  std::unordered_set<std::string> locked_ids_;
  std::unordered_set<std::string> selected_ids_;
  std::vector<PickerRow> rows_;
  Rml::String search_query_;
  Rml::String title_;
  Rml::String cta_label_;
  bool cta_enabled_ = false;
};

} // namespace pbr
