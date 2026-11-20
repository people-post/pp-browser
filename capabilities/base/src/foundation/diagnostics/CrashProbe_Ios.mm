#include "foundation/diagnostics/CrashProbe.h"

#include "common/Logger.h"
#include "common/PbrCompat.h"
#include "foundation/diagnostics/CrashDump.h"

#import <Foundation/Foundation.h>
#import <MetricKit/MetricKit.h>

#include <ctime>
#include <filesystem>
#include <string>

namespace {

std::string g_dir;

NSString* NowStamp() {
  NSDateFormatter* f = [[NSDateFormatter alloc] init];
  f.dateFormat = @"yyyyMMdd-HHmmss";
  f.locale = [NSLocale localeWithLocaleIdentifier:@"en_US_POSIX"];
  return [f stringFromDate:[NSDate date]];
}

NSString* DateText(NSDate* d) {
  return d ? [NSString stringWithFormat:@"%.0f", d.timeIntervalSince1970] : @"-";
}

void LogLine(const std::string& text) { pbr::logging::getLogger("CrashProbe").warning << "probe: " << text; }

/** Full JSON of one payload, byte for byte, into probe/<arrival>-<kind>.json. Returns the file name. */
void SaveJson(NSData* json, NSString* kind) {
  if (json == nil) {
    LogLine(std::string("payload ") + kind.UTF8String + " has no JSON");
    return;
  }
  NSString* name = [NSString stringWithFormat:@"%@-%@.json", NowStamp(), kind];
  NSString* path = [NSString stringWithUTF8String:(std::filesystem::path(g_dir) / name.UTF8String).c_str()];
  [json writeToFile:path atomically:YES];
}

int Count(NSArray* a) { return a ? (int)a.count : 0; }

} // namespace

@interface PpCrashProbeSubscriber : NSObject <MXMetricManagerSubscriber>
@end

@implementation PpCrashProbeSubscriber

- (void)didReceiveDiagnosticPayloads:(NSArray<MXDiagnosticPayload*>*)payloads {
  for (MXDiagnosticPayload* p in payloads) {
    SaveJson([p JSONRepresentation], @"diagnostic");
    LogLine(std::string("diagnostic payload from=") + DateText(p.timeStampBegin).UTF8String +
            " to=" + DateText(p.timeStampEnd).UTF8String + " crash=" + std::to_string(Count(p.crashDiagnostics)) +
            " hang=" + std::to_string(Count(p.hangDiagnostics)) + " cpu=" + std::to_string(Count(p.cpuExceptionDiagnostics)) +
            " diskwrite=" + std::to_string(Count(p.diskWriteExceptionDiagnostics)));
  }
}

- (void)didReceiveMetricPayloads:(NSArray<MXMetricPayload*>*)payloads {
  for (MXMetricPayload* p in payloads) {
    SaveJson([p JSONRepresentation], @"metric");
    std::string exits;
    MXAppExitMetric* e = p.applicationExitMetrics;
    if (e != nil) {
      MXForegroundExitData* fg = e.foregroundExitData;
      MXBackgroundExitData* bg = e.backgroundExitData;
      exits = " fg_normal=" + std::to_string((long)fg.cumulativeNormalAppExitCount) +
              " fg_abnormal=" + std::to_string((long)fg.cumulativeAbnormalExitCount) +
              " fg_memory_limit=" + std::to_string((long)fg.cumulativeMemoryResourceLimitExitCount) +
              " fg_watchdog=" + std::to_string((long)fg.cumulativeAppWatchdogExitCount) +
              " fg_bad_access=" + std::to_string((long)fg.cumulativeBadAccessExitCount) +
              " fg_illegal_instr=" + std::to_string((long)fg.cumulativeIllegalInstructionExitCount) +
              " bg_normal=" + std::to_string((long)bg.cumulativeNormalAppExitCount) +
              " bg_abnormal=" + std::to_string((long)bg.cumulativeAbnormalExitCount) +
              " bg_memory_limit=" + std::to_string((long)bg.cumulativeMemoryResourceLimitExitCount) +
              " bg_watchdog=" + std::to_string((long)bg.cumulativeAppWatchdogExitCount) +
              " bg_task_timeout=" + std::to_string((long)bg.cumulativeBackgroundTaskAssertionTimeoutExitCount) +
              " bg_cpu_limit=" + std::to_string((long)bg.cumulativeCPUResourceLimitExitCount) +
              " bg_suspended_locked=" + std::to_string((long)bg.cumulativeSuspendedWithLockedFileExitCount);
    } else {
      exits = " exits=none";
    }
    LogLine(std::string("metric payload from=") + DateText(p.timeStampBegin).UTF8String +
            " to=" + DateText(p.timeStampEnd).UTF8String + exits);
  }
}

@end

namespace pbr {

void CrashProbe::Start(const std::string& data_dir) {
  g_dir = ProbeDir(data_dir);
  std::error_code ec;
  std::filesystem::create_directories(g_dir, ec);

  // Time since the previous launch: a marker file's mtime, touched on every launch.
  const std::filesystem::path marker = std::filesystem::path(g_dir) / "last_launch.marker";
  std::string since = "first";
  const auto prev = std::filesystem::last_write_time(marker, ec);
  if (!ec) {
    const auto secs = std::chrono::duration_cast<std::chrono::seconds>(std::filesystem::file_time_type::clock::now() - prev);
    since = std::to_string(secs.count()) + "s";
  }
  [[NSData data] writeToFile:[NSString stringWithUTF8String:marker.c_str()] atomically:YES];

  MXMetricManager* manager = [MXMetricManager sharedManager];
  static PpCrashProbeSubscriber* subscriber = [[PpCrashProbeSubscriber alloc] init];
  [manager addSubscriber:subscriber];

  const bool pending = CrashDump::HasPending(data_dir);
  const int past_diag = Count([manager pastDiagnosticPayloads]);
  const int past_metric = Count([manager pastPayloads]);
  const Status status = ReadStatus(data_dir);
  LogLine("launch since_previous=" + since + " crash_pending_txt=" + (pending ? "yes" : "no") +
          " past_diagnostic_payloads=" + std::to_string(past_diag) + " past_metric_payloads=" + std::to_string(past_metric) +
          " stored_files=" + std::to_string(status.payloads));
  // Past payloads (delivered while nobody was subscribed, or again): kept too, marked "past-", for the probe only.
  for (MXDiagnosticPayload* p in [manager pastDiagnosticPayloads]) {
    SaveJson([p JSONRepresentation], @"past-diagnostic");
  }
  for (MXMetricPayload* p in [manager pastPayloads]) {
    SaveJson([p JSONRepresentation], @"past-metric");
  }
}

} // namespace pbr
