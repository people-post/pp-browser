#include "feature/chat/ChatController.h"
#include "feature/chat/ChatDataModel.h"
#include "feature/chat/ChatWidgetHost.h"
#include "feature/ui/BadgeAggregator.h"
#include "base/i18n/LocalizationService.h"
#include "base/i18n/ScriptLanguageDetector.h"
#include "base/platform/AppLifecycle.h"
#include "base/platform/BackgroundSyncScheduler.h"
#include "base/platform/BrowserThread.h"
#include "base/platform/DesktopWindowChrome.h"
#include "base/platform/ILocalNotifier.h"
#include "base/platform/IPushDeviceRegistrar.h"
#include "feature/messaging/PushDeviceCoordinator.h"

#include "base/ai/StructuredTextParser.h"
#include "base/ai/WorkingSetPolicy.h"
#include "base/ai/conversation/Conversation.h"
#include "base/platform/IAssetLocator.h"
#include "base/ui/ContextMenuHost.h"
#include "base/ui/InputCoordinator.h"
#include "base/ui/ChatFormHelper.h"
#include "base/ui/RmlVariantHelpers.h"
#include "feature/chat/CalendarHelper.h"
#include "feature/ui/ChatSessionActions.h"
#include "feature/chat/ChatWidgetStateBuilder.h"
#include "feature/chat/MessagingTools.h"
#include "common/Utilities.h"
#include "common/StartupTiming.h"
#include "feature/messaging/MessagingHub.h"
#include "base/messaging/GroupTypes.h"
#include "base/people/PeerDisplayLabel.h"
#include "base/crypto/ProfileSecretsService.h"
#include "base/net/RegistrationClientUtil.h"
#include "base/messaging/AtAiParser.h"
#include "base/messaging/ChatPayloadValidator.h"
#include "base/messaging/MessagingLimits.h"
#include "base/messaging/MessagingJson.h"
#include "base/messaging/SendRelayOptions.h"
#include "base/messaging/SyncStateTypes.h"
#include "base/messaging/ThreadTypes.h"
#include "feature/ui/DataModelHost.h"
#include "feature/ui/DocumentLoader.h"
#include "feature/ui/DeferredStartup.h"
#include "feature/ui/PinGateController.h"
#include "feature/ui/ShellHost.h"
#include "feature/ui/ShellFeedback.h"
#include "feature/ui/UserFeedback.h"
#include "base/data/Config.h"
#include "base/data/LlmPreset.h"
#include "base/data/SessionStore.h"
#include "base/ui/ContextMenuHost.h"
#include "feature/ui/ContactsController.h"
#include "feature/ui/PeoplePickerController.h"

#include <RmlUi/Core/SystemInterface.h>
#include "feature/ui/SettingsController.h"

#include <nlohmann/json.hpp>

#include <RmlUi/Core/Context.h>
#include <RmlUi/Core/DataModelHandle.h>
#include <RmlUi/Core/Element.h>
#include <RmlUi/Core/ElementDocument.h>
#include <RmlUi/Core/SystemInterface.h>

#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace pbr {

namespace {

std::string ToolActivityLabel(const std::string& tool_name, const std::string& status) {
  if (tool_name == "web_search") {
    if (status == "running") {
      return "Searching the web...";
    }
    if (status == "done") {
      return "Search complete";
    }
    if (status == "error") {
      return "Search failed";
    }
  }
  if (status == "running") {
    return "Running " + tool_name + "...";
  }
  if (status == "done") {
    return "Finished " + tool_name;
  }
  return tool_name;
}

std::string MockAssistantRespond(const std::string& query) {
  const std::string lower = util::ToLowerAscii(query);

  if (lower.find("find someone") != std::string::npos || lower.find("search people") != std::string::npos) {
    return R"JSON({
      "blocks": [
        { "type": "paragraph", "text": "Here are people on the network (mock directory):" },
        { "type": "long_list", "title": "Search results", "items": [
          { "title": "Alice Example", "subtitle": "@alice", "meta": "relay:alice123",
            "actions": [
              { "label": "Message", "message": "Start chat with Alice", "payload": "{\"type\":\"start_conversation\",\"directory_hit\":{\"hit_id\":\"hit_alice\",\"display_name\":\"Alice Example\",\"nickname\":\"alice\",\"ids\":[{\"kind\":\"relay_user\",\"value\":\"relay:alice123\",\"primary\":true}]}}" },
              { "label": "Add contact", "message": "Add Alice", "payload": "{\"type\":\"add_contact\",\"directory_hit\":{\"hit_id\":\"hit_alice\",\"display_name\":\"Alice Example\",\"nickname\":\"alice\",\"ids\":[{\"kind\":\"relay_user\",\"value\":\"relay:alice123\",\"primary\":true}]}}" }
            ]
          }
        ]}
      ]
    })JSON";
  }

  if (lower.find("contacts") != std::string::npos || lower.find("conversations") != std::string::npos) {
    return R"JSON({
      "blocks": [
        { "type": "paragraph", "text": "Ask me to find someone on the network, or open a person thread from the sidebar." }
      ]
    })JSON";
  }

  if (lower.find("help") != std::string::npos) {
    return R"JSON({
      "blocks": [
        { "type": "heading", "level": 2, "text": "Help" },
        { "type": "paragraph", "text": "pp-browser renders structured JSON blocks with reactive widgets." },
        { "type": "list", "ordered": false, "items": [
          "Type a message and press Enter to send",
          "Try help, list, code, button, form, calendar, card, or poll",
          "Press Escape to quit"
        ]}
      ]
    })JSON";
  }

  if (lower.find("list") != std::string::npos) {
    return R"JSON({
      "blocks": [
        { "type": "paragraph", "text": "Here is an ordered list:" },
        { "type": "list", "ordered": true, "items": ["First item", "Second item", "Third item"] }
      ]
    })JSON";
  }

  if (lower.find("code") != std::string::npos) {
    return R"JSON({
      "blocks": [
        { "type": "paragraph", "text": "Example code block:" },
        { "type": "code", "text": "auto result = StructuredTextParser::ParseBlocksJson(json);\nif (result.ok) { /* render */ }" }
      ]
    })JSON";
  }

  if (lower.find("form") != std::string::npos) {
    return R"JSON({
      "blocks": [
        { "type": "paragraph", "text": "Fill out the booking form below and click Submit." },
        { "type": "form", "id": "booking", "title": "Booking form (mock)", "submit_label": "Submit",
          "submit_template": "Book for {{name}} on {{date}}",
          "fields": [
            { "id": "name", "label": "Name", "field_type": "text" },
            { "id": "date", "label": "Date", "field_type": "date" }
          ]
        }
      ]
    })JSON";
  }

  if (lower.find("calendar") != std::string::npos) {
    return MockCalendarReplyJson();
  }

  if (lower.find("card") != std::string::npos) {
    return R"JSON({
      "blocks": [
        { "type": "card", "title": "Sample card", "subtitle": "Reactive templates", "variant": "highlight",
          "body": "Static card blocks render inline inside the assistant bubble." }
      ]
    })JSON";
  }

  if (lower.find("poll") != std::string::npos) {
    return R"JSON({
      "blocks": [
        { "type": "poll", "question": "Which topic next?", "options": [
          { "label": "Calendar", "message": "Show me the calendar widget again." },
          { "label": "Forms", "message": "Show me the booking form again." }
        ]}
      ]
    })JSON";
  }

  if (lower.find("button") != std::string::npos) {
    return R"JSON({
      "blocks": [
        { "type": "paragraph", "text": "Click a suggestion to send it as your next message:" },
        { "type": "button", "label": "Explain more", "message": "Can you explain that in simpler terms?" },
        { "type": "button", "label": "Give an example", "message": "Can you give a concrete example?" }
      ]
    })JSON";
  }

  return R"JSON({
    "blocks": [
      { "type": "paragraph", "text": "Thanks for your message. This is a mock assistant response." },
      { "type": "paragraph", "text": "Try help, list, code, button, form, calendar, card, or poll." }
    ]
  })JSON";
}

} // namespace

ChatController::ChatController()
    : scroller_(context_,
                ChatTranscriptScroller::View{
                    .show_jump_to_latest = chat_.show_jump_to_latest,
                    .jump_to_latest_label = chat_.jump_to_latest_label,
                    .messages = chat_.messages,
                    .has_turns = chat_.has_turns,
                },
                messaging_ready_),
      working_set_(WorkingSetController::ShellView{
          .working_set_active = shell_.working_set_active,
          .working_set_title = shell_.working_set_title,
          .working_set_subtitle = shell_.working_set_subtitle,
          .working_set_rml = shell_.working_set_rml,
          .working_set = shell_.working_set,
      }),
      chrome_(ChatThreadChrome::View{
                 .draft_placeholder = chat_.draft_placeholder,
                 .status = chat_.status,
                 .thread_title = chat_.thread_title,
                 .thread_subtitle = chat_.thread_subtitle,
                 .peer_link_status = chat_.peer_link_status,
                 .peer_link_banner = chat_.peer_link_banner,
                 .show_peer_link = chat_.show_peer_link,
                 .show_peer_link_banner = chat_.show_peer_link_banner,
                 .show_retry_peer_dial = chat_.show_retry_peer_dial,
                 .thread_encrypted = chat_.thread_encrypted,
                 .thread_is_ai = chat_.thread_is_ai,
                 .thread_is_private = chat_.thread_is_private,
                 .thread_is_public = chat_.thread_is_public,
                 .thread_is_group = chat_.thread_is_group,
                 .compose_disabled = chat_.compose_disabled,
                 .show_thread_actions = chat_.show_thread_actions,
                 .show_peer_sheet = chat_.show_peer_sheet,
                 .show_forget_memory = chat_.show_forget_memory,
                 .show_sync_with_peer = chat_.show_sync_with_peer,
                 .show_thread_menu = chat_.show_thread_menu,
                 .show_gap_banner = chat_.show_gap_banner,
                 .show_compromised_banner = chat_.show_compromised_banner,
                 .show_psk_setup_banner = chat_.show_psk_setup_banner,
                 .show_psk_import = chat_.show_psk_import,
                 .psk_has_key = chat_.psk_has_key,
                 .psk_verified = chat_.psk_verified,
                 .psk_fingerprint = chat_.psk_fingerprint,
                 .psk_export_b64 = chat_.psk_export_b64,
                 .psk_import_text = chat_.psk_import_text,
                 .sync_in_progress = chat_.sync_in_progress,
                 .show_older_history_hint = chat_.show_older_history_hint,
                 .turns = chat_.turns,
                 .messages = chat_.messages,
                 .use_messages_layout = chat_.use_messages_layout,
                 .has_turns = chat_.has_turns,
             },
             messaging_ready_) {
  redirectLogger("ChatController");
  scroller_.SetDirtyTurns([]() { DirtyChatTurns(); });
  working_set_.SetWidgetLookup([this](const std::string& entry_id) -> const TurnWidgetState* {
    return widgets_.Find(entry_id);
  });
  chrome_.SetRefreshFromMessaging([this]() { RefreshFromMessaging(); });
  chrome_.SetWithSecrets([this](std::function<void()> action) { WithSecrets(std::move(action)); });
  chrome_.SetOnScrollerReset([this]() { scroller_.Reset(); });
  chrome_.SetCaptureScrollBeforePrepend([this]() { scroller_.CaptureScrollBeforePrependIfUnpinned(); });
  chrome_.SetExpandLoadedMinFromOlderPage(
      [this](const std::string& thread_id, int64_t before) {
        scroller_.ExpandLoadedMinFromOlderPage(thread_id, before);
      });
}

