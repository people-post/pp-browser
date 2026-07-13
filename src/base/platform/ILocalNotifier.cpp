#include "base/platform/ILocalNotifier.h"

namespace pbr {

namespace {

class NullLocalNotifier final : public ILocalNotifier {
public:
  void NotifyIncoming(const std::string& /*title*/, const std::string& /*body*/,
                      const std::string& /*thread_id*/) override {}
  void ClearForThread(const std::string& /*thread_id*/) override {}
};

ILocalNotifier* g_local_notifier = nullptr;
NullLocalNotifier g_null_local_notifier;

} // namespace

ILocalNotifier& ILocalNotifier::Instance() {
  return g_local_notifier ? *g_local_notifier : g_null_local_notifier;
}

void ILocalNotifier::SetInstance(ILocalNotifier* notifier) {
  g_local_notifier = notifier;
}

} // namespace pbr
