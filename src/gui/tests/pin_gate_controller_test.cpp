// The fresh-install PIN gate's paths that bring an existing account (multi-device M023, M027): linking to the main
// device (this device's offer as a QR code, the main device's reply scanned or pasted) and the recovery phrase share the
// PIN steps, then go to their own completion.
#include "gui/PinGateController.h"

#include "foundation/crypto/PinDefaults.h"
#include "foundation/error/AppError.h"
#include "foundation/i18n/LocalizationService.h"

#include <gtest/gtest.h>

#include "foundation/crypto/CryptoConstants.h"
#include "foundation/crypto/ProfileSecretsEngine.h"
#include "foundation/crypto/ProfileUnlockGate.h"

#include <filesystem>

#include <chrono>

#include <optional>
#include <string>
#include <tuple>
#include "common/PbrCompat.h"

namespace pbr {
namespace {

class PinGateControllerTest : public ::testing::Test {
protected:
  void SetUp() override {
    // The notes are built from catalog text ({pin}); load it here so the tests do not depend on which test ran first.
    ASSERT_TRUE(LocalizationService::Instance().LoadFromAssets(PP_BROWSER_ASSETS_DIR));
    LocalizationService::Instance().SetPreferredLanguage("en");
    ShellPinGatePorts shell;
    shell.apply_pin_gate = [this](const PinGateState& state) { shown_ = state; };
    shell.pin_gate_snapshot = [this]() {
      PinGateState typed = shown_;
      typed.link_payload = typed_payload_.c_str();
      typed.payload_hidden = payload_hidden_;
      typed.link_payload_full = hidden_full_.c_str();
      if (field_pin_) {
        typed.pin = field_pin_->c_str(); // typed in the field (field mode only: the pad has none)
      }
      if (field_confirm_) {
        typed.pin_confirm = field_confirm_->c_str();
      }
      return typed;
    };
    shell.remount_pin_gate = [this]() { ++remounts_; };
    controller_.BindShellPinGate(std::move(shell));
    PinGateKeypadPorts keypad;
    keypad.keypad_available = [this]() { return keypad_available_; };
    keypad.pin_digits = [this]() { return pin_digits_; };
    keypad.biometric_kind = [this]() { return biometric_kind_; };
    controller_.BindKeypad(std::move(keypad));
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
    complete.begin_link_offer = [this]() -> Roe<LinkOfferDisplay> {
      ++offers_;
      return LinkOfferDisplay{.qr_svg = "<rect/>", .text = "pp-browser-link-offer:AQ", .code = "0427"};
    };
    complete.forget_link_offer = [this]() { ++forgotten_; };
    complete.scan_link_reply = [this](std::function<void(std::string)> done, std::function<void()> closed) {
      scan_done_ = std::move(done);
      scan_closed_ = std::move(closed);
    };
    complete.complete_with_pin = [this](const SecretText& pin, const bool create_mode) {
      typed_pin_ = pin.Reveal();
      typed_create_ = create_mode;
      ++completes_;
    };
    complete.use_device_pin_instead = [this]() { ++use_pin_instead_; };
    complete.try_biometric = [this]() { ++biometric_tries_; };
    controller_.BindGateComplete(std::move(complete));
    // Each reading of the clock a second later: a choice is never "the same tap again" unless a test says so.
    controller_.SetClockForTest([this]() { return clock_ += std::chrono::seconds(1); });
  }

  std::chrono::steady_clock::time_point clock_{};

  PinGateController controller_;
  PinGateState shown_;
  std::string typed_payload_;
  bool payload_hidden_ = false;
  std::string hidden_full_;
  std::optional<std::tuple<std::string, std::string, bool>> phrase_restore_;
  std::optional<std::tuple<std::string, std::string, bool>> link_;
  int offers_ = 0;
  int forgotten_ = 0;
  std::function<void(std::string)> scan_done_;
  std::function<void()> scan_closed_;
  std::vector<std::string> saved_copies_;
  std::string pick_name_;
  std::optional<std::string> pick_text_;
  std::optional<std::string> typed_pin_;
  bool typed_create_ = false;
  int completes_ = 0;
  int use_pin_instead_ = 0;
  int remounts_ = 0;
  bool keypad_available_ = true; // a phone
  std::optional<int> pin_digits_;
  std::string biometric_kind_; // "": face / fingerprint unlock off
  int biometric_tries_ = 0;
  std::optional<std::string> field_pin_;
  std::optional<std::string> field_confirm_;

  void Keys(const std::string& digits) {
    for (const char c : digits) {
      controller_.OnKey(c - '0');
    }
  }
  std::string Pin() const { return shown_.pin.c_str(); }
  std::string Confirm() const { return shown_.pin_confirm.c_str(); }
  std::string ErrorText() const { return shown_.error.c_str(); }
  std::string Message() const { return shown_.message.c_str(); }
  void ShowCreate() {
    controller_.ShowChooser();
    controller_.OnSetPin();
    ASSERT_TRUE(shown_.create_mode);
  }

  // B2: settings' checks answer when the test says (the real one runs on a worker).
  std::vector<std::string> checked_;
  std::function<void(Roe<void>)> check_done_;
  int verified_ok_ = 0;
  std::vector<std::pair<std::string, std::string>> changes_;
  std::function<void(Roe<void>)> change_done_;
  PinGateCheck Check() {
    return [this](const SecretText& pin, std::function<void(Roe<void>)> done) {
      checked_.push_back(pin.Reveal());
      check_done_ = std::move(done);
    };
  }
  PinGateChange Change() {
    return [this](const SecretText& old_pin, const SecretText& new_pin, std::function<void(Roe<void>)> done) {
      changes_.emplace_back(old_pin.Reveal(), new_pin.Reveal());
      change_done_ = std::move(done);
    };
  }
  static pbr::Error WrongPin() { return AppError::Pin(Err::Pin::Generic, "Incorrect PIN or corrupt vault"); }
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