ChatController& ChatController::Instance() {
  static ChatController controller;
  return controller;
}

void ChatController::OpenWorkingSetCallback(Rml::DataModelHandle /*model*/, Rml::Event& /*ev*/,
                                      const Rml::VariantList& args) {
  if (args.size() < 2 || args[0].GetType() != Rml::Variant::STRING) {
    return;
  }
  const std::optional<int> block_index = EventArgAsInt(args, 1);
  if (!block_index || *block_index < 0) {
    return;
  }
  Instance().working_set_.Open(std::string(args[0].Get<Rml::String>().c_str()), *block_index);
}

void ChatController::SendMessageCallback(Rml::DataModelHandle /*model*/, Rml::Event& /*ev*/,
                                   const Rml::VariantList& /*args*/) {
  Instance().OnSendMessage();
}

void ChatController::SendSuggestionCallback(Rml::DataModelHandle /*model*/, Rml::Event& /*ev*/,
                                      const Rml::VariantList& args) {
  if (args.empty() || args[0].GetType() != Rml::Variant::STRING) {
    return;
  }
  Instance().SendUserText(std::string(args[0].Get<Rml::String>().c_str()));
}

void ChatController::SubmitFormCallback(Rml::DataModelHandle /*model*/, Rml::Event& /*ev*/,
                                  const Rml::VariantList& args) {
  if (args.size() < 2 || args[0].GetType() != Rml::Variant::STRING || args[1].GetType() != Rml::Variant::STRING) {
    return;
  }
  Instance().SubmitForm(std::string(args[0].Get<Rml::String>().c_str()),
                        std::string(args[1].Get<Rml::String>().c_str()));
}

void ChatController::SendChatActionCallback(Rml::DataModelHandle /*model*/, Rml::Event& /*ev*/,
                                      const Rml::VariantList& args) {
  if (args.size() < 2 || args[0].GetType() != Rml::Variant::STRING) {
    return;
  }

  const std::optional<int> action_index = EventArgAsInt(args, 1);
  if (!action_index || *action_index < 0) {
    return;
  }

  Instance().SendChatAction(std::string(args[0].Get<Rml::String>().c_str()), *action_index);
}

void ChatController::CalendarPrevCallback(Rml::DataModelHandle /*model*/, Rml::Event& /*ev*/,
                                    const Rml::VariantList& args) {
  if (args.empty() || args[0].GetType() != Rml::Variant::STRING) {
    return;
  }
  Instance().CalendarPrev(std::string(args[0].Get<Rml::String>().c_str()));
}

void ChatController::CalendarNextCallback(Rml::DataModelHandle /*model*/, Rml::Event& /*ev*/,
                                    const Rml::VariantList& args) {
  if (args.empty() || args[0].GetType() != Rml::Variant::STRING) {
    return;
  }
  Instance().CalendarNext(std::string(args[0].Get<Rml::String>().c_str()));
}

void ChatController::SelectCalendarDayCallback(Rml::DataModelHandle /*model*/, Rml::Event& /*ev*/,
                                         const Rml::VariantList& args) {
  if (args.size() < 2 || args[0].GetType() != Rml::Variant::STRING || args[1].GetType() != Rml::Variant::STRING) {
    return;
  }
  Instance().SelectCalendarDay(std::string(args[0].Get<Rml::String>().c_str()),
                               std::string(args[1].Get<Rml::String>().c_str()));
}

void ChatController::NewChatCallback(Rml::DataModelHandle /*model*/, Rml::Event& /*ev*/, const Rml::VariantList& /*args*/) {
  Instance().OnNewChat();
}

void ChatController::NewMessageCallback(Rml::DataModelHandle /*model*/, Rml::Event& /*ev*/,
                                        const Rml::VariantList& /*args*/) {
  Instance().OnNewMessage();
}

void ChatController::OpenNewSessionMenuCallback(Rml::DataModelHandle /*model*/, Rml::Event& ev,
                                                const Rml::VariantList& /*args*/) {
  Instance().OnOpenNewSessionMenu(ev);
}

void ChatController::OpenThreadActionsMenuCallback(Rml::DataModelHandle /*model*/, Rml::Event& ev,
                                                   const Rml::VariantList& /*args*/) {
  Instance().OnOpenThreadActionsMenu(ev);
}

void ChatController::OpenPeerSheetCallback(Rml::DataModelHandle /*model*/, Rml::Event& ev,
                                           const Rml::VariantList& /*args*/) {
  Instance().OnOpenPeerSheet(ev);
}

void ChatController::SelectThreadCallback(Rml::DataModelHandle /*model*/, Rml::Event& /*ev*/, const Rml::VariantList& args) {
  if (args.empty() || args[0].GetType() != Rml::Variant::STRING) {
    return;
  }
  Instance().OnSelectThread(std::string(args[0].Get<Rml::String>().c_str()));
}

void ChatController::CloseThreadCallback(Rml::DataModelHandle /*model*/, Rml::Event& ev, const Rml::VariantList& args) {
  ev.StopPropagation();
  if (args.empty() || args[0].GetType() != Rml::Variant::STRING) {
    return;
  }
  Instance().OnCloseThread(std::string(args[0].Get<Rml::String>().c_str()));
}

void ChatController::ClearHistoryCallback(Rml::DataModelHandle /*model*/, Rml::Event& /*ev*/,
                                          const Rml::VariantList& /*args*/) {
  Instance().OnClearHistory();
}

void ChatController::ForgetMemoryCallback(Rml::DataModelHandle /*model*/, Rml::Event& /*ev*/,
                                          const Rml::VariantList& /*args*/) {
  Instance().OnForgetMemory();
}

void ChatController::SyncWithPeerCallback(Rml::DataModelHandle /*model*/, Rml::Event& /*ev*/,
                                          const Rml::VariantList& /*args*/) {
  Instance().OnSyncWithPeer();
}

void ChatController::RetryGapSyncCallback(Rml::DataModelHandle /*model*/, Rml::Event& /*ev*/,
                                           const Rml::VariantList& /*args*/) {
  Instance().OnRetryGapSync();
}

void ChatController::StartNewSecureChatCallback(Rml::DataModelHandle /*model*/, Rml::Event& /*ev*/,
                                                const Rml::VariantList& /*args*/) {
  Instance().OnStartNewSecureChat();
}

void ChatController::PauseIntegrityCallback(Rml::DataModelHandle /*model*/, Rml::Event& /*ev*/,
                                            const Rml::VariantList& /*args*/) {
  Instance().OnPauseIntegrityOnly();
}

void ChatController::CopyPskKeyCallback(Rml::DataModelHandle /*model*/, Rml::Event& /*ev*/,
                                      const Rml::VariantList& /*args*/) {
  Instance().OnCopyPskKey();
}

void ChatController::TogglePskImportCallback(Rml::DataModelHandle /*model*/, Rml::Event& /*ev*/,
                                           const Rml::VariantList& /*args*/) {
  Instance().OnTogglePskImport();
}

void ChatController::ImportPskCallback(Rml::DataModelHandle /*model*/, Rml::Event& /*ev*/,
                                     const Rml::VariantList& /*args*/) {
  Instance().OnImportPsk();
}

void ChatController::VerifyPskCallback(Rml::DataModelHandle /*model*/, Rml::Event& /*ev*/,
                                       const Rml::VariantList& /*args*/) {
  Instance().OnVerifyPsk();
}

void ChatController::RotatePskExportCallback(Rml::DataModelHandle /*model*/, Rml::Event& /*ev*/,
                                             const Rml::VariantList& /*args*/) {
  Instance().OnRotatePskExport();
}

void ChatController::FinalizeThreadDisplay() {
  working_set_.ClearAll();
  RefreshFromMessaging();
  // Always land on latest when opening/showing a thread (incl. re-open same id).
  scroller_.RequestScrollToLatest();
  if (ShellHost::Instance().State().layout_mode == LayoutMode::Compact &&
      ShellHost::Instance().State().nav_tab == NavTab::Sessions) {
    ShellHost::Instance().OpenCompactChat();
  }
}

void ChatController::OnHomeTabActivated() {
  if (!messaging_ready_) {
    return;
  }
  MessagingHub::Instance().Inbox().ClearActiveThread();
  working_set_.ClearAll();
  ShellHost::Instance().SetPrimaryPane("home");
  RefreshFromMessaging();
  ShellHost::Instance().RequestSyncLayout();
  ShellHost::Instance().DirtyWindow();
}

void ChatController::OnSessionsTabActivated() {
  working_set_.ClearAll();
}

void ChatController::OnSelectThread(const std::string& thread_id) {
  if (!messaging_ready_) {
    return;
  }
  if (MessagingHub::Instance().Inbox().OpenThread(thread_id)) {
    ILocalNotifier::Instance().ClearForThread(thread_id);
    MessagingHub::Instance().P2p().MaybeTailSync(thread_id);
    ShellHost::Instance().SetPrimaryPane("chat");
    FinalizeThreadDisplay();
  }
}

