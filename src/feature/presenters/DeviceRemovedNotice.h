#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <utility>

namespace pbr {

/** What the Chats page's removed-device notice needs; Application wires it to the facade and to Me → Storage. */
struct DeviceRemovedNoticePorts {
  /**
   * Whether the account's newest device list leaves this device out (OwnDeviceList::signed_out, M021 d4). False on the
   * main device, on a listed linked device, and while messaging is not up. Blocks: run through `post_worker`.
   */
  std::function<bool()> signed_out;
  /** Asks to be called (any thread) whenever this device's list changes; a null function clears it. */
  std::function<void(std::function<void()> changed)> watch;
  /**
   * "Link again": the existing reset flow — its confirm, then Me → Storage → Reset my profile (which on a linked device
   * keeps the account registered) — landing on the first-run page, where "I already have an account" takes a linking
   * code from the main device.
   */
  std::function<void()> relink;
  /** Where the check runs, and where its answer is applied (the app: a worker, the UI thread). Unset: at once. */
  std::function<void(std::function<void()>)> post_worker;
  std::function<void(std::function<void()>)> post_ui;
};

/**
 * The Chats page's notice for a linked device its account took off its device list: it may still receive, but nothing
 * it sends goes out (OwnSender refuses with SignedOutSendError, whose pill says so). Shown as soon as the device
 * knows — at every messaging start, and whenever its list changes — not only after a failed send; never as a dialog.
 * "OK" hides it until the next launch (in memory); "Link again" hands over to the reset flow. It goes when the device
 * is listed again (or after the reset).
 *
 * Shell bindings (gui/chat/ChatDataModel; the phone's sidebar.rml, and the desktop's view): `device_removed_notice`
 * (bool), events `device_removed_relink()` and `device_removed_ok()`; words `device_removed.body`,
 * `device_removed.relink`, `device_removed.ok`.
 */
class DeviceRemovedNotice {
public:
  /** `changed`: the notice's visibility changed (on the UI thread): redraw. */
  explicit DeviceRemovedNotice(std::function<void()> changed) : changed_(std::move(changed)) {}

  void BindPorts(DeviceRemovedNoticePorts ports) { ports_ = std::move(ports); }

  /** Asks again whether this device is on its list (and listens for the next change); the answer applies later. */
  void Check() {
    if (!ports_.signed_out) {
      return;
    }
    std::weak_ptr<bool> alive = alive_;
    if (ports_.watch) {
      // Set again at every check: the ports may have been bound anew (a profile reset).
      ports_.watch([this, alive, post_ui = ports_.post_ui]() {
        Post(post_ui, [this, alive]() {
          if (alive.lock()) {
            Check();
          }
        });
      });
    }
    const uint64_t asked = ++asked_;
    Post(ports_.post_worker, [this, alive, asked, read = ports_.signed_out, post_ui = ports_.post_ui]() {
      const bool out = read();
      Post(post_ui, [this, alive, asked, out]() {
        if (alive.lock() && asked == asked_) {
          Apply(out);
        }
      });
    });
  }

  /** What was asked last: this device is off its list (true), or listed / unknown (false). */
  void Apply(const bool signed_out) {
    const bool before = Shows();
    signed_out_ = signed_out;
    if (Shows() != before && changed_) {
      changed_();
    }
  }

  /** Messaging went down (a lock, a profile reset): nothing known until the next Check. "OK" stays for this launch. */
  void Forget() {
    ++asked_; // an answer still on its way is for the old session
    Apply(false);
  }

  bool Shows() const { return signed_out_ && !dismissed_; }

  /** "OK": hidden until the next launch. */
  void Ok() {
    const bool before = Shows();
    dismissed_ = true;
    if (before && changed_) {
      changed_();
    }
  }

  /** "Link again": the reset flow takes over (it asks first); the notice stays until the device is listed again. */
  void Relink() {
    if (signed_out_ && ports_.relink) {
      ports_.relink();
    }
  }

private:
  static void Post(const std::function<void(std::function<void()>)>& post, std::function<void()> task) {
    if (post) {
      post(std::move(task));
    } else {
      task();
    }
  }

  std::function<void()> changed_;
  DeviceRemovedNoticePorts ports_;
  std::shared_ptr<bool> alive_ = std::make_shared<bool>(true);
  uint64_t asked_ = 0;
  bool signed_out_ = false;
  bool dismissed_ = false;
};

} // namespace pbr