  pick_name_ = "pp-account-2026-10-30.json"; // unreadable (no text): said, nothing kept
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

TEST_F(PinGateControllerTest, TheLinkPathTakesTheMainDevicesReply) {
  controller_.ShowIdentityFork();
  controller_.OnIdentityPhrase();
  controller_.OnCancel(); // no gate bound: stays; back to the fork the way the app does
  controller_.ShowIdentityFork();
  controller_.OnIdentityLink();
  controller_.OnUseDefaultPin();
  ASSERT_TRUE(shown_.link_paste_mode);
  EXPECT_FALSE(shown_.phrase_flow) << "leaving the phrase path forgot it";
  typed_payload_ = "pp-browser-link-reply:AQ";
  controller_.OnSubmit();
  ASSERT_TRUE(link_);
  EXPECT_FALSE(phrase_restore_);
}

// M027: the link page shows this device's one-time offer (QR code and digits); "Scan" steps the gate aside for the
// camera and imports what it reads; nothing read brings the gate back; leaving the page wipes the key.
TEST_F(PinGateControllerTest, TheLinkPageShowsTheOfferAndScansTheReply) {
  controller_.ShowIdentityFork();
  controller_.OnIdentityLink();
  controller_.OnUseDefaultPin();
  ASSERT_TRUE(shown_.link_paste_mode);
  EXPECT_EQ(offers_, 1);
  EXPECT_EQ(std::string(shown_.link_offer_qr.c_str()), "<rect/>");
  EXPECT_EQ(std::string(shown_.link_offer_code.c_str()), "0427");
  EXPECT_TRUE(shown_.link_can_scan);

  controller_.OnScanReply();
  ASSERT_TRUE(scan_closed_);
  EXPECT_FALSE(shown_.active) << "the camera's page, not the gate";
  scan_closed_(); // cancelled: the gate as it was
  EXPECT_TRUE(shown_.active);
  EXPECT_TRUE(shown_.link_paste_mode);
  EXPECT_FALSE(link_);

  controller_.OnScanReply();
  ASSERT_TRUE(scan_done_);
  scan_done_(std::string("\x01reply", 6));
  ASSERT_TRUE(link_) << "what the camera read is imported at once";
  EXPECT_EQ(std::get<1>(*link_), std::string("\x01reply", 6));
  EXPECT_TRUE(shown_.active);

  EXPECT_EQ(forgotten_, 0);
  controller_.ShowIdentityFork(); // back to the start
  EXPECT_EQ(forgotten_, 1) << "the one-time key goes with the page";
}

// The link page's Scan did nothing on every device: Application never filled the shell's port for it, and the test
// above calls OnScanReply() directly. Every action the shell can raise must reach the controller.
TEST_F(PinGateControllerTest, EveryShellActionIsWiredToTheController) {
  const PinGateActionPorts ports = MakePinGateActionPorts(controller_);
  EXPECT_TRUE(ports.on_submit);
  EXPECT_TRUE(ports.on_cancel);
  EXPECT_TRUE(ports.on_set_pin);
  EXPECT_TRUE(ports.on_use_default);
  EXPECT_TRUE(ports.on_identity_new);
  EXPECT_TRUE(ports.on_identity_link);
  EXPECT_TRUE(ports.on_identity_phrase);
  EXPECT_TRUE(ports.on_use_saved_copy);
  EXPECT_TRUE(ports.on_scan_reply) << "the link page's Scan";
  EXPECT_TRUE(ports.on_forgot_toggle);
  EXPECT_TRUE(ports.on_note_toggle);
  EXPECT_TRUE(ports.on_use_pin_instead);
  EXPECT_TRUE(ports.on_key);
  EXPECT_TRUE(ports.on_backspace);
  EXPECT_TRUE(ports.on_keypad_ok);
  EXPECT_TRUE(ports.on_use_text);
  EXPECT_TRUE(ports.on_use_keypad);
  EXPECT_TRUE(ports.on_biometric);
  EXPECT_TRUE(ports.on_use_pin);

  // And pressing it on the link page reaches the camera port.
  controller_.ShowIdentityFork();
  controller_.OnIdentityLink();
  controller_.OnUseDefaultPin();
  ASSERT_TRUE(shown_.link_paste_mode);
  ports.on_scan_reply();
  EXPECT_TRUE(scan_closed_) << "Scan on the link page opened the scan";
}

// "Scan" is offered only where a camera is listed; with none, only Paste is left.
TEST_F(PinGateControllerTest, ScanIsOfferedOnlyWithACamera) {
  bool camera = false;
  UnlockGateCompletePorts complete;
  complete.begin_link_offer = []() -> Roe<LinkOfferDisplay> {
    return LinkOfferDisplay{.qr_svg = "<rect/>", .text = "pp-browser-link-offer:AQ", .code = "0427"};
  };
  complete.scan_link_reply = [](std::function<void(std::string)>, std::function<void()>) {};
  complete.can_scan_link_reply = [&camera]() { return camera; };
  controller_.BindGateComplete(std::move(complete));
  controller_.ShowIdentityFork();
  controller_.OnIdentityLink();
  controller_.OnUseDefaultPin();
  ASSERT_TRUE(shown_.link_paste_mode);
  EXPECT_FALSE(shown_.link_can_scan) << "no camera listed";
  EXPECT_FALSE(std::string(shown_.link_valid.c_str()).empty());

  camera = true;
  controller_.ShowIdentityFork();
  controller_.OnIdentityLink();
  controller_.OnUseDefaultPin();
  EXPECT_TRUE(shown_.link_can_scan);
}

TEST_F(PinGateControllerTest, ThePhrasePageMakesNoOffer) {
  controller_.ShowIdentityFork();
  controller_.OnIdentityPhrase();
  controller_.OnUseDefaultPin();
  EXPECT_EQ(offers_, 0);
  EXPECT_TRUE(std::string(shown_.link_offer_qr.c_str()).empty());
  controller_.ShowIdentityFork();
  EXPECT_EQ(forgotten_, 0);
}

TEST_F(PinGateControllerTest, AnOverLongTransferCodeSubmitsFromTheHiddenFullValue) {
  // B166/B167: the reply is a 1.6 KB blob kept out of the rendered link_payload (it froze the app). Submit
  // must still send the real value, which lives in link_payload_full while payload_hidden.
  controller_.ShowIdentityFork();
  controller_.OnIdentityLink();
  controller_.OnUseDefaultPin();
  ASSERT_TRUE(shown_.link_paste_mode);
  // The box renders nothing (the big code was hidden), but the full value is carried alongside.
  typed_payload_ = "";
  payload_hidden_ = true;
  hidden_full_ = "pp-browser-link-reply:" + std::string(1700, 'A');
  controller_.OnSubmit();
  ASSERT_TRUE(link_) << "the hidden full value is what gets sent";
  EXPECT_EQ(std::get<1>(*link_), hidden_full_);
}

// --- The number pad (DEV/PLAN-pin-gate-redesign.md B): the create state table, row by row ---

TEST_F(PinGateControllerTest, CreateKeypadDotsGrowWithInput) {
  ShowCreate();
  ASSERT_TRUE(shown_.keypad) << "a new PIN on a phone: the pad";
  EXPECT_EQ(shown_.keypad_slots, 0);
  EXPECT_EQ(Message(), Tr("pin.create_digits_message"));
  Keys("123");
  EXPECT_EQ(Pin(), "123");
  EXPECT_EQ(shown_.keypad_slots, 3);
  EXPECT_EQ(shown_.keypad_filled, 3);
  EXPECT_EQ(completes_, 0) << "a new PIN never submits by itself on its first entry";
}

TEST_F(PinGateControllerTest, CreateKeypadStopsAtEight) {
  ShowCreate();
  Keys("123456789");
  EXPECT_EQ(Pin(), "12345678");
  EXPECT_EQ(shown_.keypad_slots, 8);
}

TEST_F(PinGateControllerTest, CreateKeypadOkNeedsFour) {
  ShowCreate();
  Keys("123");
  EXPECT_FALSE(shown_.keypad_ok_shown);
  controller_.OnKeypadOk();
  EXPECT_EQ(Message(), Tr("pin.create_digits_message")) << "still the first entry";
  EXPECT_EQ(Pin(), "123");
  controller_.OnKey(4);
  EXPECT_TRUE(shown_.keypad_ok_shown);
}

TEST_F(PinGateControllerTest, CreateKeypadOkGoesToRepeat) {
  ShowCreate();
  Keys("12345");
  controller_.OnKeypadOk();
  EXPECT_EQ(shown_.keypad_slots, 5) << "as many dots as the first entry";
  EXPECT_EQ(shown_.keypad_filled, 0);
  EXPECT_EQ(Confirm(), "");
  EXPECT_FALSE(shown_.keypad_ok_shown);
  EXPECT_EQ(Message(), Tr("pin.create_repeat_message"));
}

TEST_F(PinGateControllerTest, CreateKeypadRepeatFills) {
  ShowCreate();
  Keys("12345");
  controller_.OnKeypadOk();
  Keys("12");
  EXPECT_EQ(Confirm(), "12");
  EXPECT_EQ(shown_.keypad_filled, 2);
  EXPECT_EQ(shown_.keypad_slots, 5);
  EXPECT_EQ(Pin(), "12345") << "the first entry stays";
}

TEST_F(PinGateControllerTest, CreateKeypadRepeatMatchCompletes) {
  ShowCreate();
  Keys("12345");
  controller_.OnKeypadOk();
  Keys("12345");
  EXPECT_EQ(completes_, 1) << "the full repeat submits by itself";
  EXPECT_EQ(typed_pin_, "12345");
  EXPECT_TRUE(typed_create_);
  EXPECT_TRUE(shown_.keypad_busy) << "until the vault answers";
  controller_.OnKey(1);
  EXPECT_EQ(completes_, 1);
}

TEST_F(PinGateControllerTest, CreateKeypadRepeatMismatchRestarts) {
  ShowCreate();
  Keys("12345");
  controller_.OnKeypadOk();
  Keys("12346");
  EXPECT_EQ(completes_, 0);
  EXPECT_EQ(ErrorText(), Tr("pin.error.mismatch"));
  EXPECT_EQ(Pin(), "");
  EXPECT_EQ(Confirm(), "");
  EXPECT_EQ(shown_.keypad_slots, 0) << "back to the first entry";
  EXPECT_EQ(Message(), Tr("pin.create_digits_message"));
  Keys("2468");
  EXPECT_EQ(Pin(), "2468") << "the first entry again, not the repeat";
}

TEST_F(PinGateControllerTest, CreateKeypadBackspace) {
  ShowCreate();
  Keys("1234");
  controller_.OnBackspace();
  EXPECT_EQ(Pin(), "123");
  EXPECT_EQ(shown_.keypad_slots, 3);
  controller_.OnKey(4);
  controller_.OnKeypadOk();
  controller_.OnKey(1);
  controller_.OnBackspace();
  EXPECT_EQ(Confirm(), "");
  controller_.OnBackspace(); // an empty repeat does not go back to the first entry
  EXPECT_EQ(Confirm(), "");
  EXPECT_EQ(Pin(), "1234");
  EXPECT_EQ(Message(), Tr("pin.create_repeat_message"));
}

TEST_F(PinGateControllerTest, CreateKeypadSwitchToText) {
  ShowCreate();
  Keys("1234");
  const int remounts = remounts_;
  EXPECT_EQ(std::string(shown_.keypad_text_label.c_str()), Tr("pin.create_use_text"));
  controller_.OnUseText();
  EXPECT_FALSE(shown_.keypad);
  EXPECT_TRUE(shown_.ask_confirm) << "the field and its confirmation";
  EXPECT_EQ(Pin(), "");
  EXPECT_GT(remounts_, remounts) << "the field instead of the pad";
  field_pin_ = "correct horse";
  field_confirm_ = "correct horse";
  controller_.OnSubmit();
  EXPECT_EQ(typed_pin_, "correct horse");
  EXPECT_TRUE(typed_create_);
}

TEST_F(PinGateControllerTest, CreateTextSwitchToKeypad) {
  ShowCreate();
  controller_.OnUseText();
  ASSERT_FALSE(shown_.keypad);
  EXPECT_TRUE(shown_.keypad_offered);
  field_pin_ = "12";
  controller_.OnUseKeypad();
  field_pin_.reset();
  EXPECT_TRUE(shown_.keypad);
  EXPECT_EQ(Pin(), "");
  EXPECT_EQ(shown_.keypad_slots, 0);
  EXPECT_EQ(Message(), Tr("pin.create_digits_message"));
}

TEST_F(PinGateControllerTest, CreateTextChecksLengthAndRepeatAsBefore) {
  keypad_available_ = false; // desktop: the field, as before
  ShowCreate();
  EXPECT_FALSE(shown_.keypad);
  EXPECT_FALSE(shown_.keypad_offered);
  field_pin_ = "123";
  field_confirm_ = "123";
  controller_.OnSubmit();
  EXPECT_EQ(ErrorText(), Tr("errors.pin.too_short"));
  field_pin_ = "1234";
  field_confirm_ = "1243";
  controller_.OnSubmit();
  EXPECT_EQ(ErrorText(), Tr("pin.error.mismatch"));
  EXPECT_EQ(completes_, 0);
}

// --- Unlock on the pad ---

TEST_F(PinGateControllerTest, UnlockKeypadAutoSubmitsAtLength) {
  pin_digits_ = 6;
  controller_.ShowUnlock();
  ASSERT_TRUE(shown_.keypad);
  EXPECT_EQ(shown_.keypad_slots, 6);
  Keys("13579");
  EXPECT_EQ(completes_, 0);
  EXPECT_EQ(shown_.keypad_filled, 5);
  controller_.OnKey(1);
  EXPECT_EQ(completes_, 1);
  EXPECT_EQ(typed_pin_, "135791");
  EXPECT_FALSE(typed_create_);
}

TEST_F(PinGateControllerTest, UnlockKeypadIgnoresKeysWhileVerifying) {
  pin_digits_ = 4;
  controller_.ShowUnlock();
  Keys("1234");
  ASSERT_TRUE(shown_.keypad_busy);
  const PinGateState before = shown_;
  controller_.OnKey(5);
  controller_.OnBackspace();
  controller_.OnUseText();
  controller_.OnSubmit();
  EXPECT_EQ(Pin(), std::string(before.pin.c_str()));
  EXPECT_TRUE(shown_.keypad);
  EXPECT_TRUE(shown_.keypad_busy);
  EXPECT_EQ(completes_, 1) << "one check only";
}

TEST_F(PinGateControllerTest, UnlockKeypadWrongPinClears) {
  pin_digits_ = 4;
  controller_.ShowUnlock();
  Keys("1234");
  controller_.ShowError(Tr("pin.error.wrong")); // what the unlock gate reports
  EXPECT_FALSE(shown_.keypad_busy);
  EXPECT_EQ(Pin(), "");
  EXPECT_EQ(shown_.keypad_filled, 0);
  EXPECT_EQ(ErrorText(), Tr("pin.error.wrong"));
  Keys("4321");
  EXPECT_EQ(completes_, 2) << "and the next try goes in";
}

TEST_F(PinGateControllerTest, UnlockKeypadNotOnDesktop) {
  keypad_available_ = false;
  pin_digits_ = 6;
  controller_.ShowUnlock();
  EXPECT_FALSE(shown_.keypad);
  EXPECT_FALSE(shown_.keypad_offered);
  controller_.OnKey(1);
  EXPECT_EQ(Pin(), "");
  field_pin_ = "135790";
  controller_.OnSubmit();
  EXPECT_EQ(typed_pin_, "135790");
}

TEST_F(PinGateControllerTest, UnlockKeypadBackspace) {
  pin_digits_ = 4;
  controller_.ShowUnlock();
  Keys("12");
  controller_.OnBackspace();
  EXPECT_EQ(Pin(), "1");
  EXPECT_EQ(shown_.keypad_filled, 1);
  controller_.OnBackspace();
  controller_.OnBackspace();
  EXPECT_EQ(Pin(), "");
  Keys("1234");
  EXPECT_EQ(typed_pin_, "1234");
}

TEST_F(PinGateControllerTest, UnlockKeypadSwitchToTextAcceptsAnyPin) {
  pin_digits_ = 4;
  controller_.ShowUnlock();
  EXPECT_EQ(std::string(shown_.keypad_text_label.c_str()), Tr("pin.keypad_use_text"));
  Keys("12");
  controller_.OnUseText();
  EXPECT_FALSE(shown_.keypad);
  EXPECT_FALSE(shown_.ask_confirm);
  EXPECT_EQ(Pin(), "");
  field_pin_ = "1234567890"; // remembered as four digits, typed as ten: still goes in
  controller_.OnSubmit();
  EXPECT_EQ(typed_pin_, "1234567890");
}

TEST_F(PinGateControllerTest, UnlockTextSwitchToKeypadWhenDigitsKnown) {
  controller_.ShowUnlock(); // unknown kind (a profile from before): the field
  EXPECT_FALSE(shown_.keypad);
  EXPECT_FALSE(shown_.keypad_offered);
  controller_.OnUseKeypad();
  EXPECT_FALSE(shown_.keypad);

  pin_digits_ = 0; // a text PIN
  controller_.ShowUnlock();
  EXPECT_FALSE(shown_.keypad);
  EXPECT_FALSE(shown_.keypad_offered);

  pin_digits_ = 5;
  controller_.ShowUnlock();
  ASSERT_TRUE(shown_.keypad);
  controller_.OnUseText();
  EXPECT_TRUE(shown_.keypad_offered);
  controller_.OnUseKeypad();
  EXPECT_TRUE(shown_.keypad);
  EXPECT_EQ(shown_.keypad_slots, 5);
}

// --- B2: settings asks on the gate ---

TEST_F(PinGateControllerTest, VerifyKeypadAutoSubmitsAndCallsBack) {
  pin_digits_ = 4;
  controller_.ShowVerify(PinVerifyPurpose::LinkDevice, Check(), [this]() { ++verified_ok_; });
  ASSERT_TRUE(shown_.verify_mode);
  ASSERT_TRUE(shown_.keypad);
  EXPECT_EQ(Message(), Tr("pin.verify_export_message"));
  Keys("2468");
  ASSERT_EQ(checked_, std::vector<std::string>{"2468"});
  EXPECT_TRUE(shown_.keypad_busy);
  EXPECT_EQ(verified_ok_, 0) << "not before the check passed";
  EXPECT_EQ(completes_, 0) << "never the unlock gate";
  check_done_(Roe<void>{});
  EXPECT_FALSE(shown_.active) << "the gate closed";
  EXPECT_EQ(verified_ok_, 1);
}

TEST_F(PinGateControllerTest, VerifyWrongPinStays) {
  pin_digits_ = 4;
  controller_.ShowVerify(PinVerifyPurpose::ShowRecoveryPhrase, Check(), [this]() { ++verified_ok_; });
  EXPECT_EQ(Message(), Tr("pin.verify_recovery_message"));
  Keys("1111");
  check_done_(WrongPin());
  EXPECT_TRUE(shown_.active);
  EXPECT_TRUE(shown_.verify_mode);
  EXPECT_EQ(ErrorText(), Tr("pin.error.wrong"));
  EXPECT_EQ(Pin(), "");
  EXPECT_FALSE(shown_.keypad_busy);
  EXPECT_EQ(verified_ok_, 0);
  Keys("2468");
  EXPECT_EQ(checked_.size(), 2U) << "another try";
}

TEST_F(PinGateControllerTest, VerifyCancelReturns) {
  pin_digits_ = 4;
  controller_.ShowVerify(PinVerifyPurpose::LinkDevice, Check(), [this]() { ++verified_ok_; });
  Keys("2468");
  controller_.OnCancel(); // while the check runs
  EXPECT_FALSE(shown_.active) << "back to settings";
  check_done_(Roe<void>{});
  EXPECT_EQ(verified_ok_, 0) << "a late answer does nothing";
  EXPECT_FALSE(shown_.active);
}

TEST_F(PinGateControllerTest, ChangeKeypadThreeSteps) {
  pin_digits_ = 4;
  controller_.ShowChange(Check(), Change());
  ASSERT_TRUE(shown_.change_mode);
  ASSERT_TRUE(shown_.keypad);
  EXPECT_EQ(shown_.keypad_slots, 4) << "the current PIN's length is known";
  EXPECT_EQ(Message(), Tr("pin.change_old_message"));
  Keys("1234");
  ASSERT_EQ(checked_, std::vector<std::string>{"1234"});
  check_done_(Roe<void>{});
  // Step 2: a new PIN, as on create.
  EXPECT_TRUE(shown_.active);
  EXPECT_TRUE(shown_.keypad);
  EXPECT_EQ(shown_.keypad_slots, 0);
  EXPECT_EQ(Message(), Tr("pin.create_digits_message"));
  EXPECT_EQ(std::string(shown_.keypad_text_label.c_str()), Tr("pin.create_use_text"));
  Keys("135790");
  controller_.OnKeypadOk();
  // Step 3: again.
  EXPECT_EQ(Message(), Tr("pin.create_repeat_message"));
  EXPECT_EQ(shown_.keypad_slots, 6);
  Keys("135790");
  ASSERT_EQ(changes_, (std::vector<std::pair<std::string, std::string>>{{"1234", "135790"}}));
  EXPECT_TRUE(shown_.active) << "until the change is done";
  change_done_(Roe<void>{});
  EXPECT_FALSE(shown_.active);
  EXPECT_EQ(completes_, 0) << "never the unlock gate";
}

TEST_F(PinGateControllerTest, ChangeOldPinWrongStays) {
  pin_digits_ = 4;
  controller_.ShowChange(Check(), Change());
  Keys("9999");
  check_done_(WrongPin());
  EXPECT_EQ(ErrorText(), Tr("pin.error.wrong"));
  EXPECT_EQ(Message(), Tr("pin.change_old_message")) << "still the first step";
  EXPECT_EQ(shown_.keypad_slots, 4);
  EXPECT_EQ(Pin(), "");
  EXPECT_TRUE(changes_.empty());
}

TEST_F(PinGateControllerTest, ChangeRepeatMismatchBackToNew) {
  pin_digits_ = 4;
  controller_.ShowChange(Check(), Change());
  Keys("1234");
  check_done_(Roe<void>{});
  Keys("5678");
  controller_.OnKeypadOk();
  Keys("5679");
  EXPECT_TRUE(changes_.empty());
  EXPECT_EQ(ErrorText(), Tr("pin.error.mismatch"));
  EXPECT_EQ(Message(), Tr("pin.create_digits_message")) << "the new PIN again, not the current one";
  EXPECT_EQ(Pin(), "");
  Keys("2468");
  controller_.OnKeypadOk();
  Keys("2468");
  ASSERT_EQ(changes_, (std::vector<std::pair<std::string, std::string>>{{"1234", "2468"}}));
}

TEST_F(PinGateControllerTest, ChangeTextModeWhenUnknown) {
  controller_.ShowChange(Check(), Change()); // the current PIN's kind unknown: the field
  EXPECT_FALSE(shown_.keypad);
  EXPECT_FALSE(shown_.ask_confirm);
  field_pin_ = "old secret";
  controller_.OnSubmit();
  ASSERT_EQ(checked_, std::vector<std::string>{"old secret"});
  field_pin_.reset();
  check_done_(Roe<void>{});
  // The new PIN: the pad on a phone (digits by default), with the field one tap away.
  EXPECT_TRUE(shown_.keypad);
  controller_.OnUseText();
  EXPECT_TRUE(shown_.ask_confirm);
  EXPECT_EQ(Message(), Tr("pin.change_new_message"));
  field_pin_ = "new secret";
  field_confirm_ = "new secret";
  controller_.OnSubmit();
  ASSERT_EQ(changes_, (std::vector<std::pair<std::string, std::string>>{{"old secret", "new secret"}}));
}

// The gate declined a full pad's PIN (another unlock was running) and said so: the pad takes keys again.
TEST_F(PinGateControllerTest, ADeclinedPinLeavesThePadUsable) {
  pin_digits_ = 4;
  controller_.ShowUnlock();
  Keys("1234");
  ASSERT_TRUE(shown_.keypad_busy);
  ASSERT_EQ(completes_, 1);

  controller_.OnSubmitDeclined();
  EXPECT_FALSE(shown_.keypad_busy);
  EXPECT_EQ(Pin(), "");
  EXPECT_EQ(shown_.keypad_filled, 0);
  Keys("1234");
  EXPECT_EQ(completes_, 2) << "typed and submitted again";
}

// Kenneth 2026-11-07: on the app default PIN, Change PIN starts at the new PIN; the change gets the default PIN.
TEST_F(PinGateControllerTest, ChangeFromTheDefaultPinStartsAtTheNewPin) {
  pin_digits_.reset();
  controller_.ShowChange(Check(), Change(), SecretText(std::string(kDefaultProfilePin)));
  ASSERT_TRUE(shown_.change_mode);
  EXPECT_TRUE(shown_.keypad) << "a new PIN: the pad on a phone";
  EXPECT_EQ(Message(), Tr("pin.create_digits_message"));
  Keys("2468");
  controller_.OnKeypadOk();
  Keys("2468");
  EXPECT_TRUE(checked_.empty()) << "no current PIN asked or checked";
  ASSERT_EQ(changes_, (std::vector<std::pair<std::string, std::string>>{{std::string(kDefaultProfilePin), "2468"}}));
  change_done_(Roe<void>{});
  EXPECT_FALSE(shown_.active);
}

// The app's default PIN is never asked for; people on it only see one line with a "?" that tells the digits.
TEST_F(PinGateControllerTest, ChangeFromTheDefaultPinSaysSoAndTheQuestionMarkTellsThePin) {
  for (const char* language : {"en", "zh-Hans"}) {
    LocalizationService::Instance().SetPreferredLanguage(language);
    controller_.ShowChange(Check(), Change(), SecretText(std::string(kDefaultProfilePin)));
    EXPECT_EQ(std::string(shown_.default_note_line.c_str()), Tr("settings.security.default_hint"));
    EXPECT_FALSE(shown_.default_note_open);
    EXPECT_NE(std::string(shown_.default_note_text.c_str()).find(kDefaultProfilePin), std::string::npos)
        << "the digits come from the constant";
    controller_.OnNoteToggle();
    EXPECT_TRUE(shown_.default_note_open);
    controller_.OnNoteToggle();
    EXPECT_FALSE(shown_.default_note_open);
  }
  LocalizationService::Instance().SetPreferredLanguage("en");
}

TEST_F(PinGateControllerTest, ChangeFromAnOwnPinHasNoDefaultNote) {
  pin_digits_ = 4;
  controller_.ShowChange(Check(), Change(), std::nullopt);
  EXPECT_TRUE(shown_.default_note_line.empty());
  EXPECT_TRUE(shown_.default_note_text.empty());
  controller_.OnNoteToggle();
  EXPECT_FALSE(shown_.default_note_open) << "nothing to open";
}

TEST_F(PinGateControllerTest, JustContinueOnTheLinkAndPhrasePagesSaysTheDeviceUsesTheDefault) {
  for (const char* language : {"en", "zh-Hans"})
    for (const bool phrase : {false, true}) {
      LocalizationService::Instance().SetPreferredLanguage(language);
      controller_.ShowIdentityFork();
      if (phrase) {
        controller_.OnIdentityPhrase();
      } else {
        controller_.OnIdentityLink();
      }
      controller_.OnUseDefaultPin();
      ASSERT_TRUE(shown_.link_paste_mode);
      EXPECT_EQ(shown_.phrase_flow, phrase);
      EXPECT_EQ(std::string(shown_.default_note_line.c_str()), Tr("pin.default_note.device"));
      EXPECT_NE(std::string(shown_.default_note_text.c_str()).find(kDefaultProfilePin), std::string::npos);
      EXPECT_FALSE(shown_.default_note_open);
      controller_.OnNoteToggle();
      EXPECT_TRUE(shown_.default_note_open);
    }
  LocalizationService::Instance().SetPreferredLanguage("en");
}

TEST_F(PinGateControllerTest, AnOwnPinOnTheLinkPageHasNoDefaultNote) {
  controller_.ShowIdentityFork();
  controller_.OnIdentityLink();
  controller_.OnSetPin();
  EXPECT_TRUE(shown_.create_mode);
  EXPECT_TRUE(shown_.default_note_line.empty());
  Keys("2468");
  controller_.OnKeypadOk();
  Keys("2468");
  ASSERT_TRUE(shown_.link_paste_mode);
  EXPECT_TRUE(shown_.default_note_line.empty());
}

// Face / fingerprint unlock (at-rest A014, DEV/PLAN-pin-gate-redesign.md C): the unlock door's key (pad) / link
// (field) prompts again; only there, and not while a PIN is being checked.
TEST_F(PinGateControllerTest, UnlockShowsTheBiometricKeyWhenOn) {
  pin_digits_ = 6;
  biometric_kind_ = "face";
  controller_.ShowUnlock();
  EXPECT_TRUE(shown_.biometric_first) << "the door opens with the key and Enter PIN only";
  EXPECT_TRUE(shown_.biometric_key);
  EXPECT_EQ(std::string(shown_.biometric_kind.c_str()), "face");
  EXPECT_EQ(std::string(shown_.message.c_str()), Tr("pin.biometric_first.face"));
  controller_.OnBiometric();
  EXPECT_EQ(biometric_tries_, 1);
  const int remounts = remounts_;
  controller_.OnUsePin();
  EXPECT_FALSE(shown_.biometric_first);
  EXPECT_GT(remounts_, remounts) << "the pad instead of the key-only door";
  EXPECT_EQ(std::string(shown_.message.c_str()), Tr("pin.unlock_message"));
  EXPECT_TRUE(shown_.keypad);
  EXPECT_TRUE(shown_.biometric_key);
  EXPECT_FALSE(shown_.keypad_ok_shown) << "the bottom-left key is free for it";
  controller_.OnUsePin();
  EXPECT_EQ(remounts_, remounts + 1) << "nothing more to open";

  controller_.OnUseText(); // the field: the link prompts too
  EXPECT_FALSE(shown_.keypad);
  EXPECT_TRUE(shown_.biometric_key);
  controller_.OnBiometric();
  EXPECT_EQ(biometric_tries_, 2);

  biometric_kind_.clear(); // off
  controller_.ShowUnlock();
  EXPECT_FALSE(shown_.biometric_key);
  EXPECT_FALSE(shown_.biometric_first) << "straight to the pad";
  EXPECT_EQ(std::string(shown_.message.c_str()), Tr("pin.unlock_message"));
  controller_.OnBiometric();
  EXPECT_EQ(biometric_tries_, 2);
}

TEST_F(PinGateControllerTest, AKeyOnlyDoorTakesNoPadOrFieldEvents) {
  pin_digits_ = 4;
  biometric_kind_ = "face";
  controller_.ShowUnlock();
  ASSERT_TRUE(shown_.biometric_first);
  const int completes = completes_;
  Keys("1234");
  EXPECT_EQ(completes_, completes) << "no pad to type on";
  EXPECT_FALSE(shown_.keypad_busy);
  controller_.OnUseText();
  EXPECT_TRUE(shown_.keypad) << "the mode cannot flip behind the key-only door";
  controller_.OnSubmit();
  EXPECT_EQ(completes_, completes);
  controller_.OnBiometric();
  EXPECT_EQ(biometric_tries_, 1) << "the key itself still prompts";
}

TEST_F(PinGateControllerTest, ALostBiometricUnlockKeepsTheWaitingScreensWords) {
  pin_digits_ = 4;
  biometric_kind_ = "face";
  controller_.ShowUnlock();
  controller_.SetWaitingDeviceUnlock(true);
  controller_.ShowBiometricReset("turn it on again");
  EXPECT_EQ(std::string(shown_.message.c_str()), Tr("pin.unlock_phone_call_message"));
  EXPECT_FALSE(shown_.biometric_first);
}

TEST_F(PinGateControllerTest, TheBiometricKeyIsOnlyOnTheUnlockDoor) {
  pin_digits_ = 4;
  biometric_kind_ = "touch";
  controller_.ShowVerify(PinVerifyPurpose::EnableBiometric, Check(), [] {});
  EXPECT_FALSE(shown_.biometric_key);
  EXPECT_FALSE(shown_.biometric_first);
  controller_.ShowChange(Check(), Change());
  EXPECT_FALSE(shown_.biometric_key);
  ShowCreate();
  EXPECT_FALSE(shown_.biometric_key);
  controller_.OnBiometric();
  EXPECT_EQ(biometric_tries_, 0);
}

TEST_F(PinGateControllerTest, TheBiometricKeyWaitsWhileThePinIsChecked) {
  pin_digits_ = 4;
  biometric_kind_ = "face";
  controller_.ShowUnlock();
  controller_.OnUsePin();
  Keys("1234");
  ASSERT_TRUE(shown_.keypad_busy);
  controller_.OnBiometric();
  EXPECT_EQ(biometric_tries_, 0);
}

TEST_F(PinGateControllerTest, ALostBiometricUnlockHidesTheKeyAndSaysWhy) {
  pin_digits_ = 4;
  biometric_kind_ = "face";
  controller_.ShowUnlock();
  controller_.OnUsePin();
  Keys("12");
  controller_.ShowBiometricReset("turn it on again");
  EXPECT_FALSE(shown_.biometric_key);
  EXPECT_FALSE(shown_.biometric_first);
  EXPECT_EQ(std::string(shown_.biometric_hint.c_str()), "turn it on again");
  EXPECT_EQ(Pin(), "12") << "what was typed stays";
  controller_.OnBiometric();
  EXPECT_EQ(biometric_tries_, 0);
}

// Lost while the door is still key-only: the pad opens by itself (there is no key left to tap).
TEST_F(PinGateControllerTest, ALostBiometricUnlockOpensThePadOnAKeyOnlyDoor) {
  pin_digits_ = 4;
  biometric_kind_ = "face";
  controller_.ShowUnlock();
  ASSERT_TRUE(shown_.biometric_first);
  const int remounts = remounts_;
  controller_.ShowBiometricReset("turn it on again");
  EXPECT_FALSE(shown_.biometric_first);
  EXPECT_FALSE(shown_.biometric_key);
  EXPECT_TRUE(shown_.keypad);
  EXPECT_GT(remounts_, remounts);
  EXPECT_EQ(std::string(shown_.message.c_str()), Tr("pin.unlock_message"));
  EXPECT_EQ(std::string(shown_.biometric_hint.c_str()), "turn it on again");
}

// The reset's confirm tap, or the fork's own button, delivered twice (a trackpad's touch and the mouse click made from
// it): the second copy, landing on the page that just appeared, picks nothing there; a real tap after the guard does.
TEST_F(PinGateControllerTest, AChoicePageIgnoresTheTapThatBroughtItUp) {
  controller_.SetClockForTest([this]() { return clock_; });
  controller_.ShowIdentityFork();
  clock_ += std::chrono::milliseconds(200);
  controller_.OnIdentityNew();
  EXPECT_TRUE(shown_.identity_fork_mode) << "still the fork: not taken to \"Set a PIN?\"";
  EXPECT_FALSE(shown_.chooser_mode);
  controller_.OnIdentityLink();
  controller_.OnIdentityPhrase();
  EXPECT_TRUE(shown_.identity_fork_mode);
  clock_ += PinGateController::kChoiceTapGuard;
  controller_.OnIdentityNew();
  EXPECT_TRUE(shown_.chooser_mode) << "a real tap goes on to \"Set a PIN?\"";
  // Its own double tap does not pick "Set a PIN" or "Just continue" right away either.
  clock_ += std::chrono::milliseconds(100);
  controller_.OnSetPin();
  controller_.OnUseDefaultPin();
  EXPECT_TRUE(shown_.chooser_mode);
  EXPECT_FALSE(shown_.create_mode);
  EXPECT_EQ(completes_, 0);
  clock_ += PinGateController::kChoiceTapGuard;
  controller_.OnSetPin();
  EXPECT_TRUE(shown_.create_mode);
}

// Reset my profile in the running app, through the real unlock gate and this controller: the page shown is the fresh
// install's first page (the fork: new account / I already have an account / recovery phrase), not "Set a PIN?" —
// with the keypad (a phone) and without (a desktop window).
TEST_F(PinGateControllerTest, AfterAResetInTheRunningAppTheFreshInstallsFirstPageShows) {
  for (const bool keypad : {true, false}) {
    keypad_available_ = keypad;
    shown_ = {};
    const auto dir = std::filesystem::temp_directory_path() / ("pp_reset_fork_" + std::to_string(keypad));
    std::filesystem::remove_all(dir);
    std::filesystem::create_directories(dir);
    ProfileSecretsEngine engine;
    ASSERT_TRUE(engine.Initialize(dir.string()));
    ProfileUnlockGate gate;
    gate.BindSecrets(engine);
    bool ready = false;
    ProfileUnlockPorts ports;
    ports.messaging_initialized = []() { return true; };
    ports.messaging_ready = [&ready]() { return ready; };
    ports.ui.show_identity_fork = [this]() { controller_.ShowIdentityFork(); };
    ports.ui.show_chooser = [this]() { controller_.ShowChooser(); };
    ports.ui.show_unlock = [this]() { controller_.ShowUnlock(); };
    ports.ui.dismiss = [this]() { controller_.Dismiss(); };
    gate.BindPorts(std::move(ports));

    // A fresh install's first frame, for comparison (its own gate: one decision per process).
    {
      ProfileUnlockGate fresh_gate;
      fresh_gate.BindSecrets(engine);
      ProfileUnlockPorts fresh;
      fresh.messaging_initialized = []() { return true; };
      fresh.messaging_ready = []() { return false; };
      fresh.ui.show_identity_fork = [this]() { controller_.ShowIdentityFork(); };
      fresh.ui.show_chooser = [this]() { controller_.ShowChooser(); };
      fresh_gate.BindPorts(std::move(fresh));
      fresh_gate.BeginDeferredUnlockAfterFirstPresent();
      fresh_gate.BindPorts({});
    }
    EXPECT_TRUE(shown_.identity_fork_mode) << "fresh install, keypad=" << keypad;
    const std::string fresh_title = shown_.title.c_str();
    controller_.Dismiss();

    // An account in use (the first-frame decision found it ready), then the reset as Application does it (secrets
    // shut down, the directory wiped and made again, secrets back, messaging initialized but not ready), then its
    // EnsureUnlocked.
    ASSERT_TRUE(engine.Vault()->CreateWithDek("4321", ByteVector(kDataEncryptionKeySize, 0x52)));
    ready = true;
    gate.BeginDeferredUnlockAfterFirstPresent();
    shown_ = {};
    engine.Shutdown();
    std::filesystem::remove_all(dir);
    std::filesystem::create_directories(dir);
    ASSERT_TRUE(engine.Initialize(dir.string()));
    ready = false;
    gate.EnsureUnlocked(nullptr);
    EXPECT_TRUE(shown_.active) << "keypad=" << keypad;
    EXPECT_TRUE(shown_.identity_fork_mode) << "the fork, keypad=" << keypad;
    EXPECT_FALSE(shown_.chooser_mode) << "not \"Set a PIN?\", keypad=" << keypad;
    EXPECT_FALSE(shown_.create_mode);
    EXPECT_EQ(std::string(shown_.title.c_str()), fresh_title) << "the same page as a fresh install's";
    gate.BindPorts({});
    engine.Shutdown();
    std::filesystem::remove_all(dir);
  }
}

} // namespace
} // namespace pbr