void ChatController::OnCloseThread(const std::string& thread_id) {
  if (!messaging_ready_) {
    return;
  }

  auto finish_close = [this, thread_id]() {
    if (!MessagingHub::Instance().Inbox().CloseThread(thread_id)) {
      UserFeedback::Fail("Could not delete conversation");
      ShellHost::Instance().DirtyWindow();
      return;
    }
    chat_.draft = "";
    chat_.status = "";
    chat_.loading = false;
    pending_reply_.reset();
    working_set_.ClearAll();
    widgets_.ClearAll();
    chat_.turns.clear();
    RefreshFromMessaging();
    if (shell_.sessions.empty()) {
      ShellHost::Instance().SelectNavTab(NavTab::Home);
      ShellHost::Instance().CloseCompactChat();
    }
    ShellHost::Instance().RequestSyncLayout();
    ShellHost::Instance().DirtyWindow();
  };

  auto dismiss_and_close = [finish_close](const std::string& group_id) {
    (void)MessagingHub::Instance().Groups().DismissLocalGroup(group_id);
    finish_close();
  };

  auto thread = MessagingHub::Instance().Store().GetThread(thread_id);
  if (thread && *thread && (*thread)->kind == ThreadKind::Group && (*thread)->group_id) {
    const std::string group_id = *(*thread)->group_id;
    std::string local_identity;
    if (auto identity = MessagingHub::Instance().Identity().Get()) {
      local_identity = identity->relay_user_id;
    }

    bool is_owner = false;
    if (auto owner = MessagingHub::Instance().Groups().IsLocalOwner(group_id)) {
      is_owner = *owner;
    } else if (auto roster = MessagingHub::Instance().Groups().ListRoster(group_id)) {
      for (const GroupRosterMember& member : *roster) {
        if (member.member_identity == local_identity && member.role == MemberRole::Owner) {
          is_owner = true;
          break;
        }
      }
    }

    std::vector<GroupRosterMember> members;
    if (auto roster = MessagingHub::Instance().Groups().ListRoster(group_id)) {
      members = *roster;
    }

    std::vector<GroupRosterMember> successors;
    for (const GroupRosterMember& member : members) {
      if (member.member_identity == local_identity) {
        continue;
      }
      if (MessagingHub::Instance().Groups().IsMemberUnreachable(group_id, member.member_identity)) {
        continue;
      }
      successors.push_back(member);
    }

    bool owner_on_roster = false;
    if (auto owner_id = MessagingHub::Instance().Groups().OwnerIdentity(group_id)) {
      for (const GroupRosterMember& member : members) {
        if (member.member_identity == *owner_id) {
          owner_on_roster = true;
          break;
        }
      }
    }

    // Not the owner: normal leave — unless this is an orphaned/solo shell (owner missing or no
    // reachable peers), then dismiss locally. Covers "both sides solo, neither is owner".
    if (!is_owner) {
      const bool orphaned_shell = successors.empty() || !owner_on_roster;
      if (orphaned_shell) {
        ShellFeedback::ShowConfirm(ShellHost::Instance().State(), Tr("chat.group.dismiss_title"),
                                   Tr("chat.group.dismiss_confirm"), [dismiss_and_close, group_id](bool ok) {
                                     if (!ok) {
                                       return;
                                     }
                                     dismiss_and_close(group_id);
                                   });
      } else {
        ShellFeedback::ShowConfirm(ShellHost::Instance().State(), Tr("chat.group.leave_title"),
                                   Tr("chat.group.leave_confirm"),
                                   [finish_close, dismiss_and_close, group_id](bool ok) {
                                     if (!ok) {
                                       return;
                                     }
                                     if (auto left = MessagingHub::Instance().Groups().LeaveGroup(group_id); !left) {
                                       dismiss_and_close(group_id);
                                       return;
                                     }
                                     finish_close();
                                   });
      }
      ShellHost::Instance().RequestSyncLayout();
      ShellHost::Instance().DirtyWindow();
      return;
    }

    // Solo or only-unreachable peers: local dismiss — no transfer into a ghost roster.
    if (successors.empty()) {
      ShellFeedback::ShowConfirm(ShellHost::Instance().State(), Tr("chat.group.dismiss_title"),
                                 Tr("chat.group.dismiss_confirm"), [dismiss_and_close, group_id](bool ok) {
                                   if (!ok) {
                                     return;
                                   }
                                   dismiss_and_close(group_id);
                                 });
      ShellHost::Instance().RequestSyncLayout();
      ShellHost::Instance().DirtyWindow();
      return;
    }

    ShellFeedback::ShowConfirm(
        ShellHost::Instance().State(), Tr("chat.group.leave_title"), Tr("chat.group.leave_owner_confirm"),
        [finish_close, dismiss_and_close, group_id, successors](bool ok) {
          if (!ok) {
            return;
          }
          std::vector<ContextMenuAction> actions;
          for (const GroupRosterMember& member : successors) {
            std::string label = member.member_identity;
            if (auto contact = MessagingHub::Instance().Contacts().FindByIdentity(member.member_identity,
                                                                                  ContactIdKind::RelayUser)) {
              if (*contact) {
                label = (*contact)->display_name.empty() ? (*contact)->server_nickname : (*contact)->display_name;
                if (label.empty()) {
                  label = member.member_identity;
                }
              }
            }
            const std::string successor = member.member_identity;
            actions.push_back({
                "transfer_" + successor,
                "Transfer to " + label,
                nullptr,
                [finish_close, group_id, successor]() {
                  if (auto left = MessagingHub::Instance().Groups().LeaveAsOwner(group_id, successor); !left) {
                    UserFeedback::Fail(left.error().message);
                    ShellHost::Instance().DirtyWindow();
                    return;
                  }
                  finish_close();
                },
                "../icons/contacts.svg",
            });
          }
          actions.push_back({
              "dismiss_local",
              "Dismiss on this device only",
              nullptr,
              [dismiss_and_close, group_id]() { dismiss_and_close(group_id); },
              "../icons/trash.svg",
              true,
          });
          ContextMenuHost::Instance().ShowActions(Rml::Vector2i(120, 120), std::move(actions));
          ShellHost::Instance().DirtyWindow();
        });
    ShellHost::Instance().RequestSyncLayout();
    ShellHost::Instance().DirtyWindow();
    return;
  }

  ShellFeedback::ShowConfirm(ShellHost::Instance().State(), Tr("chat.delete_conversation"),
                             Tr("chat.delete_confirm"), [finish_close](bool ok) {
                               if (!ok) {
                                 return;
                               }
                               finish_close();
                             });
  ShellHost::Instance().RequestSyncLayout();
  ShellHost::Instance().DirtyWindow();
}

void ChatController::OnClearHistory() {
  if (!messaging_ready_) {
    return;
  }
  const std::string thread_id = MessagingHub::Instance().Inbox().ActiveThreadId();
  if (thread_id.empty()) {
    return;
  }

  auto thread = MessagingHub::Instance().Inbox().GetActiveThread();
  const bool is_ai = thread && thread->kind == ThreadKind::Ai;
  std::string message =
      "Remove all messages on this device? The thread stays in your sidebar. "
      "Messages on the peer's device or relay are not affected.";
  if (is_ai) {
    message += " AI memory is kept unless you check the box below.";
  } else if (thread && thread->kind == ThreadKind::Direct && thread->encrypted) {
    message += " Unsent outbound messages are cancelled; assigned sender_seq values are not reused.";
  }

  if (is_ai) {
    ShellFeedback::ShowConfirmWithCheckbox(
        ShellHost::Instance().State(), Tr("chat.clear_history"), message, "Also forget what AI learned", false,
        [this, thread_id](bool ok, bool forget_memory) {
          if (!ok) {
            return;
          }
          if (!MessagingHub::Instance().Inbox().ClearThreadHistory(thread_id, forget_memory)) {
            return;
          }
          chat_.draft = "";
          chat_.status = "";
          chat_.loading = false;
          pending_reply_.reset();
          widgets_.ClearAll();
          RefreshFromMessaging();
          ShellHost::Instance().RequestSyncLayout();
          ShellHost::Instance().DirtyWindow();
        });
  } else {
    ShellFeedback::ShowConfirm(ShellHost::Instance().State(), Tr("chat.clear_history"), message,
                               [this, thread_id](bool ok) {
                                 if (!ok) {
                                   return;
                                 }
                                 if (!MessagingHub::Instance().Inbox().ClearThreadHistory(thread_id, false)) {
                                   return;
                                 }
                                 chat_.draft = "";
                                 chat_.status = "";
                                 chat_.loading = false;
                                 pending_reply_.reset();
                                 widgets_.ClearAll();
                                 RefreshFromMessaging();
                                 ShellHost::Instance().RequestSyncLayout();
                                 ShellHost::Instance().DirtyWindow();
                               });
  }
  ShellHost::Instance().RequestSyncLayout();
  ShellHost::Instance().DirtyWindow();
}

void ChatController::OnForgetMemory() {
  if (!messaging_ready_) {
    return;
  }
  const std::string thread_id = MessagingHub::Instance().Inbox().ActiveThreadId();
  if (thread_id.empty()) {
    return;
  }

  ShellFeedback::ShowConfirm(
      ShellHost::Instance().State(), "Forget what AI learned?",
      "Delete the durable conversation summary? Your message transcript stays on this device.",
      [this, thread_id](bool ok) {
        if (!ok) {
          return;
        }
        if (!MessagingHub::Instance().Inbox().ForgetThreadMemory(thread_id)) {
          return;
        }
        ShellHost::Instance().RequestSyncLayout();
        ShellHost::Instance().DirtyWindow();
      });
  ShellHost::Instance().RequestSyncLayout();
  ShellHost::Instance().DirtyWindow();
}

void ChatController::LoadOlderHistoryCallback(Rml::DataModelHandle /*model*/, Rml::Event& /*ev*/,
                                              const Rml::VariantList& /*args*/) {
  Instance().OnLoadOlderHistory();
}

void ChatController::RetryPeerDialCallback(Rml::DataModelHandle /*model*/, Rml::Event& /*ev*/,
                                           const Rml::VariantList& /*args*/) {
  Instance().OnRetryPeerDial();
}

void ChatController::SendSharedAssistantRelay(const std::string& thread_id, const AtAiMode mode,
                                              const std::string& plain_text) {
  if (!messaging_ready_ || plain_text.empty()) {
    return;
  }
  auto thread = MessagingHub::Instance().Store().GetThread(thread_id);
  if (!thread || !*thread) {
    return;
  }

  SendRelayOptions opts;
  opts.sender_contact_id = kAiAssistantContactId;
  opts.generation = "ai_on_behalf";
  opts.ai_invoke_mode = mode == AtAiMode::SharedFull ? "shared_full" : "shared_reply";
  if (!(*thread)->participant_contact_ids.empty()) {
    opts.seq_owner_contact_id = (*thread)->participant_contact_ids.front();
  }
  opts.update_preview = true;
  (void)MessagingHub::Instance().P2p().SendUserMessage(thread_id, plain_text, opts);
}

void ChatController::RefreshFromMessaging() {
  const int prev_sessions = ShellHost::Instance().State().nav_badges.sessions_unread;
  const int prev_contacts = ShellHost::Instance().State().nav_badges.contacts_unread;
  SyncShellSessions();
  SyncDisplayFromThread();
  chrome_.Update();
  BadgeAggregator::Instance().Refresh();
  DirtyChat();
  DirtyShell();
  ShellHost::Instance().DirtyWindow();
  const NavBadgeState& badges = ShellHost::Instance().State().nav_badges;
  if (badges.sessions_unread != prev_sessions || badges.contacts_unread != prev_contacts) {
    ShellHost::Instance().RequestRemountNavRail();
  }
}

void ChatController::OnProfileDataReset() {
  messaging_ready_ = false;
  working_set_.ClearAll();
  widgets_.ClearAll();
  pending_reply_.reset();
  chrome_.ResetPanelState();
  if (MessagingHub::Instance().IsInitialized()) {
    WireMessagingBindings();
  } else {
    DirtyChat();
    DirtyShell();
  }
}

void ChatController::SyncShellSessions() {
  if (!messaging_ready_) {
    return;
  }
  shell_.sessions.clear();
  auto threads = MessagingHub::Instance().Inbox().ListThreads();
  if (!threads) {
    return;
  }
  std::vector<Thread> sorted_threads = *threads;
  std::sort(sorted_threads.begin(), sorted_threads.end(),
            [](const Thread& a, const Thread& b) { return a.updated_at > b.updated_at; });

  const std::string active_id = MessagingHub::Instance().Inbox().ActiveThreadId();
  auto& inbox = MessagingHub::Instance().Inbox();
  for (const Thread& thread : sorted_threads) {
    SessionRow row;
    row.id = thread.id.c_str();
    row.title = inbox.ResolveThreadLabel(thread).title.c_str();
    row.preview = thread.preview.c_str();
    row.kind = SessionVisualKind(thread);
    row.unread_count = thread.unread_count;
    row.unread_display = FormatBadgeCount(thread.unread_count).c_str();
    row.active = thread.id == active_id;
    row.closable = true;
    shell_.sessions.push_back(std::move(row));
  }
}

void ChatController::OnMessagesScroll() {
  scroller_.OnMessagesScroll();
}

void ChatController::OnJumpToLatest() {
  scroller_.OnJumpToLatest();
}

void ChatController::MessagesScrollCallback(Rml::DataModelHandle /*model*/, Rml::Event& /*ev*/,
                                            const Rml::VariantList& /*args*/) {
  Instance().OnMessagesScroll();
}

void ChatController::JumpToLatestCallback(Rml::DataModelHandle /*model*/, Rml::Event& /*ev*/,
                                          const Rml::VariantList& /*args*/) {
  Instance().OnJumpToLatest();
}

