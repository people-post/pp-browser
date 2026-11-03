#include "lib/media/ThreadCpu.h"

#include <algorithm>
#include <map>

namespace pbr {

std::string_view CanonicalThreadName(const std::string_view name) {
  for (const ShareWord& word : kKnownThreadNames) {
    if (word.view() == name) {
      return word.view();
    }
  }
  return "other";
}

namespace {

std::string OwnThreadName(const std::string_view name) {
  std::string out;
  for (const char c : name) {
    if (out.size() >= 32) {
      break;
    }
    const bool ok = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '.' ||
                    c == '_' || c == '-';
    out.push_back(ok ? c : '_');
  }
  return out.empty() ? std::string("unnamed") : out;
}

template <typename NameOf>
std::vector<ThreadCpuDelta> TopDeltas(const std::vector<ThreadCpuSample>& now,
                                      std::unordered_map<uint32_t, double>& previous, const std::size_t top_n,
                                      NameOf name_of) {
  const bool have_baseline = !previous.empty();
  std::map<std::string, double> by_name;
  std::unordered_map<uint32_t, double> next;
  next.reserve(now.size());
  for (const ThreadCpuSample& t : now) {
    next[t.id] = t.cpu_s;
    if (!have_baseline) {
      continue;
    }
    const auto it = previous.find(t.id);
    const double delta_s = it == previous.end() ? t.cpu_s : t.cpu_s - it->second;
    if (delta_s > 0.0) {
      by_name[name_of(t.name)] += delta_s * 1000.0;
    }
  }
  previous = std::move(next);

  std::vector<ThreadCpuDelta> out;
  out.reserve(by_name.size());
  for (const auto& [name, ms] : by_name) {
    out.push_back({name, ms});
  }
  std::stable_sort(out.begin(), out.end(), [](const ThreadCpuDelta& a, const ThreadCpuDelta& b) { return a.ms > b.ms; });
  if (out.size() > top_n) {
    out.resize(top_n);
  }
  return out;
}

} // namespace

std::vector<ThreadCpuDelta> TopThreadCpuDeltas(const std::vector<ThreadCpuSample>& now,
                                               std::unordered_map<uint32_t, double>& previous,
                                               const std::size_t top_n) {
  return TopDeltas(now, previous, top_n,
                   [](const std::string& name) { return std::string(CanonicalThreadName(name)); });
}

std::vector<ThreadCpuDelta> TopThreadCpuDeltasByOwnName(const std::vector<ThreadCpuSample>& now,
                                                        std::unordered_map<uint32_t, double>& previous,
                                                        const std::size_t top_n) {
  return TopDeltas(now, previous, top_n, [](const std::string& name) { return OwnThreadName(name); });
}

} // namespace pbr
