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
    Rml::String trust;
  };

  struct ContactIdentityRow {
    Rml::String label;
    Rml::String value;
    Rml::String kind;
    bool is_primary = false;
  };

  struct ContactThreadRow {
    Rml::String id;
    Rml::String title;
    Rml::String channel_label;
    Rml::String kind;
  };

  struct ContactDetail {
    Rml::String id;
    Rml::String display_name;
    Rml::String nickname;
    Rml::String relay_id;
    Rml::String peer_id;
    Rml::String trust;
    Rml::String signing_fingerprint;
    std::vector<ContactIdentityRow> identities;
    std::vector<ContactThreadRow> threads;
  };

  bool RegisterModel(Rml::Context* context);
  void OnNavTabActivated();
  void SyncLayoutMode();
  /** Reload list from store (e.g. after AI add_contact while tab is open). */
  void Refresh();

private:
  ContactsController();

  static void SelectContactCallback(Rml::DataModelHandle model, Rml::Event& ev, const Rml::VariantList& args);
  static void BackToListCallback(Rml::DataModelHandle model, Rml::Event& ev, const Rml::VariantList& args);
  static void StartChatCallback(Rml::DataModelHandle model, Rml::Event& ev, const Rml::VariantList& args);
  static void SecureMessageCallback(Rml::DataModelHandle model, Rml::Event& ev, const Rml::VariantList& args);
  static void FindSomeoneCallback(Rml::DataModelHandle model, Rml::Event& ev, const Rml::VariantList& args);
  static void CopyIdCallback(Rml::DataModelHandle model, Rml::Event& ev, const Rml::VariantList& args);
  static void ShareContactCallback(Rml::DataModelHandle model, Rml::Event& ev, const Rml::VariantList& args);
  static void SetTrustCallback(Rml::DataModelHandle model, Rml::Event& ev, const Rml::VariantList& args);
  static void OpenThreadCallback(Rml::DataModelHandle model, Rml::Event& ev, const Rml::VariantList& args);
  static void OnSearchChangedCallback(Rml::DataModelHandle model, Rml::Event& ev, const Rml::VariantList& args);

  void SyncFromStore();
  void LoadSelectedDetail(const std::string& contact_id);
  void OnSelectContact(const std::string& contact_id);
  void OnBackToList();
  void OnStartChat();
  void OnSecureMessage();
  void OnFindSomeone();
  void OnCopyId();
  void OnShareContact();
  void OnSetTrust(const std::string& trust);
  void OnOpenThread(const std::string& thread_id);
  void OnSearchChanged();
  void DirtyAll();

  std::vector<ContactListRow> contacts_;
  Rml::String search_query_;
  bool compact_layout_ = false;
  bool show_detail_ = false;
  ContactDetail selected_;
  Rml::Context* context_ = nullptr;
};

} // namespace pbr
