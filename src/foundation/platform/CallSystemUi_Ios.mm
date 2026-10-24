// CallKit + PushKit: the phone's own call screen for incoming calls (P013).
//
// A VoIP push (`type: call_wake`, sent by the relay for an urgent send) puts "Incoming call" on the
// CallKit screen at once — iOS requires it for every VoIP push — and wakes the app to sync; the
// app then fills that call in with the ring it finds (CallSystemUiBridge → ReportIncoming). Built
// with MRC: objects kept for the process are never released.

#include "foundation/platform/IosCallSystemUi.h"

#include "foundation/platform/CallSystemUi.h"
#include "foundation/platform/IPushDeviceRegistrar.h"
#include "foundation/runtime/BackgroundSyncScheduler.h"

#import <AVFoundation/AVFoundation.h>
#import <CallKit/CallKit.h>
#import <Foundation/Foundation.h>
#import <PushKit/PushKit.h>

#include <mutex>
#include <string>
#include <unordered_map>
#include <utility>

namespace pbr {
namespace {

/** The push's call waits this long for its ring; answered, this long more (the app may be locked). */
constexpr int64_t kPendingRingNs = 20 * NSEC_PER_SEC;
constexpr int64_t kPendingAnsweredNs = 45 * NSEC_PER_SEC;

std::mutex g_voip_token_mu;
std::string g_voip_token;

NSString* ToNs(const std::string& s) {
  return [NSString stringWithUTF8String:s.c_str()];
}

std::string HexOf(NSData* data) {
  static const char* kHex = "0123456789abcdef";
  const auto* bytes = static_cast<const unsigned char*>(data.bytes);
  std::string hex;
  hex.reserve(data.length * 2);
  for (NSUInteger i = 0; i < data.length; ++i) {
    hex.push_back(kHex[(bytes[i] >> 4) & 0xF]);
    hex.push_back(kHex[bytes[i] & 0xF]);
  }
  return hex;
}

/** Words for the push's call before its caller is known (the bundle's Localizable.strings). */
NSString* UnknownCallerName() {
  return [[NSBundle mainBundle] localizedStringForKey:@"CALL_INCOMING_TITLE" value:@"Incoming call" table:nil];
}

/** Main thread only (CallKit's and PushKit's delegate queue, and the app's UI thread). */
class IosCallSystemUi final : public ICallSystemUi {
public:
  void Start(id<CXProviderDelegate> delegate) {
    CXProviderConfiguration* config = [[CXProviderConfiguration alloc] init];
    config.supportsVideo = YES;
    config.maximumCallGroups = 1;
    config.maximumCallsPerCallGroup = 1;
    config.supportedHandleTypes = [NSSet setWithObject:@(CXHandleTypeGeneric)];
    // Calls stay out of the phone's recents (and its iCloud call history).
    config.includesCallsInRecents = NO;
    provider_ = [[CXProvider alloc] initWithConfiguration:config];
    [provider_ setDelegate:delegate queue:nil];
  }

  bool Available() const override { return provider_ != nil; }
  void SetCallbacks(Callbacks callbacks) override { callbacks_ = std::move(callbacks); }
  bool HasPendingPushCall() const override { return pending_ != nil; }

  void ReportIncoming(const std::string& call_id, const std::string& caller_name, const bool video) override {
    CXCallUpdate* update = Update(caller_name, video);
    if (auto it = calls_.find(call_id); it != calls_.end()) {
      [provider_ reportCallWithUUID:it->second updated:update];
      return;
    }
    if (pending_ != nil) {
      // The push's call is this ring: it takes the caller's name (and pending_'s retain).
      NSUUID* uuid = pending_;
      pending_ = nil;
      ++pending_generation_;
      calls_[call_id] = uuid;
      [provider_ reportCallWithUUID:uuid updated:update];
      return;
    }
    NSUUID* uuid = [[NSUUID alloc] init];
    calls_[call_id] = uuid;
    const std::string reported = call_id;
    [provider_ reportNewIncomingCallWithUUID:uuid
                                      update:update
                                  completion:^(NSError* error) {
                                    if (error != nil) {
                                      // Do Not Disturb, a blocked number, a call already up…
                                      NSLog(@"CallKit: incoming call not shown: %@", error);
                                      Forget(reported);
                                    }
                                  }];
  }

  void ReportEnded(const std::string& call_id, const EndReason reason) override {
    auto it = calls_.find(call_id);
    if (it == calls_.end()) {
      return;
    }
    CXCallEndedReason ended = CXCallEndedReasonRemoteEnded;
    if (reason == EndReason::AnsweredElsewhere) {
      ended = CXCallEndedReasonAnsweredElsewhere;
    } else if (reason == EndReason::Failed) {
      ended = CXCallEndedReasonFailed;
    }
    [provider_ reportCallWithUUID:it->second endedAtDate:nil reason:ended];
    Forget(call_id);
  }

