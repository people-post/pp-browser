// The fresh-install PIN gate's paths that bring an existing account (multi-device M023): the transfer code and the
// recovery phrase share the PIN steps, then go to their own completion.
#include "gui/PinGateController.h"

#include "foundation/crypto/PinDefaults.h"

#include <gtest/gtest.h>

#include <optional>
#include <string>
#include <tuple>
#include "common/PbrCompat.h"

namespace pbr {
namespace {

class PinGateControllerTest : public ::testing::Test {
protected:
  void SetUp() override {
    ShellPinGatePorts shell;
    shell.apply_pin_gate = [this](const PinGateState& state) { shown_ = state; };
    shell.pin_gate_snapshot = [this]() {
      PinGateState typed = shown_;
      typed.link_payload = typed_payload_.c_str();
      return typed;
    };
    controller_.BindShellPinGate(std::move(shell));
    UnlockGateCompletePorts complete;
    complete.complete_phrase_restore = [this](const std::string& pin, const std::string& phrase, bool is_default,
                                              const std::vector<std::string>& saved_copies) {
      phrase_restore_ = std::make_tuple(pin, phrase, is_default);
      saved_copies_ = saved_copies;
    };
    complete.pick_saved_copy = [this](std::function<void(std::string, std::optional<std::string>)> picked) {
      picked(pick_name_, pick_text_);
    };
    complete.complete_link_device = [this](const std::string& pin, const std::string& bundle, bool is_default) {
      link_ = std::make_tuple(pin, bundle, is_default);
    };
    controller_.BindGateComplete(std::move(complete));
  }

  PinGateController controller_;
  PinGateState shown_;
  std::string typed_payload_;
  std::optional<std::tuple<std::string, std::string, bool>> phrase_restore_;
  std::optional<std::tuple<std::string, std::string, bool>> link_;
  std::vector<std::string> saved_copies_;
  std::string pick_name_;
  std::optional<std::string> pick_text_;
};

TEST_F(PinGateControllerTest, ThePhrasePathAsksForAPinThenTheWords) {
  controller_.ShowIdentityFork();
  controller_.OnIdentityPhrase();
  ASSERT_TRUE(shown_.chooser_mode) << "a PIN for this device first";
  controller_.OnUseDefaultPin();
  ASSERT_TRUE(shown_.link_paste_mode);
  EXPECT_TRUE(shown_.phrase_flow) << "the words box, not the transfer-code box";

  controller_.OnSubmit();
  EXPECT_FALSE(phrase_restore_) << "nothing typed yet";
  EXPECT_FALSE(std::string(shown_.error.c_str()).empty());

  typed_payload_ = "legal winner thank year";
  controller_.OnSubmit();
  ASSERT_TRUE(phrase_restore_);
  EXPECT_EQ(*phrase_restore_, std::make_tuple(std::string(kDefaultProfilePin), typed_payload_, true));
  EXPECT_FALSE(link_);
}

TEST_F(PinGateControllerTest, ASavedCopyPickedOnThePhrasePageRidesWithTheWords) {
  controller_.ShowIdentityFork();
  controller_.OnIdentityPhrase();
  controller_.OnUseDefaultPin();
  typed_payload_ = "legal winner thank year";

  pick_name_ = "pp-account-2026-10-30.json";  // unreadable (no text): said, nothing kept
  controller_.OnUseSavedCopy();
  EXPECT_FALSE(std::string(shown_.error.c_str()).empty());
  EXPECT_TRUE(std::string(shown_.saved_copy_name.c_str()).empty());

  pick_text_ = "{\"format\":\"pp-browser-account-locker-file-v1\"}";
  controller_.OnUseSavedCopy();
  EXPECT_EQ(std::string(shown_.saved_copy_name.c_str()), pick_name_);
  EXPECT_TRUE(std::string(shown_.error.c_str()).empty());
  EXPECT_EQ(std::string(shown_.link_payload.c_str()), typed_payload_) << "the words typed so far stay";

  controller_.OnSubmit();
  ASSERT_TRUE(phrase_restore_);
  EXPECT_EQ(saved_copies_, std::vector<std::string>{*pick_text_});
}

TEST_F(PinGateControllerTest, TheLinkPathStillTakesATransferCode) {
  controller_.ShowIdentityFork();
  controller_.OnIdentityPhrase();
  controller_.OnCancel();  // no gate bound: stays; back to the fork the way the app does
  controller_.ShowIdentityFork();
  controller_.OnIdentityLink();
  controller_.OnUseDefaultPin();
  ASSERT_TRUE(shown_.link_paste_mode);
  EXPECT_FALSE(shown_.phrase_flow) << "leaving the phrase path forgot it";
  typed_payload_ = "{\"format\":\"pp-browser-link-device-v1\"}";
  controller_.OnSubmit();
  ASSERT_TRUE(link_);
  EXPECT_FALSE(phrase_restore_);
}

} // namespace
} // namespace pbr