void ChatController::OnRetryPeerDial() {
  chrome_.OnRetryPeerDial();
}

void ChatController::OnLoadOlderHistory() {
  chrome_.OnLoadOlderHistory();
}

void ChatController::OnSyncWithPeer() {
  chrome_.OnSyncWithPeer();
}

void ChatController::OnRetryGapSync() {
  chrome_.OnRetryGapSync();
}

void ChatController::OnStartNewSecureChat() {
  chrome_.OnStartNewSecureChat();
}

void ChatController::OnPauseIntegrityOnly() {
  chrome_.OnPauseIntegrityOnly();
}

void ChatController::OnCopyPskKey() {
  chrome_.OnCopyPskKey();
}

void ChatController::OnTogglePskImport() {
  chrome_.OnTogglePskImport();
}

void ChatController::OnImportPsk() {
  chrome_.OnImportPsk();
}

void ChatController::OnVerifyPsk() {
  chrome_.OnVerifyPsk();
}

void ChatController::OnRotatePskExport() {
  chrome_.OnRotatePskExport();
}

void ChatController::UpdateThreadChrome() {
  chrome_.Update();
}

void ChatController::UpdatePeerLinkChrome() {
  chrome_.UpdatePeerLink();
}

void ChatController::ResetChatPanelState() {
  chrome_.ResetPanelState();
}

void ChatController::SyncDisplayFromThread() {
  if (!messaging_ready_) {
    return;
  }
  auto& inbox = MessagingHub::Instance().Inbox();
  if (!inbox.GetActiveThread()) {
    chrome_.ResetPanelState();
    return;
  }
  const std::string thread_id = inbox.ActiveThreadId();
  const bool thread_changed = scroller_.BeginDisplaySync(thread_id);

  const std::string prev_tail_id =
      chat_.messages.empty() ? std::string() : std::string(chat_.messages.back().message_id.c_str());
  const size_t prev_count = chat_.messages.size();

  chat_.messages = inbox.BuildDisplayRows(thread_id, scroller_.LoadedMinDisplayOrder());
  chat_.turns.clear();
  chat_.has_turns = !chat_.messages.empty();
  chat_.use_messages_layout = true;

  scroller_.EndDisplaySync(thread_changed, prev_tail_id, prev_count);
}

void ChatController::HandleLocalAction(const std::string& message, const std::optional<std::string>& payload) {
  if (payload && !payload->empty()) {
    const nlohmann::json action_json = nlohmann::json::parse(*payload, nullptr, false);
    if (action_json.is_object() && action_json.contains("type") && action_json["type"].is_string() &&
        action_json["type"].get<std::string>() == "fork_group") {
      const std::string confirmed_payload = *payload;
      ShellFeedback::ShowConfirm(
          ShellHost::Instance().State(), "Start a new group?",
          "This creates a new group with a fresh history. People who are still reachable can be invited again.",
          [this, message, confirmed_payload](bool ok) {
            if (!ok) {
              return;
            }
            auto result = MessagingHub::Instance().Actions().Dispatch(confirmed_payload);
            if (!result) {
              log().warning << "Local action failed: " << result.error().message;
              ShellFeedback::ShowToast(ShellHost::Instance().State(), result.error().message);
              ShellHost::Instance().DirtyWindow();
              return;
            }
            if (*result) {
              SendUserText(message, *result);
              return;
            }
            RefreshFromMessaging();
            ContactsController::Instance().Refresh();
            if (!MessagingHub::Instance().Inbox().ActiveThreadId().empty()) {
              ShellHost::Instance().SelectNavTab(NavTab::Sessions);
              ShellHost::Instance().SetPrimaryPane("chat");
              if (ShellHost::Instance().State().layout_mode == LayoutMode::Compact) {
                ShellHost::Instance().OpenCompactChat();
              }
            }
            ShellHost::Instance().DirtyWindow();
          });
      ShellHost::Instance().DirtyWindow();
      return;
    }
    auto result = MessagingHub::Instance().Actions().Dispatch(*payload);
    if (!result) {
      // Never re-route the same action payload through MessageRouter — that loops
      // HandleLocalAction → SendUserText → Route → HandleLocalAction until stack overflow.
      log().warning << "Local action failed: " << result.error().message;
      ShellFeedback::ShowToast(ShellHost::Instance().State(), result.error().message);
      ShellHost::Instance().DirtyWindow();
      return;
    }
    if (*result) {
      SendUserText(message, *result);
      return;
    }
    RefreshFromMessaging();
    ContactsController::Instance().Refresh();
    if (!MessagingHub::Instance().Inbox().ActiveThreadId().empty()) {
      ShellHost::Instance().SelectNavTab(NavTab::Sessions);
      ShellHost::Instance().SetPrimaryPane("chat");
      if (ShellHost::Instance().State().layout_mode == LayoutMode::Compact) {
        ShellHost::Instance().OpenCompactChat();
      }
    }
    return;
  }
  SendUserText(message, payload);
}

void ChatController::OnSendMessage() {
  if (chat_.loading || chat_.compose_disabled) {
    return;
  }

  const std::string text = util::Trim(chat_.draft.c_str());
  if (text.empty()) {
    return;
  }

  if (auto valid = ChatPayloadValidator::ValidateOutboundText(text); !valid) {
    ShellFeedback::ShowToast(ShellHost::Instance().State(), "Message is too long to send.");
    return;
  }

  chat_.draft = "";
  DirtyChatChrome();
  scroller_.RequestScrollToLatest();
  SendUserText(text);
}

void ChatController::OnNewChat() {
  if (!messaging_ready_) {
    return;
  }

  (void)MessagingHub::Instance().Inbox().CreateNewAiThread();
  chat_.draft = "";
  chat_.status = "";
  chat_.loading = false;
  chat_.compose_disabled = false;
  pending_reply_.reset();
  if (agent_) {
    agent_->Cancel();
  }
  working_set_.ClearAll();
  widgets_.ClearAll();
  ShellHost::Instance().SetPrimaryPane("chat");
  focus_draft_after_sync_ = true;
  FinalizeThreadDisplay();
  ShellHost::Instance().DirtyWindow();
}

void ChatController::OnNewMessage() {
  PeoplePickerController::Instance().OpenFree();
}

void ChatController::OnOpenNewSessionMenu(Rml::Event& ev) {
  const Rml::Vector2i position = MenuPositionBelowEvent(ev);

  std::vector<ContextMenuAction> actions;
  actions.push_back({
      "chat_with_ai",
      "Chat with AI",
      nullptr,
      []() { ChatController::Instance().OnNewChat(); },
      "../icons/sparkle.svg",
  });
  actions.push_back({
      "message_contact",
      "Message a contact",
      nullptr,
      []() { ChatController::Instance().OnNewMessage(); },
      "../icons/contacts.svg",
  });
  actions.push_back({
      "find_someone",
      "Find someone",
      nullptr,
      []() { ChatController::Instance().OnFindSomeone(); },
      "../icons/message.svg",
  });
  ContextMenuHost::Instance().ShowActions(position, std::move(actions));
}

void ChatController::OnOpenThreadActionsMenu(Rml::Event& ev) {
  // Anchor near the right edge of the trigger so the menu stays in the header corner.
  const Rml::Vector2i position = MenuPositionBelowRightAlignedEvent(ev);

  std::vector<ContextMenuAction> actions;
  if (chat_.show_sync_with_peer && !chat_.sync_in_progress) {
    actions.push_back({
        "sync_with_peer",
        "Sync with peer",
        nullptr,
        []() { ChatController::Instance().OnSyncWithPeer(); },
        "../icons/sync.svg",
    });
  }
  if (chat_.show_thread_actions) {
    actions.push_back({
        "clear_history",
        "Clear history…",
        nullptr,
        []() { ChatController::Instance().OnClearHistory(); },
        "../icons/trash.svg",
        true,
    });
  }
  if (chat_.show_forget_memory) {
    actions.push_back({
        "forget_memory",
        "Forget AI memory…",
        nullptr,
        []() { ChatController::Instance().OnForgetMemory(); },
        "../icons/sparkle.svg",
        true,
    });
  }
  if (actions.empty()) {
    return;
  }
  ContextMenuHost::Instance().ShowActions(position, std::move(actions));
}

