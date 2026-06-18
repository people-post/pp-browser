#include "messaging/MessagingHub.h"

#include "agent/AgentSession.h"

#include <cstdlib>
#include <filesystem>

namespace pbr {

namespace {

std::string ResolveDataDir(const AppConfig& config) {
  if (!config.data_dir.empty()) {
    if (config.data_dir.front() == '~') {
      const char* home = std::getenv("HOME");
      if (home) {
        return std::string(home) + config.data_dir.substr(1);
      }
    }
    return config.data_dir;
  }
  const char* home = std::getenv("HOME");
  if (home) {
    return std::string(home) + "/.local/share/pp-browser";
  }
  return "./pp-browser-data";
}

} // namespace

MessagingHub& MessagingHub::Instance() {
  static MessagingHub hub;
  return hub;
}

Roe<void> MessagingHub::Initialize(const AppConfig& config) {
  if (initialized_) {
    return {};
  }

  data_dir_ = ResolveDataDir(config);
  std::error_code ec;
  std::filesystem::create_directories(data_dir_, ec);

  store_ = std::make_unique<JsonThreadStore>(data_dir_);
  contacts_ = std::make_unique<ContactsStore>(data_dir_);
  identity_ = std::make_unique<IdentityStore>(data_dir_);

  if (auto identity = identity_->LoadOrCreate()) {
    (void)identity;
  }

  inbox_ = std::make_unique<InboxController>(*store_, *contacts_);
  (void)inbox_->CreateAiHomeThread();

  mock_relay_ = std::make_unique<MockRelayClient>();
  http_relay_ = std::make_unique<HttpRelayClient>(config.relay.base_url);
  relay_ = config.relay.base_url.empty() ? static_cast<IRelayClient*>(mock_relay_.get())
                                          : static_cast<IRelayClient*>(http_relay_.get());

  mock_directory_ = std::make_unique<MockDirectoryClient>();
  http_directory_ = std::make_unique<HttpDirectoryClient>(config.directory.base_url);
  directory_ = config.directory.base_url.empty() ? static_cast<IDirectoryClient*>(mock_directory_.get())
                                                 : static_cast<IDirectoryClient*>(http_directory_.get());

  mock_registration_ = std::make_unique<MockRegistrationClient>();
  http_registration_ = std::make_unique<HttpRegistrationClient>(config.registration.base_url);
  registration_ = config.registration.base_url.empty()
                      ? static_cast<IRegistrationClient*>(mock_registration_.get())
                      : static_cast<IRegistrationClient*>(http_registration_.get());

  p2p_ = std::make_unique<P2pMessagingService>(*store_, *contacts_, *identity_, *relay_, *inbox_);
  actions_ = std::make_unique<ContactActionDispatcher>(*inbox_, *contacts_, *identity_, *registration_);

  initialized_ = true;
  return {};
}

void MessagingHub::BindAgent(AgentSession& agent) {
  router_ = std::make_unique<MessageRouter>(*inbox_, *p2p_, agent);
}

void MessagingHub::Shutdown() {
  if (!initialized_) {
    return;
  }
  router_.reset();
  store_->Flush();
  contacts_->Flush();
  identity_->Flush();
  actions_.reset();
  p2p_.reset();
  inbox_.reset();
  mock_relay_.reset();
  http_relay_.reset();
  mock_directory_.reset();
  http_directory_.reset();
  mock_registration_.reset();
  http_registration_.reset();
  identity_.reset();
  contacts_.reset();
  store_.reset();
  initialized_ = false;
}

InboxController& MessagingHub::Inbox() {
  return *inbox_;
}

P2pMessagingService& MessagingHub::P2p() {
  return *p2p_;
}

MessageRouter& MessagingHub::Router() {
  return *router_;
}

ContactActionDispatcher& MessagingHub::Actions() {
  return *actions_;
}

IThreadStore& MessagingHub::Store() {
  return *store_;
}

ContactsStore& MessagingHub::Contacts() {
  return *contacts_;
}

IdentityStore& MessagingHub::Identity() {
  return *identity_;
}

IDirectoryClient& MessagingHub::Directory() {
  return *directory_;
}

IRegistrationClient& MessagingHub::Registration() {
  return *registration_;
}

} // namespace pbr