  // --- the push ------------------------------------------------------------------------------

  /** A VoIP push: "Incoming call" on the CallKit screen now (iOS requires it), then wake the app. */
  void OnCallPush(void (^completion)(void)) {
    NSUUID* uuid = [[NSUUID alloc] init];
    const bool busy = pending_ != nil || !calls_.empty();
    [provider_ reportNewIncomingCallWithUUID:uuid
                                      update:Update({}, false)
                                  completion:^(NSError* error) {
                                    if (error != nil) {
                                      NSLog(@"CallKit: push call not shown: %@", error);
                                      if (pending_ != nil && [pending_ isEqual:uuid]) {
                                        ClearPending();
                                      }
                                    }
                                    if (busy && error == nil) {
                                      // A call is already on the screen (an earlier push, the
                                      // ring itself): this push's call goes at once.
                                      [provider_ reportCallWithUUID:uuid
                                                        endedAtDate:nil
                                                             reason:CXCallEndedReasonFailed];
                                    }
                                    [uuid release];
                                    if (completion) {
                                      completion();
                                    }
                                  }];
    if (!busy) {
      pending_ = [uuid retain];
      ArmPendingTimer(kPendingRingNs);
    }
    BackgroundSyncScheduler::Instance().RequestCallWakeSync();
  }

  // --- CXProviderDelegate ----------------------------------------------------------------------

  void OnReset() {
    for (auto& entry : calls_) {
      [entry.second release];
    }
    calls_.clear();
    ClearPending();
  }

  void OnAnswer(CXAnswerCallAction* action) {
    ConfigureAudioSession();
    if (pending_ != nil && [action.callUUID isEqual:pending_]) {
      ArmPendingTimer(kPendingAnsweredNs);
      Call(callbacks_.answer, std::string());
    } else if (const std::string call_id = IdOf(action.callUUID); !call_id.empty()) {
      Call(callbacks_.answer, call_id);
    }
    [action fulfill];
  }

  void OnEnd(CXEndCallAction* action) {
    if (pending_ != nil && [action.callUUID isEqual:pending_]) {
      ClearPending();
      Call(callbacks_.end, std::string());
    } else if (const std::string call_id = IdOf(action.callUUID); !call_id.empty()) {
      Forget(call_id);
      Call(callbacks_.end, call_id);
    }
    [action fulfill];
  }

  void OnSetMuted(CXSetMutedCallAction* action) {
    if (const std::string call_id = IdOf(action.callUUID); !call_id.empty() && callbacks_.set_muted) {
      callbacks_.set_muted(call_id, action.muted ? true : false);
    }
    [action fulfill];
  }

private:
  static CXCallUpdate* Update(const std::string& caller_name, const bool video) {
    CXCallUpdate* update = [[[CXCallUpdate alloc] init] autorelease];
    // No identity in the system's call records: a generic handle, the name only on screen.
    update.remoteHandle = [[[CXHandle alloc] initWithType:CXHandleTypeGeneric value:@"pp"] autorelease];
    update.localizedCallerName = caller_name.empty() ? UnknownCallerName() : ToNs(caller_name);
    update.hasVideo = video ? YES : NO;
    update.supportsHolding = NO;
    update.supportsGrouping = NO;
    update.supportsUngrouping = NO;
    update.supportsDTMF = NO;
    return update;
  }

  /** CallKit activates the session after an answer: it must already be a voice call's. */
  static void ConfigureAudioSession() {
    AVAudioSession* session = [AVAudioSession sharedInstance];
    NSError* error = nil;
    [session setCategory:AVAudioSessionCategoryPlayAndRecord
                    mode:AVAudioSessionModeVoiceChat
                 options:AVAudioSessionCategoryOptionAllowBluetoothA2DP
                   error:&error];
    if (error != nil) {
      NSLog(@"CallKit: audio session: %@", error);
    }
  }

  static void Call(const std::function<void(const std::string&)>& fn, const std::string& call_id) {
    if (fn) {
      fn(call_id);
    }
  }

  std::string IdOf(NSUUID* uuid) const {
    for (const auto& entry : calls_) {
      if ([entry.second isEqual:uuid]) {
        return entry.first;
      }
    }
    return {};
  }

  void Forget(const std::string& call_id) {
    if (auto it = calls_.find(call_id); it != calls_.end()) {
      [it->second release];
      calls_.erase(it);
    }
  }