void ChatController::OnOpenPeerSheet(Rml::Event& ev) {
  if (!messaging_ready_ || !chat_.show_peer_sheet) {
    return;
  }
  auto thread = MessagingHub::Instance().Inbox().GetActiveThread();
  if (!thread) {
    return;
  }

  const Rml::Vector2i position = MenuPositionBelowEvent(ev);

  std::vector<ContextMenuAction> actions;
  if (thread->kind == ThreadKind::Direct) {
    const PeerDisplayLabel label = MessagingHub::Instance().Inbox().ResolveThreadLabel(*thread);
    const std::string peer_id = thread->peer_identity_value;
    std::optional<std::string> dm_contact_id = label.contact_id;
    if (!dm_contact_id && !thread->participant_contact_ids.empty()) {
      dm_contact_id = thread->participant_contact_ids.front();
    }
    if (dm_contact_id) {
      const std::string contact_id = *dm_contact_id;
      actions.push_back({
          "add_people",
          "Add people…",
          nullptr,
          [contact_id]() { PeoplePickerController::Instance().OpenFromDm(contact_id); },
          "../icons/group.svg",
      });
      actions.push_back({
          "edit_contact",
          "Edit contact",
          nullptr,
          [contact_id]() { ContactsController::Instance().OnSelectContact(contact_id); },
          "../icons/contacts.svg",
      });
    } else {
      actions.push_back({
          "add_contact",
          "Add to contacts",
          nullptr,
          [peer_id]() {
            DirectoryHit hit;
            if (auto shadow = MessagingHub::Instance().DirectoryShadows().Get(peer_id)) {
              hit = *shadow;
            } else {
              hit.hit_id = peer_id;
              hit.ids = {{ContactIdKind::RelayUser, peer_id, true}};
            }
            auto created = MessagingHub::Instance().Contacts().AddFromDirectoryHit(hit);
            if (!created) {
              UserFeedback::Fail("Could not add contact");
              ShellHost::Instance().DirtyWindow();
              return;
            }
            ContactsController::Instance().OnSelectContact(created->id);
            MessagingHub::Instance().Inbox().NotifyThreadChanged();
          },
          "../icons/contacts.svg",
      });
    }
    if (!peer_id.empty()) {
      actions.push_back({
          "copy_id",
          "Copy ID",
          nullptr,
          [peer_id]() {
            if (Rml::SystemInterface* system = Rml::GetSystemInterface()) {
              system->SetClipboardText(peer_id.c_str());
            }
            ShellFeedback::ShowToast(ShellHost::Instance().State(), "ID copied");
            ShellHost::Instance().DirtyWindow();
          },
          "../icons/copy.svg",
      });
    }
  } else if (thread->kind == ThreadKind::Group && thread->group_id) {
    const std::string thread_id = thread->id;
    const std::string group_id = *thread->group_id;
    const std::string current_local = thread->local_title;
    const PeerDisplayLabel label = MessagingHub::Instance().Inbox().ResolveThreadLabel(*thread);
    const std::string shared_default = label.shared_title.value_or(thread->title);

    actions.push_back({
        "rename_for_me",
        "Rename for me",
        nullptr,
        [thread_id, current_local, shared_default]() {
          ShellFeedback::ShowPrompt(
              ShellHost::Instance().State(), "Rename for me", "Local nickname for this group",
              current_local.empty() ? shared_default : current_local,
              [thread_id](bool ok, std::string value) {
                if (!ok) {
                  return;
                }
                if (auto saved = MessagingHub::Instance().Inbox().SetThreadLocalTitle(thread_id, value); !saved) {
                  UserFeedback::Fail(saved.error().message);
                }
                ShellHost::Instance().DirtyWindow();
              });
          ShellHost::Instance().DirtyWindow();
        },
        "../icons/message.svg",
    });
    if (!current_local.empty()) {
      actions.push_back({
          "clear_my_name",
          "Clear my name",
          nullptr,
          [thread_id]() {
            (void)MessagingHub::Instance().Inbox().SetThreadLocalTitle(thread_id, "");
            ShellHost::Instance().DirtyWindow();
          },
          "../icons/trash.svg",
      });
    }

    bool is_owner = false;
    std::string local_identity;
    if (auto identity = MessagingHub::Instance().Identity().Get()) {
      local_identity = identity->relay_user_id;
      if (auto roster = MessagingHub::Instance().Groups().ListRoster(group_id)) {
        for (const auto& member : *roster) {
          if (member.member_identity == identity->relay_user_id && member.role == MemberRole::Owner) {
            is_owner = true;
            break;
          }
        }
      }
    }
    const bool owner_unreachable = MessagingHub::Instance().Groups().IsOwnerUnreachable(group_id);
    if (is_owner) {
      actions.push_back({
          "rename_for_everyone",
          "Rename for everyone",
          nullptr,
          [group_id, shared_default]() {
            ShellFeedback::ShowPrompt(
                ShellHost::Instance().State(), "Rename for everyone", "Shared group name for all members",
                shared_default, [group_id](bool ok, std::string value) {
                  if (!ok) {
                    return;
                  }
                  if (value.empty()) {
                    UserFeedback::Fail("Title required");
                    ShellHost::Instance().DirtyWindow();
                    return;
                  }
                  if (auto renamed = MessagingHub::Instance().Groups().RenameGroupShared(group_id, value); !renamed) {
                    UserFeedback::Fail(renamed.error().message);
                  } else {
                    MessagingHub::Instance().Inbox().NotifyThreadChanged();
                  }
                  ShellHost::Instance().DirtyWindow();
                });
            ShellHost::Instance().DirtyWindow();
          },
          "../icons/group.svg",
      });
      for (const std::string& unreachable_id : MessagingHub::Instance().Groups().ListUnreachable(group_id)) {
        if (unreachable_id == local_identity) {
          continue;
        }
        std::string name = unreachable_id;
        if (auto contact =
                MessagingHub::Instance().Contacts().FindByIdentity(unreachable_id, ContactIdKind::RelayUser)) {
          if (*contact) {
            name = (*contact)->display_name.empty() ? (*contact)->server_nickname : (*contact)->display_name;
            if (name.empty()) {
              name = unreachable_id;
            }
          }
        }
        actions.push_back({
            "remove_unreachable_" + unreachable_id,
            "Remove unreachable: " + name,
            nullptr,
            [group_id, unreachable_id, name]() {
              ShellFeedback::ShowConfirm(
                  ShellHost::Instance().State(), "Remove member",
                  "Remove " + name + " from this group? Others will be notified.",
                  [group_id, unreachable_id](bool ok) {
                    if (!ok) {
                      return;
                    }
                    if (auto removed =
                            MessagingHub::Instance().Groups().RemoveMemberByIdentity(group_id, unreachable_id);
                        !removed) {
                      UserFeedback::Fail(removed.error().message);
                    } else {
                      MessagingHub::Instance().Inbox().NotifyThreadChanged();
                    }
                    ShellHost::Instance().DirtyWindow();
                  });
              ShellHost::Instance().DirtyWindow();
            },
            "../icons/trash.svg",
            true,
        });
      }
    } else if (owner_unreachable) {
      actions.push_back({
          "rename_for_everyone",
          "Rename for everyone",
          nullptr,
          []() {
            ShellFeedback::ShowToast(ShellHost::Instance().State(),
                                     "Only the owner can do that — see the note in the chat");
            ShellHost::Instance().DirtyWindow();
          },
          "../icons/group.svg",
      });
    }
  }

  if (actions.empty()) {
    return;
  }
  ContextMenuHost::Instance().ShowActions(position, std::move(actions));
}

void ChatController::OnFindSomeone() {
  if (!messaging_ready_) {
    return;
  }
  OnNewChat();
  focus_draft_after_sync_ = true;
  chat_.draft = "Find someone on the network";
  DirtyChatChrome();
}

void ChatController::OnShellLayoutSynced() {
  // SyncLayout remounts the shell DOM and resets scroll offsets — re-arm follow-tail.
  scroller_.OnShellRemounted();
  if (!focus_draft_after_sync_) {
    return;
  }
  focus_draft_after_sync_ = false;
  if (!context_ || context_->GetNumDocuments() == 0) {
    return;
  }
  if (Rml::Element* draft = context_->GetDocument(0)->GetElementById("draft-input")) {
    draft->Focus();
  }
}

void ChatController::SubmitForm(const std::string& entry_id, const std::string& form_id) {
  if (chat_.loading) {
    return;
  }
  const auto submission = widgets_.TrySubmit(entry_id, form_id);
  if (!submission) {
    log().warning << "Ignoring submit for inactive or expired form: " << entry_id << "/" << form_id;
    return;
  }
  working_set_.Clear();
  SyncDisplayFromThread();
  SendUserText(submission->display_text, submission->payload);
}

void ChatController::CalendarPrev(const std::string& entry_id) {
  if (!widgets_.ShiftCalendar(entry_id, -1)) {
    return;
  }
  if (working_set_.ActiveEntryId() == entry_id) {
    working_set_.SyncWidgetBindings(entry_id);
  }
  SyncDisplayFromThread();
}

void ChatController::CalendarNext(const std::string& entry_id) {
  if (!widgets_.ShiftCalendar(entry_id, 1)) {
    return;
  }
  if (working_set_.ActiveEntryId() == entry_id) {
    working_set_.SyncWidgetBindings(entry_id);
  }
  SyncDisplayFromThread();
}

void ChatController::SelectCalendarDay(const std::string& entry_id, const std::string& iso_date) {
  if (chat_.loading) {
    return;
  }
  if (!widgets_.IsCalendarDayAvailable(entry_id, iso_date)) {
    return;
  }
  SendUserText("Selected " + iso_date);
}

std::string ChatController::HydrateAssistantRml(const TranscriptEntry& entry) const {
  return widgets_.HydrateAssistantRml(entry);
}

bool ChatController::IsFormEditable(const std::string& entry_id, const std::string& form_id) const {
  return widgets_.IsFormEditable(entry_id, form_id);
}

void ChatController::InitializeWidgetState(const std::string& entry_id, const std::vector<WidgetInit>& inits) {
  widgets_.Initialize(entry_id, inits);
}

void ChatController::MergeWidgetStateIntoRow(const std::string& entry_id, TranscriptDisplayRow& row) const {
  widgets_.MergeIntoRow(entry_id, row);
}

TurnWidgetState* ChatController::FindWidgetState(const std::string& entry_id) {
  return widgets_.Find(entry_id);
}

const TurnWidgetState* ChatController::FindWidgetState(const std::string& entry_id) const {
  return widgets_.Find(entry_id);
}

void ChatController::ClearFormState() {
  widgets_.ClearForms();
}

void ChatController::UpdateSidebarPreview(const std::string& preview_text) {
  if (!messaging_ready_) {
    return;
  }
  const std::string thread_id = MessagingHub::Instance().Inbox().ActiveThreadId();
  (void)MessagingHub::Instance().Inbox().UpdatePreview(thread_id, preview_text);
  SyncShellSessions();
  DirtyShell();
}

bool ChatController::EnsureHomeOutboundSession() {
  if (!messaging_ready_) {
    return false;
  }
  if (!MessagingHub::Instance().Inbox().CreateNewAiThread()) {
    return false;
  }
  ShellHost::Instance().SelectNavTab(NavTab::Sessions);
  ShellHost::Instance().SetPrimaryPane("chat");
  FinalizeThreadDisplay();
  ShellHost::Instance().DirtyWindow();
  return true;
}

void ChatController::SendUserText(const std::string& text, std::optional<std::string> user_payload) {
  const std::string trimmed = util::Trim(text);
  if (trimmed.empty() || chat_.loading) {
    return;
  }
  if (chat_.compose_disabled && messaging_ready_) {
    return;
  }
  if (!messaging_ready_) {
    WithSecrets([this, trimmed, user_payload = std::move(user_payload)]() mutable {
      SendUserText(trimmed, std::move(user_payload));
    });
    return;
  }

  if (ShellHost::Instance().State().nav_tab == NavTab::Home) {
    if (!EnsureHomeOutboundSession()) {
      return;
    }
  }

  widgets_.ExpireOpenForms();
  DirtyChatTurns();

  const bool use_mock_reply = !use_llm_;
  bool expect_agent_work = use_mock_reply;
  if (!expect_agent_work && messaging_ready_ && MessagingHub::Instance().HasRouter()) {
    expect_agent_work = MessagingHub::Instance().Router().ExpectsAgentWork(
        MessagingHub::Instance().Inbox().ActiveThreadId(), trimmed, user_payload);
  } else if (!expect_agent_work) {
    expect_agent_work = true;
  }

  if (expect_agent_work) {
    chat_.loading = true;
    chat_.status = "";
  }
  UpdateSidebarPreview(trimmed);
  DirtyChatChrome();

  if (!use_llm_) {
    const std::string thread_id = MessagingHub::Instance().Inbox().ActiveThreadId();
    ThreadMessage user_message;
    user_message.id = util::GenerateUuid();
    user_message.thread_id = thread_id;
    user_message.sender_contact_id = kLocalSelfContactId;
    user_message.text = trimmed;
    user_message.timestamp = util::NowUnixMs();
    user_message.transport = MessageTransport::Local;
    (void)MessagingHub::Instance().Store().AppendMessage(user_message);
    SyncDisplayFromThread();
    DirtyChatTurns();
    log().debug << "Using mock assistant response";
    pending_reply_ = PendingReply{.entry_id = user_message.id, .thread_id = thread_id,
                                  .output = MockAssistantRespond(trimmed), .from_llm = false};
    return;
  }

  if (messaging_ready_ && MessagingHub::Instance().HasRouter()) {
    log().info << "Routing message via MessageRouter";
    (void)MessagingHub::Instance().Router().Route(MessagingHub::Instance().Inbox().ActiveThreadId(), trimmed,
                                                  std::move(user_payload));
    SyncDisplayFromThread();
    return;
  }

  log().info << "Submitting message to agent session";
  agent_->Submit(trimmed, std::move(user_payload));
}

