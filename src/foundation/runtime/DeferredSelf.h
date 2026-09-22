#pragma once

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <utility>

namespace pbr {

/**
 * Generation ticket for deferred work that captures raw `this` / Impl*.
 *
 * Invalidate bumps the generation so already-queued callbacks no-op — no queue yank.
 * Prefer Post / Bind over capturing raw self into an unguarded PostToIo.
 *
 * Whitelist + rules: docs/architecture/OWNERSHIP.md § DeferredSelf.
 */
class DeferredSelf {
public:
  using Token = std::shared_ptr<std::atomic<uint64_t>>;

  DeferredSelf() : gen_(std::make_shared<std::atomic<uint64_t>>(0)) {}

  Token token() const { return gen_; }

  uint64_t Snapshot() const {
    return gen_ ? gen_->load(std::memory_order_acquire) : 0;
  }

  /** Bump generation; queued Bind/Post callbacks with the prior snap become no-ops. */
  void Invalidate() {
    if (gen_) {
      gen_->fetch_add(1, std::memory_order_acq_rel);
    }
  }

  static bool Alive(const Token& token, const uint64_t snap) {
    return token && token->load(std::memory_order_acquire) == snap;
  }

  bool Alive(const uint64_t snap) const { return Alive(gen_, snap); }

  /** Wrap `fn` so it no-ops after Invalidate (captures token + snap at bind time). */
  template <typename F>
  auto Bind(F&& fn) const {
    Token tok = gen_;
    const uint64_t snap = Snapshot();
    // shared_ptr so the returned callable is const-callable (nested Bind / std::function)
    // while still allowing mutable bound functors.
    auto held = std::make_shared<std::decay_t<F>>(std::forward<F>(fn));
    return [tok, snap, held](auto&&... args) {
      if (!Alive(tok, snap)) {
        return;
      }
      std::invoke(*held.get(), std::forward<decltype(args)>(args)...);
    };
  }

  /**
   * Exclusive post helper: Bind then deliver via `post`.
   * Use for any deferred work that may touch the owner after Stop / Clear.
   */
  template <typename PostFn>
  void Post(PostFn&& post, std::function<void()> task) const {
    if (!task) {
      return;
    }
    std::forward<PostFn>(post)(Bind(std::move(task)));
  }

private:
  Token gen_;
};

} // namespace pbr
