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
      typed.payload_hidden = payload_hidden_;
      typed.link_payload_full = hidden_full_.c_str();
      return typed;
    };
    controller_.BindShellPinGate(std::move(shell));
    UnlockGateCompletePorts complete;
    complete.complete_phrase_restore = [this](const SecretText& pin, const SecretText& phrase, bool is_default,
                                              const std::vector<std::string>& saved_copies) {
      phrase_restore_ = std::make_tuple(pin.Reveal(), phrase.Reveal(), is_default);
      saved_copies_ = saved_copies;
    };
    complete.pick_saved_copy = [this](std::function<void(std::string, std::optional<std::string>)> picked) {
      picked(pick_name_, pick_text_);
    };
    complete.complete_link_device = [this](const SecretText& pin, const std::string& bundle, bool is_default) {
      link_ = std::make_tuple(pin.Reveal(), bundle, is_default);
    };
    complete.complete_with_pin = [this](const SecretText& pin, bool) { typed_pin_ = pin.Reveal(); };
    complete.use_device_pin_instead = [this]() { ++use_pin_instead_; };
    controller_.BindGateComplete(std::move(complete));
  }

  PinGateController controller_;
  PinGateState shown_;
  std::string typed_payload_;
  bool payload_hidden_ = false;
  std::string hidden_full_;
  std::optional<std::tuple<std::string, std::string, bool>> phrase_restore_;
  std::optional<std::tuple<std::string, std::string, bool>> link_;
  std::vector<std::string> saved_copies_;
  std::string pick_name_;
  std::optional<std::string> pick_text_;
  std::optional<std::string> typed_pin_;
  int use_pin_instead_ = 0;
};

// PIN grace window: a call answered on a locked phone waits for the phone's unlock; the screen says so and offers the
// PIN instead of a PIN box. Off again: the unlock screen as before.
TEST_F(PinGateControllerTest, TheWaitingScreenOffersThePinInstead) {
  controller_.ShowUnlock();
  const std::string plain_message = shown_.message.c_str();
  ASSERT_TRUE(shown_.unlock_mode);
  EXPECT_FALSE(shown_.waiting_device_unlock);

  controller_.SetWaitingDeviceUnlock(true);
  EXPECT_TRUE(shown_.waiting_device_unlock);
  EXPECT_NE(std::string(shown_.message.c_str()), plain_message);

  controller_.OnSubmit();
  EXPECT_FALSE(typed_pin_) << "no PIN box while waiting";

  controller_.OnUsePinInstead();
  EXPECT_EQ(use_pin_instead_, 1);

  controller_.SetWaitingDeviceUnlock(false);
  EXPECT_FALSE(shown_.waiting_device_unlock);
  EXPECT_EQ(std::string(shown_.message.c_str()), plain_message);
  controller_.OnUsePinInstead();
  EXPECT_EQ(use_pin_instead_, 1) << "only while waiting";
}

TEST_F(PinGateControllerTest, WaitingNeedsTheUnlockScreen) {
  controller_.ShowIdentityFork();
  controller_.SetWaitingDeviceUnlock(true);
  EXPECT_FALSE(shown_.waiting_device_unlock);
}

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

TEST_F(PinGateControllerTest, AnOverLongTransferCodeSubmitsFromTheHiddenFullValue) {
  // B166/B167: a transfer code is a multi-KB blob kept out of the rendered link_payload (it froze the app). Submit
  // must still send the real value, which lives in link_payload_full while payload_hidden.
  controller_.ShowIdentityFork();
  controller_.OnIdentityLink();
  controller_.OnUseDefaultPin();
  ASSERT_TRUE(shown_.link_paste_mode);
  // The box renders nothing (the big code was hidden), but the full value is carried alongside.
  typed_payload_ = "";
  payload_hidden_ = true;
  hidden_full_ = "{\"format\":\"pp-browser-link-device-v1\",\"big\":\"" + std::string(20000, 'A') + "\"}";
  controller_.OnSubmit();
  ASSERT_TRUE(link_) << "the hidden full value is what gets sent";
  EXPECT_EQ(std::get<1>(*link_), hidden_full_);
}

} // namespace
} // namespace pbr