void ChatController::SendChatAction(const std::string& entry_id, int action_index) {
  if (chat_.loading || action_index < 0 || !messaging_ready_) {
    return;
  }

  const std::string thread_id = MessagingHub::Instance().Inbox().ActiveThreadId();
  auto messages = MessagingHub::Instance().Store().GetMessagesPage(thread_id, std::nullopt, 10000);
  if (!messages) {
    return;
  }

  for (const ThreadMessage& message : *messages) {
    if (message.id != entry_id) {
      continue;
    }
    if (action_index >= static_cast<int>(message.chat_actions.size())) {
      return;
    }
    const TranscriptChatAction& action = message.chat_actions[static_cast<size_t>(action_index)];
    // Copy by value: layout-mutating actions close the working set / remount panes. Doing that
    // synchronously destroys the click target and leaves RmlUi data views with dangling aliases
    // ("Variable address not found" → segfault). Defer like SettingsController section open.
    const std::string action_message = action.message;
    const std::optional<std::string> action_payload = action.payload;
    const bool close_working_set = working_set_.ShouldCloseForAction(action_payload);
    BrowserThread::PostTask(BrowserThreadId::UI, [action_message, action_payload, close_working_set]() {
      auto& self = ChatController::Instance();
      if (close_working_set) {
        self.working_set_.Clear();
      }
      self.HandleLocalAction(action_message, action_payload);
    });
    return;
  }

  log().warning << "Chat action entry not found: " << entry_id;
}

void ChatController::FinishAssistantReply(const std::string& entry_id, const std::string& raw_output, const bool from_llm,
                                    const std::string& finish_reason, const std::string& thread_id,
                                    ResponseGoal response_goal, RenderMode render_mode, const AtAiMode shared_ai_mode) {
  if (!from_llm) {
    response_goal = InferResponseGoalFromBlocksJson(raw_output);
  }

  auto parsed = from_llm ? StructuredTextParser::ParseFromLlmOutput(raw_output, response_goal, render_mode)
                         : StructuredTextParser::ParseBlocksJson(raw_output, response_goal, render_mode);
  if (!parsed.ok) {
    log().warning << "Failed to parse assistant reply: " << parsed.error;
    if (from_llm && !finish_reason.empty()) {
      log().warning << "LLM finish_reason: " << finish_reason;
    }
    if (messaging_ready_) {
      const std::string active_thread = thread_id.empty() ? MessagingHub::Instance().Inbox().ActiveThreadId() : thread_id;
      auto messages = MessagingHub::Instance().Store().GetMessagesPage(active_thread, std::nullopt, 10000);
      if (messages) {
        for (ThreadMessage& message : *messages) {
          if (message.id == entry_id) {
            message.content_rml = ApplyLangAttribute(R"(<div class="bubble bubble-assistant")", parsed.error) +
                                  R"( selectable="text"><p class="error">)" + StructuredTextParser::EscapeText(parsed.error) + "</p></div>";
            (void)MessagingHub::Instance().Store().UpdateMessage(message);
            break;
          }
        }
      }
    }
  } else {
    for (const std::string& warning : parsed.warnings) {
      log().warning << "Skipped block in assistant reply: " << warning;
    }

    if (!parsed.widget_inits.empty()) {
      widgets_.Initialize(entry_id, parsed.widget_inits);
    }

    std::vector<TranscriptChatAction> chat_actions;
    chat_actions.reserve(parsed.chat_actions.size());
    for (const ParsedChatAction& action : parsed.chat_actions) {
      chat_actions.push_back({action.label, action.message, action.payload});
    }

    std::string hydrated = InjectEntryPlaceholders(parsed.rml, entry_id);
    hydrated = HydrateChatActionButtons(hydrated, chat_actions);

    const std::string assistant_open =
        ApplyLangAttribute(R"(<div class="bubble bubble-assistant")", raw_output) + R"( selectable="text">)";

    if (messaging_ready_) {
      const std::string active_thread = thread_id.empty() ? MessagingHub::Instance().Inbox().ActiveThreadId() : thread_id;
      auto messages = MessagingHub::Instance().Store().GetMessagesPage(active_thread, std::nullopt, 10000);
      bool updated = false;
      if (messages) {
        for (ThreadMessage& message : *messages) {
          if (message.id == entry_id) {
            message.content_rml = assistant_open + hydrated + "</div>";
            message.chat_actions = chat_actions;
            (void)MessagingHub::Instance().Store().UpdateMessage(message);
            updated = true;
            break;
          }
        }
      }
      if (!updated) {
        ThreadMessage ai_message;
        ai_message.id = util::GenerateUuid();
        ai_message.thread_id = active_thread;
        ai_message.sender_contact_id = kAiAssistantContactId;
        ai_message.text = raw_output;
        ai_message.content_rml = assistant_open + hydrated + "</div>";
        ai_message.chat_actions = chat_actions;
        ai_message.timestamp = util::NowUnixMs();
        ai_message.transport = MessageTransport::Local;
        (void)MessagingHub::Instance().Store().AppendMessage(ai_message);
      }
      (void)MessagingHub::Instance().Inbox().UpdatePreview(active_thread, parsed.rml);
    }

    working_set_.ApplyFromParse(entry_id, parsed.working_set_candidates);

    if (shared_ai_mode == AtAiMode::SharedReply || shared_ai_mode == AtAiMode::SharedFull) {
      const std::string active_thread =
          thread_id.empty() ? MessagingHub::Instance().Inbox().ActiveThreadId() : thread_id;
      std::string relay_plain = raw_output;
      if (StructuredTextParser::IsBlocksJsonDocument(raw_output)) {
        try {
          const nlohmann::json blocks_doc = nlohmann::json::parse(raw_output);
          if (blocks_doc.contains("blocks") && blocks_doc["blocks"].is_array()) {
            std::string joined;
            for (const auto& block : blocks_doc["blocks"]) {
              if (block.value("type", "") == "paragraph" && block.contains("text") &&
                  block["text"].is_string()) {
                if (!joined.empty()) {
                  joined += "\n";
                }
                joined += block["text"].get<std::string>();
              }
            }
            if (!joined.empty()) {
              relay_plain = joined;
            }
          }
        } catch (const std::exception&) {
        }
      }
      SendSharedAssistantRelay(active_thread, shared_ai_mode, relay_plain);
    }
  }

  SyncDisplayFromThread();
  SyncShellSessions();
  chat_.loading = false;
  chat_.status = "";
  ShellHost::Instance().SetActivity(false);
  DirtyChat();
  DirtyShell();
}

void ChatController::HandleAgentEvent(const AgentEvent& event) {
  switch (event.type) {
  case AgentEventType::LoadingChanged:
    chat_.loading = event.loading;
    if (!event.loading) {
      chat_.status = "";
      ShellHost::Instance().SetActivity(false);
    } else {
      ShellHost::Instance().SetActivity(true);
      if (messaging_ready_) {
        SyncDisplayFromThread();
        DirtyChatTurns();
      }
    }
    DirtyChatChrome();
    break;
  case AgentEventType::ToolActivity:
    chat_.status = Rml::String(ToolActivityLabel(event.tool_name, event.status).c_str());
    ShellHost::Instance().SetActivity(true, chat_.status);
    DirtyChatChrome();
    break;
  case AgentEventType::AssistantReady:
    FinishAssistantReply(event.entry_id, event.text, !StructuredTextParser::IsBlocksJsonDocument(event.text),
                         event.finish_reason, event.thread_id, event.response_goal, event.render_mode,
                         event.shared_ai_mode);
    break;
  case AgentEventType::Error:
    log().error << "Agent session error: " << event.message;
    UserFeedback::NeedsSetup(event.message);
    if (agent_ && !agent_->conversation().Entries().empty()) {
      const TranscriptEntry& entry = agent_->conversation().Entries().back();
      agent_->CompleteAssistantMessage(entry.id, event.message);
      agent_->SetAssistantDisplay(entry.id, event.message, {});
      SyncDisplayFromThread();
      DirtyChatTurns();
    }
    chat_.loading = false;
    chat_.status = "";
    ShellHost::Instance().SetActivity(false);
    DirtyChatChrome();
    break;
  }
}

void ChatController::WithSecrets(std::function<void()> action) {
  if (PinGateController::Instance().IsUnlockInProgress()) {
    ShellFeedback::ShowToast(ShellHost::Instance().State(), Tr("startup.still_preparing"));
    ShellHost::Instance().DirtyWindow();
  }
  PinGateController::Instance().EnsureUnlocked(
      [this, action = std::move(action)](const bool unlocked) {
        if (!unlocked) {
          if (!PinGateController::Instance().IsUnlockInProgress()) {
            ShellFeedback::ShowToast(ShellHost::Instance().State(), "PIN required to continue");
            ShellHost::Instance().DirtyWindow();
          }
          return;
        }
        if (!messaging_ready_) {
          WireMessagingBindings();
        }
        if (action) {
          action();
        }
      });
}

void ChatController::RefreshLlmSetupBanner() {
  const AppConfig& config = SessionStore::Instance().Snapshot().config;
  use_llm_ = !config.llm.base_url.empty();
  constexpr const char* kRegisterBrief =
      "Register your identity in Me → Profile to use Brief assistant (or switch to Cloud/Ollama).";

  if (!use_llm_) {
    UserFeedback::NeedsSetup("Using mock replies — LLM is not configured.");
    return;
  }
  if (ResolvePreset(config) == "brief") {
    std::string brief_key;
    if (MessagingHub::Instance().IsMessagingReady()) {
      if (auto identity = MessagingHub::Instance().Identity().Get()) {
        brief_key = identity->brief_llm_api_key;
      }
    }
    if (brief_key.empty()) {
      UserFeedback::NeedsSetup(kRegisterBrief);
      return;
    }
    auto& shell = ShellHost::Instance().State();
    if (shell.banner_message == kRegisterBrief) {
      ShellFeedback::DismissBanner(shell);
      ShellHost::Instance().DirtyWindow();
    }
    return;
  }
  if (config.llm.require_api_key && config.llm.api_key.empty()) {
    UserFeedback::NeedsSetup("Add your API key in Me → Assistant to enable the assistant.");
  }
}

