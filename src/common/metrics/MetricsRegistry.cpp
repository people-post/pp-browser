#include "common/metrics/MetricsRegistry.h"

#include <algorithm>
#include <cmath>
#include <iomanip>
#include <limits>
#include <sstream>
#include <string>
#include "common/PbrCompat.h"

namespace pbr {
namespace {

void AtomicAdd(std::atomic<double>& target, const double delta) {
  double current = target.load(std::memory_order_relaxed);
  while (!target.compare_exchange_weak(current, current + delta, std::memory_order_relaxed)) {
  }
}

std::string EscapeLabelValue(const std::string& value) {
  std::string out;
  for (const char c : value) {
    if (c == '\\' || c == '"') {
      out.push_back('\\');
      out.push_back(c);
    } else if (c == '\n') {
      out += "\\n";
    } else {
      out.push_back(c);
    }
  }
  return out;
}

std::string LabelText(const MetricLabels& labels, const std::string& extra_key = {}, const std::string& extra = {}) {
  if (labels.empty() && extra_key.empty()) {
    return {};
  }
  std::string out = "{";
  bool first = true;
  for (const auto& [key, value] : labels) {
    out += (first ? "" : ",") + key + "=\"" + EscapeLabelValue(value) + "\"";
    first = false;
  }
  if (!extra_key.empty()) {
    out += (first ? "" : ",") + extra_key + "=\"" + extra + "\"";
  }
  return out + "}";
}

std::string Number(const double value) {
  if (std::isinf(value)) {
    return value > 0 ? "+Inf" : "-Inf";
  }
  if (std::isnan(value)) {
    return "NaN";
  }
  if (value == std::floor(value) && std::abs(value) < 1e15) {
    std::ostringstream out;
    out << static_cast<int64_t>(value);
    return out.str();
  }
  // The shortest form that reads back as the same double (0.1, not 0.10000000000000001).
  for (int precision = 6; precision <= std::numeric_limits<double>::max_digits10; ++precision) {
    std::ostringstream out;
    out << std::setprecision(precision) << value;
    if (std::stod(out.str()) == value) {
      return out.str();
    }
  }
  std::ostringstream out;
  out << std::setprecision(std::numeric_limits<double>::max_digits10) << value;
  return out.str();
}

} // namespace

void MetricGauge::Add(const double delta) {
  AtomicAdd(value_, delta);
}

MetricHistogram::MetricHistogram(std::vector<double> bounds)
    : bounds_(std::move(bounds)), buckets_(new std::atomic<uint64_t>[bounds_.size() + 1]) {
  for (size_t i = 0; i <= bounds_.size(); ++i) {
    buckets_[i].store(0, std::memory_order_relaxed);
  }
}

void MetricHistogram::Observe(const double value) {
  const size_t index =
      static_cast<size_t>(std::lower_bound(bounds_.begin(), bounds_.end(), value) - bounds_.begin());
  buckets_[index].fetch_add(1, std::memory_order_relaxed);
  AtomicAdd(sum_, value);
  count_.fetch_add(1, std::memory_order_relaxed);
}

MetricHistogram::Snapshot MetricHistogram::Read() const {
  Snapshot snap;
  snap.bounds = bounds_;
  uint64_t running = 0;
  for (size_t i = 0; i <= bounds_.size(); ++i) {
    running += buckets_[i].load(std::memory_order_relaxed);
    snap.cumulative.push_back(running);
  }
  snap.count = running;  // consistent with the buckets (count_ may be a step ahead)
  snap.sum = sum_.load(std::memory_order_relaxed);
  return snap;
}

MetricsRegistry& MetricsRegistry::Global() {
  static MetricsRegistry registry;
  return registry;
}

MetricsRegistry::Series& MetricsRegistry::Find(const std::string& name, const std::string& help, const Type type,
                                               const MetricLabels& labels, const std::vector<double>* bounds) {
  std::lock_guard lock(mu_);
  auto [it, inserted] = families_.try_emplace(name);
  Family& family = it->second;
  if (inserted) {
    family.type = type;
    family.help = help;
  }
  for (const auto& series : family.series) {
    if (series->labels == labels) {
      return *series;
    }
  }
  auto series = std::make_unique<Series>();
  series->labels = labels;
  // A name keeps its first type; a mismatched request still gets a working (unrendered) instrument.
  series->counter = std::make_unique<MetricCounter>();
  series->gauge = std::make_unique<MetricGauge>();
  series->histogram = std::make_unique<MetricHistogram>(bounds ? *bounds : std::vector<double>{});
  family.series.push_back(std::move(series));
  return *family.series.back();
}

MetricCounter& MetricsRegistry::Counter(const std::string& name, const std::string& help, const MetricLabels& labels) {
  return *Find(name, help, Type::Counter, labels, nullptr).counter;
}

MetricGauge& MetricsRegistry::Gauge(const std::string& name, const std::string& help, const MetricLabels& labels) {
  return *Find(name, help, Type::Gauge, labels, nullptr).gauge;
}

MetricHistogram& MetricsRegistry::Histogram(const std::string& name, const std::string& help,
                                            const std::vector<double>& bounds, const MetricLabels& labels) {
  return *Find(name, help, Type::Histogram, labels, &bounds).histogram;
}

MetricsRegistry::CollectorId MetricsRegistry::AddCollector(std::function<void(MetricsRegistry&)> collect) {
  std::lock_guard lock(collect_mu_);
  const CollectorId id = ++next_collector_;
  collectors_.emplace(id, std::move(collect));
  return id;
}

void MetricsRegistry::RemoveCollector(const CollectorId id) {
  std::lock_guard lock(collect_mu_);  // waits out a scrape running it
  collectors_.erase(id);
}

std::string MetricsRegistry::RenderPrometheus() {
  {
    std::lock_guard lock(collect_mu_);
    for (auto& [id, collect] : collectors_) {
      if (collect) {
        collect(*this);
      }
    }
  }
  std::ostringstream out;
  std::lock_guard lock(mu_);
  for (const auto& [name, family] : families_) {
    const char* type = family.type == Type::Counter ? "counter" : family.type == Type::Gauge ? "gauge" : "histogram";
    out << "# HELP " << name << ' ' << family.help << '\n' << "# TYPE " << name << ' ' << type << '\n';
    for (const auto& series : family.series) {
      switch (family.type) {
      case Type::Counter:
        out << name << LabelText(series->labels) << ' ' << series->counter->Value() << '\n';
        break;
      case Type::Gauge:
        out << name << LabelText(series->labels) << ' ' << Number(series->gauge->Value()) << '\n';
        break;
      case Type::Histogram: {
        const auto snap = series->histogram->Read();
        for (size_t i = 0; i < snap.bounds.size(); ++i) {
          out << name << "_bucket" << LabelText(series->labels, "le", Number(snap.bounds[i])) << ' '
              << snap.cumulative[i] << '\n';
        }
        out << name << "_bucket" << LabelText(series->labels, "le", "+Inf") << ' ' << snap.count << '\n';
        out << name << "_sum" << LabelText(series->labels) << ' ' << Number(snap.sum) << '\n';
        out << name << "_count" << LabelText(series->labels) << ' ' << snap.count << '\n';
        break;
      }
      }
    }
  }
  return out.str();
}

} // namespace pbr
