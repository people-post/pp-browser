// The people picker's "choose and return" mode (DEV/PLAN-message-forward-text.md, step 3): the "Forward to" list is the
// presenter's (ChatPresenter::ForwardTargets) shown by the real PeoplePickerController, whose ports are fakes. The
// picker's other uses (a free pick, a chat with someone) are checked to behave as before.
#include "gui/contacts/PeoplePickerController.h"

#include "feature/presenters/ChatPresenter.h"
#include "feature/presenters/MessageForward.h"

#include <gtest/gtest.h>

#include <string>
#include <vector>

namespace pbr {

/** The picker's private state, read and driven the way a tap would. */
class PeoplePickerControllerTestAccess {
public:
  struct Row {
    std::string id;
    std::string title;
    bool selected;
    bool disabled;
  };
  static std::vector<Row> Rows(const PeoplePickerController& picker) {
    std::vector<Row> rows;
    for (const PeoplePickerController::PickerRow& row : picker.rows_) {
      rows.push_back({row.id.c_str(), row.title.c_str(), row.selected, row.disabled});
    }
    return rows;
  }
  static std::vector<std::string> Ids(const PeoplePickerController& picker) {
    std::vector<std::string> ids;
    for (const Row& row : Rows(picker)) {
      ids.push_back(row.id);
    }
    return ids;
  }
  static void Tap(PeoplePickerController& picker, const std::string& id) { picker.OnToggleContact(id); }
  static void Confirm(PeoplePickerController& picker) { picker.OnConfirm(); }
  static void Cancel(PeoplePickerController& picker) { picker.OnCancel(); }
  static bool ConfirmEnabled(const PeoplePickerController& picker) { return picker.cta_enabled_; }
  static std::string ConfirmLabel(const PeoplePickerController& picker) { return picker.cta_label_.c_str(); }
  static std::string Title(const PeoplePickerController& picker) { return picker.title_.c_str(); }
  static std::string Preview(const PeoplePickerController& picker) {
    return picker.show_preview_ ? picker.preview_text_.c_str() : "";
  }
  static void Search(PeoplePickerController& picker, const std::string& query) {
    picker.search_query_ = query.c_str();
    picker.OnSearchChanged();
  }
};

namespace {

using Access = PeoplePickerControllerTestAccess;

ForwardTargetFacts Facts() {
  ForwardTargetFacts facts;
  facts.saved_contact = true;
  facts.my_account = "acct-me";
  facts.support_account = "acct-support";
  return facts;
}

ForwardCandidate Person(const std::string& thread, const std::string& contact, const std::string& title,
                        const std::string& peer, const int64_t updated_at) {
  ForwardCandidate candidate;
  candidate.thread.id = thread;
  candidate.thread.kind = ThreadKind::Direct;
  candidate.thread.peer_identity_value = peer;
  candidate.thread.updated_at = updated_at;
  candidate.contact_id = contact;
  candidate.title = title;
  candidate.facts = Facts();
  return candidate;
}

ForwardCandidate GroupChat(const std::string& thread, const std::string& title, const bool in_group,
                           const int64_t updated_at) {
  ForwardCandidate candidate;
  candidate.thread.id = thread;
  candidate.thread.kind = ThreadKind::Group;
  candidate.thread.group_id = "g-" + thread;
  candidate.thread.updated_at = updated_at;
  candidate.title = title;
  candidate.facts.in_group = in_group;
  return candidate;
}

class PeoplePickerChooseTest : public ::testing::Test {
protected:
  PeoplePickerChooseTest() {
    MessagingContactsPorts contacts;
    contacts.snapshot = []() {
      MessagingView view;
      view.initialized = true;
      view.messaging_ready = false;
      return view;
    };
    contacts.find_or_create_direct_thread = [this](const std::string&, ThreadChannel) {
      ++direct_thread_calls;
      return Roe<Thread>::error(Error("not in this test"));
    };
    picker.BindContactsPorts(contacts);
    MessagingPeoplePickerPorts picker_ports;
    picker_ports.create_group = [this](const std::string&, const std::vector<std::string>&) {
      ++group_calls;
      return Roe<Thread>::error(Error("not in this test"));
    };
    picker_ports.invite_members_async = [this](const std::string&, std::vector<std::string>,
                                               std::function<void(GroupInviteBatchResult)>) { ++invite_calls; };
    picker.BindPickerPorts(picker_ports);
    ShellNavigationPorts navigation;
    navigation.push_layer = [this](const PaneSpec& spec) {
      layers.push_back(spec.key);
      ++open_layers;
      return ++next_layer;
    };
    navigation.close_layer = [this](int) { --open_layers; };
    picker.BindShellNavigation(navigation);
    ShellFeedbackPorts feedback;
    feedback.show_toast = [this](const std::string& message, ToastDuration) { pills.push_back(message); };
    picker.BindShellFeedback(feedback);

    presenter.SetForwardPortsForTest(Ports());
  }