void ChatController::WireMessagingBindings() {
  if (!MessagingHub::Instance().IsInitialized() || !agent_) {
    return;
  }
  // Identity / Brief key / push registration are only valid after vault unlock.
  if (!MessagingHub::Instance().IsMessagingReady()) {
    return;
  }
  messaging_ready_ = true;
  agent_->SetThreadStore(&MessagingHub::Instance().Store());
  MessagingHub::Instance().BindAgent(*agent_);
  chat_.compose_disabled = false;
  DirtyChatChrome();
  MessagingHub::Instance().P2p().SetOnMessagesChanged([this]() {
    RefreshFromMessaging();
    ContactsController::Instance().Refresh();
  });
  MessagingHub::Instance().P2p().SetOnDeliveryNotice([this](const std::string& message) {
    ShellFeedback::ShowToast(ShellHost::Instance().State(), message);
    ShellHost::Instance().DirtyWindow();
  });
  MessagingHub::Instance().P2p().SetOnBackgroundUnread(
      [](std::string title, std::string body, std::string thread_id) {
        if (!SessionStore::Instance().Snapshot().profile_prefs.show_notifications) {
          return;
        }
        ILocalNotifier::Instance().NotifyIncoming(title, body, thread_id);
      });
  ILocalNotifier::Instance().SetActivationHandler([](std::string thread_id) {
    DesktopWindowChrome::RaiseAndFocus();
    if (!thread_id.empty() && ChatSessionActions::Instance().select_thread) {
      ChatSessionActions::Instance().select_thread(thread_id);
    }
  });
  BackgroundSyncScheduler::Instance().SetSyncHandler([](bool force) {
    if (!MessagingHub::Instance().IsMessagingReady()) {
      return;
    }
    MessagingHub::Instance().P2p().SyncInboxFromWake(force);
  });
  IPushDeviceRegistrar::SetTokenChangedHandler([](const std::string& /*token*/) {
    BrowserThread::PostTask(BrowserThreadId::UI, []() {
      if (!MessagingHub::Instance().IsMessagingReady()) {
        return;
      }
      (void)PushDeviceCoordinator::SyncWithPreference(
          SessionStore::Instance().Snapshot().profile_prefs.show_notifications);
    });
  });
  (void)PushDeviceCoordinator::SyncWithPreference(
      SessionStore::Instance().Snapshot().profile_prefs.show_notifications);
  MessagingHub::Instance().Inbox().SetOnThreadChanged([this]() {
    RefreshFromMessaging();
    ContactsController::Instance().Refresh();
  });
  if (MessagingHub::Instance().HasRouter()) {
    MessagingHub::Instance().Router().SetOnLocalAction(
        [this](const std::string& message, const std::optional<std::string>& payload) {
          HandleLocalAction(message, payload);
        });
    MessagingHub::Instance().Router().SetSharedAiConfirmCallback(
        [this](const std::string& thread_id, const AtAiMode mode, const std::string& prompt,
               std::function<void(bool confirmed, bool dont_ask_again)> done) {
          const char* mode_label = mode == AtAiMode::SharedFull ? "share the prompt and reply" : "share the AI reply";
          ShellFeedback::ShowConfirmWithCheckbox(
              ShellHost::Instance().State(), "Share with peer?",
              std::string("This will ") + mode_label +
                  " on the encrypted thread. Your local @ai assist stays private.",
              "Don't ask again for this conversation", false,
              [this, thread_id, done = std::move(done)](const bool ok, const bool dont_ask) {
                if (ok && dont_ask) {
                  MessagingHub::Instance().Router().MarkSharedAiConfirmed(thread_id);
                }
                done(ok, dont_ask);
              });
        });
  }
  MessagingHub::Instance().Actions().SetOnActionMessage([this](const std::string& message) {
    ShellFeedback::ShowToast(ShellHost::Instance().State(), message);
    ShellHost::Instance().DirtyWindow();
  });
  RefreshFromMessaging();
  MessagingHub::Instance().P2p().TailSyncActiveE2eThread();

  const bool auto_renew = SessionStore::Instance().Snapshot().profile_prefs.auto_renew_registration;
  auto renew = MaybeAutoRenewRegistration(MessagingHub::Instance().Registration(),
                                          MessagingHub::Instance().Identity(), auto_renew);
  if (!renew) {
    log().warning << "Auto-renew registration failed: " << renew.error().message;
  } else if (*renew) {
    log().info << "Network registration auto-renewed";
  } else {
    auto identity = MessagingHub::Instance().Identity().Get();
    if (identity && ShouldRenewRegistration(*identity) && !auto_renew) {
      UserFeedback::NeedsSetup("Network registration expires soon — renew in Me → Profile");
    }
  }
  // Always reload so Brief key from identity is applied after unlock (not only on renew).
  ReloadAgentConfig();
  RefreshLlmSetupBanner();
}

bool ChatController::Setup(Rml::Context* context) {
  StartupPhase setup_phase("ChatController::Setup");
  if (!context) {
    return false;
  }

  context_ = context;
  AppLifecycle::AddBackgroundListener([this]() { OnApplicationPause(); });
  AppLifecycle::AddForegroundListener([this]() {
    if (!messaging_ready_) {
      return;
    }
    const std::string active = MessagingHub::Instance().Inbox().ActiveThreadId();
    if (!active.empty() && MessagingHub::Instance().IsMessagingReady()) {
      MessagingHub::Instance().P2p().WarmPeerForThread(active);
    }
  });
  const AppConfig& config = SessionStore::Instance().Snapshot().config;
  widgets_.ClearAll();
  chat_ = {};
  shell_ = {};
  shell_.sessions = {{Rml::String("Chat"), Rml::String("Ask anything...")}};
  pending_reply_.reset();
  use_llm_ = !config.llm.base_url.empty();
  agent_.emplace();
  StartupMark("chat_after_agent_emplace");

  ChatSessionActions& actions = ChatSessionActions::Instance();
  actions.reload_agent_config = [this]() { ReloadAgentConfig(); };
  actions.finalize_thread_display = [this]() { FinalizeThreadDisplay(); };
  actions.select_thread = [this](const std::string& thread_id) { OnSelectThread(thread_id); };
  actions.on_find_someone = [this]() { OnFindSomeone(); };
  actions.on_profile_data_reset = [this]() { OnProfileDataReset(); };

  if (MessagingHub::Instance().IsInitialized()) {
    WireMessagingBindings();
    MessagingHub::Instance().SetOnMessagingReady([this]() {
      WireMessagingBindings();
      ContactsController::Instance().Refresh();
      if (ShellHost::Instance().State().account_sheet_open) {
        SettingsController::Instance().OnAccountSheetOpened();
      } else if (ShellHost::Instance().State().nav_tab == NavTab::Me) {
        SettingsController::Instance().OnNavTabActivated();
      } else if (ShellHost::Instance().State().nav_tab == NavTab::Home) {
        OnHomeTabActivated();
      }
    });
  }

  // Must go through ApplyRuntimeConfig so Brief injects identity.brief_llm_api_key.
  // A raw Configure(SessionStore config) wipes the vault key after WireMessagingBindings.
  ApplyRuntimeConfig(config);
  log().info << "Chat initialized (model: " << config.llm.model << ")";

  DataModelHost::Instance().Clear();

  const auto register_enter_send = [this](Rml::Input::KeyIdentifier key) {
    InputCoordinator::Instance().Register(KeyBinding{
        .key = key,
        .forbidden_modifiers = Rml::Input::KM_SHIFT,
        .when = [](Rml::Context* ctx) {
          Rml::Element* focus = ctx ? ctx->GetFocusElement() : nullptr;
          return focus && focus->GetId() == "draft-input";
        },
        .action = [this]() {
          OnSendMessage();
          return false;
        },
        .priority = 50,
    });
  };
  register_enter_send(Rml::Input::KI_RETURN);
  register_enter_send(Rml::Input::KI_NUMPADENTER);

  if (!DataModelHost::Instance().Register(context, "chat", [](Rml::DataModelConstructor& ctor) {
        RegisterChatWidgetDataTypes(ctor);
        ctor.Bind("draft", &ChatController::Instance().chat_.draft);
        ctor.Bind("draft_placeholder", &ChatController::Instance().chat_.draft_placeholder);
        ctor.Bind("status", &ChatController::Instance().chat_.status);
        ctor.Bind("loading", &ChatController::Instance().chat_.loading);
        ctor.Bind("has_turns", &ChatController::Instance().chat_.has_turns);
        ctor.Bind("turns", &ChatController::Instance().chat_.turns);
        ctor.Bind("messages", &ChatController::Instance().chat_.messages);
        ctor.Bind("use_messages_layout", &ChatController::Instance().chat_.use_messages_layout);
        ctor.Bind("thread_title", &ChatController::Instance().chat_.thread_title);
        ctor.Bind("thread_subtitle", &ChatController::Instance().chat_.thread_subtitle);
        ctor.Bind("peer_link_status", &ChatController::Instance().chat_.peer_link_status);
        ctor.Bind("peer_link_banner", &ChatController::Instance().chat_.peer_link_banner);
        ctor.Bind("show_peer_link", &ChatController::Instance().chat_.show_peer_link);
        ctor.Bind("show_peer_link_banner", &ChatController::Instance().chat_.show_peer_link_banner);
        ctor.Bind("show_retry_peer_dial", &ChatController::Instance().chat_.show_retry_peer_dial);
        ctor.Bind("thread_encrypted", &ChatController::Instance().chat_.thread_encrypted);
        ctor.Bind("thread_is_ai", &ChatController::Instance().chat_.thread_is_ai);
        ctor.Bind("thread_is_private", &ChatController::Instance().chat_.thread_is_private);
        ctor.Bind("thread_is_public", &ChatController::Instance().chat_.thread_is_public);
        ctor.Bind("thread_is_group", &ChatController::Instance().chat_.thread_is_group);
        ctor.Bind("compose_disabled", &ChatController::Instance().chat_.compose_disabled);
        ctor.Bind("show_thread_actions", &ChatController::Instance().chat_.show_thread_actions);
        ctor.Bind("show_peer_sheet", &ChatController::Instance().chat_.show_peer_sheet);
        ctor.Bind("show_forget_memory", &ChatController::Instance().chat_.show_forget_memory);
        ctor.Bind("show_sync_with_peer", &ChatController::Instance().chat_.show_sync_with_peer);
        ctor.Bind("show_thread_menu", &ChatController::Instance().chat_.show_thread_menu);
        ctor.Bind("show_gap_banner", &ChatController::Instance().chat_.show_gap_banner);
        ctor.Bind("show_compromised_banner", &ChatController::Instance().chat_.show_compromised_banner);
        ctor.Bind("show_psk_setup_banner", &ChatController::Instance().chat_.show_psk_setup_banner);
        ctor.Bind("show_psk_import", &ChatController::Instance().chat_.show_psk_import);
        ctor.Bind("psk_has_key", &ChatController::Instance().chat_.psk_has_key);
        ctor.Bind("psk_verified", &ChatController::Instance().chat_.psk_verified);
        ctor.Bind("psk_fingerprint", &ChatController::Instance().chat_.psk_fingerprint);
        ctor.Bind("psk_export_b64", &ChatController::Instance().chat_.psk_export_b64);
        ctor.Bind("psk_import_text", &ChatController::Instance().chat_.psk_import_text);
        ctor.Bind("sync_in_progress", &ChatController::Instance().chat_.sync_in_progress);
        ctor.Bind("show_older_history_hint", &ChatController::Instance().chat_.show_older_history_hint);
        ctor.Bind("show_jump_to_latest", &ChatController::Instance().chat_.show_jump_to_latest);
        ctor.Bind("jump_to_latest_label", &ChatController::Instance().chat_.jump_to_latest_label);
        ctor.BindEventCallback("send_message", &ChatController::SendMessageCallback);
        ctor.BindEventCallback("send_suggestion", &ChatController::SendSuggestionCallback);
        ctor.BindEventCallback("send_chat_action", &ChatController::SendChatActionCallback);
        ctor.BindEventCallback("submit_form", &ChatController::SubmitFormCallback);
        ctor.BindEventCallback("calendar_prev", &ChatController::CalendarPrevCallback);
        ctor.BindEventCallback("calendar_next", &ChatController::CalendarNextCallback);
        ctor.BindEventCallback("select_calendar_day", &ChatController::SelectCalendarDayCallback);
        ctor.BindEventCallback("open_working_set", &ChatController::OpenWorkingSetCallback);
        ctor.BindEventCallback("clear_history", &ChatController::ClearHistoryCallback);
        ctor.BindEventCallback("forget_memory", &ChatController::ForgetMemoryCallback);
        ctor.BindEventCallback("open_thread_actions_menu", &ChatController::OpenThreadActionsMenuCallback);
        ctor.BindEventCallback("open_peer_sheet", &ChatController::OpenPeerSheetCallback);
        ctor.BindEventCallback("sync_with_peer", &ChatController::SyncWithPeerCallback);
        ctor.BindEventCallback("retry_gap_sync", &ChatController::RetryGapSyncCallback);
        ctor.BindEventCallback("start_new_secure_chat", &ChatController::StartNewSecureChatCallback);
        ctor.BindEventCallback("pause_integrity_only", &ChatController::PauseIntegrityCallback);
        ctor.BindEventCallback("copy_psk_key", &ChatController::CopyPskKeyCallback);
        ctor.BindEventCallback("toggle_psk_import", &ChatController::TogglePskImportCallback);
        ctor.BindEventCallback("import_psk", &ChatController::ImportPskCallback);
        ctor.BindEventCallback("verify_psk", &ChatController::VerifyPskCallback);
        ctor.BindEventCallback("rotate_psk_export", &ChatController::RotatePskExportCallback);
        ctor.BindEventCallback("load_older_history", &ChatController::LoadOlderHistoryCallback);
        ctor.BindEventCallback("retry_peer_dial", &ChatController::RetryPeerDialCallback);
        ctor.BindEventCallback("messages_scroll", &ChatController::MessagesScrollCallback);
        ctor.BindEventCallback("jump_to_latest", &ChatController::JumpToLatestCallback);
        ctor.BindEventCallback("new_message", &ChatController::NewMessageCallback);
      })) {
    return false;
  }

  if (!DataModelHost::Instance().Register(context, "shell", [](Rml::DataModelConstructor& ctor) {
        RegisterChatWidgetDataTypes(ctor);
        if (auto working_set_handle = ctor.RegisterStruct<TurnWidgetState>()) {
          working_set_handle.RegisterMember("has_form", &TurnWidgetState::has_form);
          working_set_handle.RegisterMember("form", &TurnWidgetState::form);
          working_set_handle.RegisterMember("has_calendar", &TurnWidgetState::has_calendar);
          working_set_handle.RegisterMember("calendar", &TurnWidgetState::calendar);
        }
        if (auto session_handle = ctor.RegisterStruct<ChatController::SessionRow>()) {
          session_handle.RegisterMember("id", &ChatController::SessionRow::id);
          session_handle.RegisterMember("title", &ChatController::SessionRow::title);
          session_handle.RegisterMember("preview", &ChatController::SessionRow::preview);
          session_handle.RegisterMember("kind", &ChatController::SessionRow::kind);
          session_handle.RegisterMember("unread_count", &ChatController::SessionRow::unread_count);
          session_handle.RegisterMember("unread_display", &ChatController::SessionRow::unread_display);
          session_handle.RegisterMember("active", &ChatController::SessionRow::active);
          session_handle.RegisterMember("closable", &ChatController::SessionRow::closable);
        }
        ctor.RegisterArray<std::vector<ChatController::SessionRow>>();
        ctor.Bind("sessions", &ChatController::Instance().shell_.sessions);
        ctor.Bind("working_set_active", &ChatController::Instance().shell_.working_set_active);
        ctor.Bind("working_set_title", &ChatController::Instance().shell_.working_set_title);
        ctor.Bind("working_set_subtitle", &ChatController::Instance().shell_.working_set_subtitle);
        ctor.Bind("working_set_rml", &ChatController::Instance().shell_.working_set_rml);
        ctor.Bind("working_set", &ChatController::Instance().shell_.working_set);
        ctor.BindEventCallback("new_chat", &ChatController::NewChatCallback);
        ctor.BindEventCallback("new_message", &ChatController::NewMessageCallback);
        ctor.BindEventCallback("open_new_session_menu", &ChatController::OpenNewSessionMenuCallback);
        ctor.BindEventCallback("select_thread", &ChatController::SelectThreadCallback);
        ctor.BindEventCallback("close_thread", &ChatController::CloseThreadCallback);
        ctor.BindEventCallback("send_chat_action", &ChatController::SendChatActionCallback);
        ctor.BindEventCallback("submit_form", &ChatController::SubmitFormCallback);
        ctor.BindEventCallback("calendar_prev", &ChatController::CalendarPrevCallback);
        ctor.BindEventCallback("calendar_next", &ChatController::CalendarNextCallback);
        ctor.BindEventCallback("select_calendar_day", &ChatController::SelectCalendarDayCallback);
        ctor.BindEventCallback("open_working_set", &ChatController::OpenWorkingSetCallback);
      })) {
    return false;
  }

  if (!ShellHost::RegisterWindowModel(context)) {
    return false;
  }

  if (!SettingsController::Instance().RegisterModel(context)) {
    return false;
  }

  if (!ContactsController::Instance().RegisterModel(context)) {
    return false;
  }

  if (!PeoplePickerController::Instance().RegisterModel(context)) {
    return false;
  }

  SessionStore::Instance().AddConfigListener([this](const AppConfig& updated) { ApplyRuntimeConfig(updated); });

  ShellHost::Instance().Initialize(context);
  // After Initialize clears state: Latin UI is ready; CJK waits on deferred faces.
  ShellHost::Instance().State().fonts_ready = !UiLanguageNeedsCjkFonts();
  ShellHost::Instance().SetOnNavTabChanged([](NavTab tab) {
    if (tab == NavTab::Home) {
      ChatController::Instance().OnHomeTabActivated();
    }
    if (tab == NavTab::Sessions) {
      ChatController::Instance().OnSessionsTabActivated();
    }
    if (tab == NavTab::Contacts) {
      ContactsController::Instance().OnNavTabActivated();
    }
    if (tab == NavTab::Me) {
      SettingsController::Instance().OnNavTabActivated();
    }
    // Active-thread deduction for sessions badge depends on nav_tab.
    BadgeAggregator::Instance().Refresh();
    ShellHost::Instance().DirtyWindow();
  });
  ShellHost::Instance().SetOnLayoutModeChanged([](LayoutMode mode) {
    if (ShellHost::Instance().State().nav_tab == NavTab::Contacts) {
      ContactsController::Instance().SyncLayoutMode();
    }
    SettingsController::Instance().SyncLayoutMode();
    if (mode == LayoutMode::Compact && ShellHost::Instance().State().nav_tab == NavTab::Home) {
      ChatController::Instance().OnHomeTabActivated();
    }
  });
  ShellHost::Instance().SetOnLayoutSynced([]() {
    SettingsController::Instance().OnShellLayoutSynced();
    ChatController::Instance().OnShellLayoutSynced();
  });
  ShellHost::Instance().RegisterPane(
      {.key = "sidebar", .rml_path = "views/sidebar.rml", .role = PaneRole::Secondary});
  ShellHost::Instance().RegisterPane(
      {.key = "contacts", .rml_path = "views/contacts.rml", .role = PaneRole::Secondary});
  ShellHost::Instance().RegisterPane(
      {.key = "settings", .rml_path = "views/settings.rml", .role = PaneRole::Secondary});
  ShellHost::Instance().RegisterPane(
      {.key = "home", .rml_path = "views/home.rml", .role = PaneRole::Primary});
  ShellHost::Instance().RegisterPane({.key = "chat",
                                       .rml_path = "views/chat.rml",
                                       .role = PaneRole::Primary,
                                       .provides_composer = true});
  ShellHost::Instance().RegisterPane(
      {.key = "contact_detail", .rml_path = "views/contact_detail.rml", .role = PaneRole::Primary});
  ShellHost::Instance().RegisterPane(
      {.key = "settings_detail", .rml_path = "views/settings_detail.rml", .role = PaneRole::Primary});
  ShellHost::Instance().RegisterPane(
      {.key = "preview", .rml_path = "views/preview.rml", .role = PaneRole::Auxiliary, .toolbar_label = "Preview"});

  if (DocumentLoader::LoadFile(context, IAssetLocator::Instance().Resolve("samples/window_shell.rml")) == nullptr) {
    return false;
  }
  StartupMark("chat_after_window_shell");

  ShellHost::Instance().Update(context);
  {
    StartupPhase phase("ShellHost::SyncLayout");
    ShellHost::Instance().SyncLayout();
  }

  // Vault unlock + deferred fonts run after first present (DeferredStartup).
  chat_.compose_disabled = !MessagingHub::Instance().IsMessagingReady();
  DirtyChatChrome();

  if (messaging_ready_) {
    OnHomeTabActivated();
  }

  // Brief key lives in the vault — refresh after unlock via WireMessagingBindings.
  // Non-Brief setup can be checked immediately (config-only).
  if (ResolvePreset(config) != "brief") {
    RefreshLlmSetupBanner();
  } else if (messaging_ready_) {
    RefreshLlmSetupBanner();
  }

  return true;
}

