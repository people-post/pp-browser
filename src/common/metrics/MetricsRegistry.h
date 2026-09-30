#pragma once

#include <atomic>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <utility>
#include <vector>
#include "common/PbrCompat.h"

namespace pbr {

/**
 * Operator metrics (projects/node-monitoring; names in docs/contracts/NODE_METRICS.md): counters,
 * gauges and fixed-bucket histograms, rendered as Prometheus text for `/metrics`. Instruments are
 * registered once and then updated lock-free from any thread; components only update them (no
 * threads, no timers). Labels are fixed at registration and low-cardinality — never peer ids,
 * account ids, addresses or content (the `Metrics.h` rule).
 */
using MetricLabels = std::vector<std::pair<std::string, std::string>>;

class MetricCounter {
public:
  void Inc(uint64_t n = 1) { value_.fetch_add(n, std::memory_order_relaxed); }
  /** A collector mirroring a cumulative count kept elsewhere (e.g. DHT stats): set its total. */
  void Mirror(uint64_t total) { value_.store(total, std::memory_order_relaxed); }
  uint64_t Value() const { return value_.load(std::memory_order_relaxed); }

private:
  std::atomic<uint64_t> value_{0};
};

class MetricGauge {
public:
  void Set(double value) { value_.store(value, std::memory_order_relaxed); }
  void Add(double delta);
  double Value() const { return value_.load(std::memory_order_relaxed); }

private:
  std::atomic<double> value_{0.0};
};

class MetricHistogram {
public:
  /** `bounds`: ascending upper bounds (the +Inf bucket is implicit). */
  explicit MetricHistogram(std::vector<double> bounds);
  void Observe(double value);

  struct Snapshot {
    std::vector<double> bounds;
    /** Cumulative counts per bound, then +Inf (= count). */
    std::vector<uint64_t> cumulative;
    double sum = 0.0;
    uint64_t count = 0;
  };
  Snapshot Read() const;

private:
  const std::vector<double> bounds_;
  std::unique_ptr<std::atomic<uint64_t>[]> buckets_;  // per bound, then +Inf (not cumulative)
  std::atomic<double> sum_{0.0};
  std::atomic<uint64_t> count_{0};
};

class MetricsRegistry {
public:
  /** The process's registry. */
  static MetricsRegistry& Global();

  MetricsRegistry() = default;
  MetricsRegistry(const MetricsRegistry&) = delete;
  MetricsRegistry& operator=(const MetricsRegistry&) = delete;

  /**
   * The instrument for `name` + `labels`, created on first use (stable reference for the process).
   * A name keeps the type and help it was first registered with.
   */
  MetricCounter& Counter(const std::string& name, const std::string& help, const MetricLabels& labels = {});
  MetricGauge& Gauge(const std::string& name, const std::string& help, const MetricLabels& labels = {});
  MetricHistogram& Histogram(const std::string& name, const std::string& help, const std::vector<double>& bounds,
                             const MetricLabels& labels = {});

  using CollectorId = uint64_t;
  /**
   * Run at every scrape, before rendering: sets gauges from state that already exists as a snapshot
   * (relay load, DHT stats, process). Runs on the scraping thread — it must be thread-safe, and
   * must not add or remove collectors.
   */
  CollectorId AddCollector(std::function<void(MetricsRegistry&)> collect);
  /** After this returns, the collector no longer runs (it may capture an object about to go). */
  void RemoveCollector(CollectorId id);

  /** Prometheus text exposition format 0.0.4. */
  std::string RenderPrometheus();

private:
  enum class Type { Counter, Gauge, Histogram };
  struct Series {
    MetricLabels labels;
    std::unique_ptr<MetricCounter> counter;
    std::unique_ptr<MetricGauge> gauge;
    std::unique_ptr<MetricHistogram> histogram;
  };
  struct Family {
    Type type = Type::Counter;
    std::string help;
    std::vector<std::unique_ptr<Series>> series;
  };
  Series& Find(const std::string& name, const std::string& help, Type type, const MetricLabels& labels,
               const std::vector<double>* bounds);

  std::mutex mu_;
  std::map<std::string, Family> families_;  // sorted: stable output
  /** Held while collectors run and while one is removed. */
  std::mutex collect_mu_;
  std::map<CollectorId, std::function<void(MetricsRegistry&)>> collectors_;
  CollectorId next_collector_ = 0;
};

/** Removes a collector when it goes (hold it next to what the collector reads). */
class ScopedMetricsCollector {
public:
  ScopedMetricsCollector() = default;
  ScopedMetricsCollector(MetricsRegistry& registry, std::function<void(MetricsRegistry&)> collect)
      : registry_(&registry), id_(registry.AddCollector(std::move(collect))) {}
  ~ScopedMetricsCollector() { Reset(); }
  ScopedMetricsCollector(const ScopedMetricsCollector&) = delete;
  ScopedMetricsCollector& operator=(const ScopedMetricsCollector&) = delete;
  ScopedMetricsCollector(ScopedMetricsCollector&& other) noexcept { *this = std::move(other); }
  ScopedMetricsCollector& operator=(ScopedMetricsCollector&& other) noexcept {
    if (this != &other) {
      Reset();
      registry_ = std::exchange(other.registry_, nullptr);
      id_ = std::exchange(other.id_, 0);
    }
    return *this;
  }
  void Reset() {
    if (registry_) {
      registry_->RemoveCollector(id_);
      registry_ = nullptr;
    }
  }

private:
  MetricsRegistry* registry_ = nullptr;
  MetricsRegistry::CollectorId id_ = 0;
};

} // namespace pbr