  ForwardPorts Ports() {
    ForwardPorts ports;
    ports.candidates = [this]() { return candidates; };
    ports.target_of = [this](const std::string& thread) {
      for (const ForwardCandidate& candidate : candidates) {
        if (candidate.thread.id == thread) {
          return ClassifyForwardTarget(candidate.thread, candidate.facts);
        }
      }
      return ForwardTargetKind::Unavailable;
    };
    return ports;
  }

  PeoplePickerChooseOptions Options() {
    PeoplePickerChooseOptions options;
    options.title = "Forward to";
    options.send_label = "Send";
    options.preview = "hello there";
    options.limit_message = "Up to 3 chats";
    options.max_targets = kMaxForwardTargets;
    options.list = [this]() { return presenter.ForwardTargets(); };
    return options;
  }

  void Open() {
    picker.OpenForResult(
        Options(), [this](std::vector<ForwardTarget> chosen) { returned = std::move(chosen); },
        [this]() { ++cancelled; });
  }

  std::vector<std::string> ChosenKeys() const {
    std::vector<std::string> keys;
    for (const ForwardTarget& target : returned) {
      keys.push_back(target.Key());
    }
    return keys;
  }

  PeoplePickerController picker;
  ChatPresenter presenter;
  std::vector<ForwardCandidate> candidates;
  std::vector<ForwardTarget> returned;
  std::vector<std::string> pills;
  std::vector<std::string> layers;
  int open_layers = 0;
  int next_layer = 0;
  int cancelled = 0;
  int direct_thread_calls = 0;
  int group_calls = 0;
  int invite_calls = 0;
  std::string last_direct;
};

TEST_F(PeoplePickerChooseTest, ThePickerInChooseModeReturnsTargetsAndRunsNoInvite) {
  candidates = {Person("t-bob", "bob", "Bob", "acct-bob", 30), Person("t-cy", "cy", "Cy", "acct-cy", 20),
                GroupChat("t-club", "Club", true, 10)};
  Open();
  EXPECT_EQ(open_layers, 1);
  EXPECT_EQ(Access::Title(picker), "Forward to");
  EXPECT_EQ(Access::Preview(picker), "hello there");
  EXPECT_FALSE(Access::ConfirmEnabled(picker)) << "nothing ticked yet";
  EXPECT_EQ(Access::ConfirmLabel(picker), "Send") << "the same word, greyed";

  Access::Tap(picker, "t-club");
  Access::Tap(picker, "t-bob");
  EXPECT_TRUE(Access::ConfirmEnabled(picker));
  Access::Confirm(picker);

  EXPECT_EQ(open_layers, 0) << "the picker closes, then hands the choice back";
  EXPECT_EQ(ChosenKeys(), (std::vector<std::string>{"t-bob", "t-club"})) << "in the list's order";
  EXPECT_EQ(invite_calls, 0);
  EXPECT_EQ(group_calls, 0);
  EXPECT_EQ(direct_thread_calls, 0) << "nothing was opened, invited or created";
  EXPECT_EQ(cancelled, 0);
}

TEST_F(PeoplePickerChooseTest, APickerThatCannotOpenAnswersCancelAndOpensNothing) {
  MessagingContactsPorts down;
  down.snapshot = []() { return MessagingView{}; }; // messaging not initialized
  picker.BindContactsPorts(down);
  Open();
  EXPECT_EQ(open_layers, 0);
  EXPECT_EQ(cancelled, 1) << "the caller is told, or its Forward button stays dead";
  EXPECT_TRUE(returned.empty());
}

TEST_F(PeoplePickerChooseTest, ClosingItWithoutChoosingRunsCancelAndNotDone) {
  candidates = {Person("t-bob", "bob", "Bob", "acct-bob", 30)};
  Open();
  Access::Tap(picker, "t-bob");
  Access::Cancel(picker);
  EXPECT_EQ(open_layers, 0);
  EXPECT_EQ(cancelled, 1);
  EXPECT_TRUE(returned.empty());
}

TEST_F(PeoplePickerChooseTest, TheHostClosingItRunsNeitherDoneNorCancel) {
  candidates = {Person("t-bob", "bob", "Bob", "acct-bob", 30)};
  Open();
  Access::Tap(picker, "t-bob");
  picker.Close(); // a lock or a sign-out
  EXPECT_EQ(open_layers, 0);
  EXPECT_EQ(cancelled, 0);
  EXPECT_TRUE(returned.empty());
}

TEST_F(PeoplePickerChooseTest, ChooseModeListsRecentChatsThenContactsThenGroupsOnce) {
  for (int i = 0; i < 9; ++i) { // nine chats: the eight newest lead, the oldest is a person further down
    candidates.push_back(Person("t" + std::to_string(i), "c" + std::to_string(i), "P" + std::to_string(i),
                                "acct-" + std::to_string(i), 100 + i));
  }
  ForwardCandidate nochat = Person("", "c-new", "Zed", "acct-zed", 0); // a contact with no chat yet
  candidates.push_back(nochat);
  ForwardCandidate again = Person("", "c8", "P8 again", "acct-8", 0); // c8 has a chat: not listed twice
  candidates.push_back(again);
  candidates.push_back(GroupChat("g-old", "Old club", true, 5));
  Open();
  EXPECT_EQ(Access::Ids(picker),
            (std::vector<std::string>{"t8", "t7", "t6", "t5", "t4", "t3", "t2", "t1", "t0", "contact:c-new", "g-old"}));
}

TEST_F(PeoplePickerChooseTest, ChooseModeListsNoAiThreadNoSupportThreadAndNotMyself) {
  ForwardCandidate ai;
  ai.thread.id = "t-ai";
  ai.thread.kind = ThreadKind::Ai;
  ai.thread.updated_at = 99;
  ai.title = "New chat";
  candidates = {Person("t-bob", "bob", "Bob", "acct-bob", 1), ai,
                Person("t-support", "support", "PP support", "acct-support", 50),
                Person("t-me", "me", "Me", "acct-me", 60)};
  Open();
  EXPECT_EQ(Access::Ids(picker), (std::vector<std::string>{"t-bob"}));
}

TEST_F(PeoplePickerChooseTest, BlockedPeopleAndGroupsICannotWriteToAreNotListed) {
  ForwardCandidate blocked = Person("t-bad", "bad", "Bad", "acct-bad", 5);
  blocked.facts.blocked = true;
  candidates = {Person("t-bob", "bob", "Bob", "acct-bob", 1), blocked, GroupChat("t-left", "Left club", false, 7),
                GroupChat("t-club", "Club", true, 3)};
  ForwardCandidate ended = GroupChat("t-ended", "Ended club", true, 4);
  ended.facts.group_dissolved = true;
  candidates.push_back(ended);
  Open();
  EXPECT_EQ(Access::Ids(picker), (std::vector<std::string>{"t-club", "t-bob"}));
}

TEST_F(PeoplePickerChooseTest, TheSourceChatIsListedAndCanBeChosen) {
  candidates = {Person("t-src", "src", "The chat I am in", "acct-src", 100),
                Person("t-bob", "bob", "Bob", "acct-bob", 1)};
  Open();
  ASSERT_EQ(Access::Ids(picker).front(), "t-src");
  Access::Tap(picker, "t-src");
  Access::Confirm(picker);
  EXPECT_EQ(ChosenKeys(), (std::vector<std::string>{"t-src"}));
}

TEST_F(PeoplePickerChooseTest, TheFourthTargetCannotBeChosenAndThePillSaysWhy) {
  for (int i = 0; i < 4; ++i) {
    candidates.push_back(Person("t" + std::to_string(i), "c" + std::to_string(i), "P" + std::to_string(i),
                                "acct-" + std::to_string(i), 100 - i));
  }
  Open();
  for (int i = 0; i < 3; ++i) {
    Access::Tap(picker, "t" + std::to_string(i));
  }
  EXPECT_TRUE(pills.empty());
  const auto rows = Access::Rows(picker);
  ASSERT_EQ(rows.size(), 4u);
  EXPECT_TRUE(rows[3].disabled) << "the fourth is greyed";
  EXPECT_FALSE(rows[0].disabled);
  Access::Tap(picker, "t3");
  EXPECT_EQ(pills, (std::vector<std::string>{"Up to 3 chats"}));
  EXPECT_FALSE(Access::Rows(picker)[3].selected);
  Access::Tap(picker, "t0"); // one taken off, the fourth can be chosen
  EXPECT_FALSE(Access::Rows(picker)[3].disabled);
  Access::Tap(picker, "t3");
  Access::Confirm(picker);
  EXPECT_EQ(returned.size(), 3u);
}

TEST_F(PeoplePickerChooseTest, SearchNarrowsTheListAndKeepsWhatIsTicked) {
  candidates = {Person("t-bob", "bob", "Bob", "acct-bob", 3), Person("t-cy", "cy", "Cy", "acct-cy", 2)};
  Open();
  Access::Tap(picker, "t-bob");
  Access::Search(picker, "cy");
  EXPECT_EQ(Access::Ids(picker), (std::vector<std::string>{"t-cy"}));
  Access::Search(picker, "");
  EXPECT_TRUE(Access::Rows(picker)[0].selected);
  Access::Confirm(picker);
  EXPECT_EQ(ChosenKeys(), (std::vector<std::string>{"t-bob"}));
}

// The picker's other uses are as before: a free pick of one person still starts a chat with them, and lists contacts.
TEST_F(PeoplePickerChooseTest, TheFreePickStillListsContactsAndMessagesOne) {
  Contact contact;
  contact.id = "c-ann";
  contact.display_name = "Ann";
  contact.ids.push_back(mesh::ContactId{mesh::ContactIdKind::Account, "acct-ann", true});
  MessagingContactsPorts contacts;
  contacts.snapshot = []() {
    MessagingView view;
    view.initialized = true;
    return view;
  };
  contacts.list_contacts = [contact]() { return Roe<std::vector<Contact>>(std::vector<Contact>{contact}); };
  contacts.get_contact = [contact](const std::string&) {
    return Roe<std::optional<Contact>>(std::optional<Contact>(contact));
  };
  contacts.find_or_create_direct_thread = [this](const std::string& id, ThreadChannel) {
    ++direct_thread_calls;
    last_direct = id;
    return Roe<Thread>::error(Error("stop here"));
  };
  picker.BindContactsPorts(contacts);
  picker.OpenFree();
  EXPECT_EQ(Access::Ids(picker), (std::vector<std::string>{"c-ann"}));
  EXPECT_FALSE(Access::ConfirmEnabled(picker));
  Access::Tap(picker, "c-ann");
  EXPECT_TRUE(Access::ConfirmEnabled(picker));
  Access::Confirm(picker);
  EXPECT_EQ(direct_thread_calls, 1);
  EXPECT_EQ(last_direct, "c-ann");
  EXPECT_TRUE(returned.empty()) << "no choose-mode callback in the old uses";
}

} // namespace
} // namespace pbr