void ChatController::ApplyRuntimeConfig(const AppConfig& config) {
  AppConfig runtime = config;
  use_llm_ = !runtime.llm.base_url.empty();
  if (!agent_) {
    return;
  }
  if (ResolvePreset(runtime) == "brief") {
    runtime.llm.require_api_key = true;
    if (MessagingHub::Instance().IsInitialized()) {
      if (auto identity = MessagingHub::Instance().Identity().Get()) {
        runtime.llm.api_key = identity->brief_llm_api_key;
      }
    }
  }
  agent_->SetToolRegistrationHook([](ToolRegistry& tools) {
    if (MessagingHub::Instance().IsInitialized()) {
      RegisterMessagingTools(tools, MessagingHub::Instance());
    }
  });
  agent_->Configure(runtime);
  if (MessagingHub::Instance().IsInitialized()) {
    const auto& bootstrap = SessionStore::Instance().Snapshot();
    (void)MessagingHub::Instance().Reinitialize(runtime, bootstrap.profile_data_dir);
  }
}

void ChatController::ReloadAgentConfig() {
  ApplyRuntimeConfig(SessionStore::Instance().Snapshot().config);
}

void ChatController::OnApplicationPause() {
  if (agent_) {
    agent_->Cancel();
  }
  if (messaging_ready_) {
    MessagingHub::Instance().SuspendLibp2pColdPeers();
  }
}

void ChatController::Update() {
  if (pending_reply_) {
    PendingReply reply = std::move(*pending_reply_);
    pending_reply_.reset();
    FinishAssistantReply(reply.entry_id, reply.output, reply.from_llm, {}, reply.thread_id);
  }

  if (messaging_ready_) {
    MessagingHub::Instance().TickLibp2p();
    if (MessagingHub::Instance().IsMessagingReady()) {
      BackgroundSyncScheduler::Instance().Tick();
      const auto now = std::chrono::steady_clock::now();
      if (chrome_.MaybePollPeerLink(now)) {
        DirtyChatHeader();
      }
    }
  }

  if (agent_) {
    std::vector<AgentEvent> events;
    agent_->PollEvents(events);
    for (const AgentEvent& event : events) {
      HandleAgentEvent(event);
    }
  }
}

void ChatController::AfterLayout() {
  scroller_.ApplyPolicy();
}

void ChatController::Shutdown() {
  AppLifecycle::ClearBackgroundListeners();
  AppLifecycle::ClearForegroundListeners();
  IPushDeviceRegistrar::SetTokenChangedHandler(nullptr);
  if (agent_) {
    StartupPhase phase("Shutdown::AgentSession");
    agent_->Cancel();
    agent_.reset();
  }
  if (messaging_ready_) {
    {
      StartupPhase phase("Shutdown::MessagingHub");
      MessagingHub::Instance().Shutdown();
    }
    {
      StartupPhase phase("Shutdown::ProfileSecrets");
      ProfileSecretsService::Instance().Shutdown();
    }
    messaging_ready_ = false;
  }
  pending_reply_.reset();
  context_ = nullptr;
  widgets_.ClearAll();
  chat_ = {};
  shell_ = {};
  use_llm_ = false;
}

bool SetupChatController(Rml::Context* context) {
  return ChatController::Instance().Setup(context);
}

void UpdateChatController() {
  ChatController::Instance().Update();
}

void AfterLayoutChatController() {
  ChatController::Instance().AfterLayout();
}

void ShutdownChatController() {
  ChatController::Instance().Shutdown();
}

} // namespace pbr