  void ClearPending() {
    [pending_ release];
    pending_ = nil;
    ++pending_generation_;
  }

  /** The push's call goes (failed) if its ring has not shown by then. */
  void ArmPendingTimer(const int64_t delay_ns) {
    const uint64_t generation = ++pending_generation_;
    dispatch_after(dispatch_time(DISPATCH_TIME_NOW, delay_ns), dispatch_get_main_queue(), ^{
      if (generation != pending_generation_ || pending_ == nil) {
        return;
      }
      NSLog(@"CallKit: the push's call found no ring");
      [provider_ reportCallWithUUID:pending_ endedAtDate:nil reason:CXCallEndedReasonFailed];
      ClearPending();
    });
  }

  CXProvider* provider_ = nil;
  Callbacks callbacks_;
  /** Calls on the CallKit screen, by call id (retained). */
  std::unordered_map<std::string, NSUUID*> calls_;
  /** The push's call, its ring not found yet (retained); nil: none. */
  NSUUID* pending_ = nil;
  uint64_t pending_generation_ = 0;
};

IosCallSystemUi& Ui() {
  static IosCallSystemUi ui;
  return ui;
}

} // namespace
} // namespace pbr

@interface PpCallKitDelegate : NSObject <CXProviderDelegate, PKPushRegistryDelegate>
@end

@implementation PpCallKitDelegate

- (void)providerDidReset:(CXProvider*)provider {
  (void)provider;
  pbr::Ui().OnReset();
}

- (void)provider:(CXProvider*)provider performAnswerCallAction:(CXAnswerCallAction*)action {
  (void)provider;
  pbr::Ui().OnAnswer(action);
}

- (void)provider:(CXProvider*)provider performEndCallAction:(CXEndCallAction*)action {
  (void)provider;
  pbr::Ui().OnEnd(action);
}

- (void)provider:(CXProvider*)provider performSetMutedCallAction:(CXSetMutedCallAction*)action {
  (void)provider;
  pbr::Ui().OnSetMuted(action);
}

- (void)provider:(CXProvider*)provider performSetHeldCallAction:(CXSetHeldCallAction*)action {
  (void)provider;
  [action fail];
}

- (void)provider:(CXProvider*)provider didActivateAudioSession:(AVAudioSession*)audioSession {
  (void)provider;
  (void)audioSession;
  NSLog(@"CallKit: audio session activated");
}

- (void)provider:(CXProvider*)provider didDeactivateAudioSession:(AVAudioSession*)audioSession {
  (void)provider;
  (void)audioSession;
  NSLog(@"CallKit: audio session deactivated");
}

- (void)pushRegistry:(PKPushRegistry*)registry
    didUpdatePushCredentials:(PKPushCredentials*)credentials
                     forType:(PKPushType)type {
  (void)registry;
  if (![type isEqualToString:PKPushTypeVoIP] || credentials.token.length == 0) {
    return;
  }
  const std::string token = pbr::HexOf(credentials.token);
  {
    std::lock_guard lock(pbr::g_voip_token_mu);
    pbr::g_voip_token = token;
  }
  pbr::IPushDeviceRegistrar::NotifyTokenChanged(token);  // registers it with the relay
}

- (void)pushRegistry:(PKPushRegistry*)registry didInvalidatePushTokenForType:(PKPushType)type {
  (void)registry;
  if ([type isEqualToString:PKPushTypeVoIP]) {
    std::lock_guard lock(pbr::g_voip_token_mu);
    pbr::g_voip_token.clear();
  }
}

- (void)pushRegistry:(PKPushRegistry*)registry
    didReceiveIncomingPushWithPayload:(PKPushPayload*)payload
                              forType:(PKPushType)type
                withCompletionHandler:(void (^)(void))completion {
  (void)registry;
  (void)payload;
  if (![type isEqualToString:PKPushTypeVoIP]) {
    completion();
    return;
  }
  pbr::Ui().OnCallPush(completion);
}

@end

namespace pbr {

void InstallIosCallSystemUi() {
  static bool installed = false;
  if (installed) {
    return;
  }
  installed = true;
  PpCallKitDelegate* delegate = [[PpCallKitDelegate alloc] init];  // kept for the process
  Ui().Start(delegate);
  ICallSystemUi::SetInstance(&Ui());
  PKPushRegistry* registry = [[PKPushRegistry alloc] initWithQueue:dispatch_get_main_queue()];
  registry.delegate = delegate;
  registry.desiredPushTypes = [NSSet setWithObject:PKPushTypeVoIP];
}

std::string IosCachedVoipToken() {
  std::lock_guard lock(g_voip_token_mu);
  return g_voip_token;
}

} // namespace pbr
