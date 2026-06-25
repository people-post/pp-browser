#pragma once

#include "common/Module.h"

#include <RmlUi/Core/DataModelHandle.h>
#include <RmlUi/Core/Event.h>
#include <RmlUi/Core/Types.h>

#include <vector>

namespace Rml {
class Context;
}

namespace pbr {

class ContactsController : public Module {
public:
  static ContactsController& Instance();

  struct ContactListRow {
    Rml::String id;
    Rml::String title;
    Rml::String subtitle;
  };

  struct ContactDetail {
    Rml::String id;
    Rml::String display_name;
    Rml::String nickname;
    Rml::String relay_id;
    Rml::String trust;
  };

  bool RegisterModel(Rml::Context* context);
  void OnNavTabActivated();
  void SyncLayoutMode();

private:
  ContactsController();

  static void SelectContactCallback(Rml::DataModelHandle model, Rml::Event& ev, const Rml::VariantList& args);
  static void BackToListCallback(Rml::DataModelHandle model, Rml::Event& ev, const Rml::VariantList& args);
  static void StartChatCallback(Rml::DataModelHandle model, Rml::Event& ev, const Rml::VariantList& args);

  void SyncFromStore();
  void OnSelectContact(const std::string& contact_id);
  void OnBackToList();
  void OnStartChat();
  void DirtyAll();

  std::vector<ContactListRow> contacts_;
  bool compact_layout_ = false;
  bool show_detail_ = false;
  ContactDetail selected_;
  Rml::Context* context_ = nullptr;
};

} // namespace pbr
