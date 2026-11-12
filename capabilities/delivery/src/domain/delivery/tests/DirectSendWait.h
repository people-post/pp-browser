#pragma once

#include "common/chat/IDirectMessageClient.h"

#include <chrono>
#include <functional>
#include <future>
#include <memory>
#include <string>
#include <thread>

namespace pbr::test {

/**
 * Send over the product's asynchronous path and wait for its outcome, driving `pump` (the test's io) meanwhile.
 * Past `budget` with no outcome: a NotConfirmed failure.
 */
inline DirectSendRoe SendAndWait(IDirectMessageClient& chat, const std::string& key, const RelayEnvelope& envelope,
                                 const std::function<void()>& pump,
                                 const std::chrono::milliseconds budget = std::chrono::seconds(5)) {
  auto done = std::make_shared<std::promise<DirectSendRoe>>();
  auto outcome = done->get_future();
  chat.SendEnvelopeAsync(key, envelope, [done](DirectSendRoe value) { done->set_value(std::move(value)); });
  const auto deadline = std::chrono::steady_clock::now() + budget;
  while (outcome.wait_for(std::chrono::milliseconds(0)) != std::future_status::ready &&
         std::chrono::steady_clock::now() < deadline) {
    if (pump) {
      pump();
    } else {
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
  }
  if (outcome.wait_for(std::chrono::milliseconds(0)) != std::future_status::ready) {
    return DirectSendFailure::Of(DirectSendErr::NotConfirmed, "test: no outcome in time");
  }
  return outcome.get();
}

} // namespace pbr::test
