#include "common/metrics/MetricsRegistry.h"

#include <gtest/gtest.h>

#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace pbr {
namespace {

bool Has(const std::string& text, const std::string& line) {
  return text.find(line + "\n") != std::string::npos;
}

TEST(MetricsRegistryTest, RendersCountersAndGaugesAsPrometheusText) {
  MetricsRegistry registry;
  registry.Counter("pp_test_frames_total", "Frames seen.", {{"direction", "in"}}).Inc(3);
  registry.Counter("pp_test_frames_total", "Frames seen.", {{"direction", "in"}}).Inc();
  registry.Counter("pp_test_frames_total", "Frames seen.", {{"direction", "out"}}).Inc(2);
  registry.Gauge("pp_test_sessions", "Sessions.").Set(5);

  const std::string text = registry.RenderPrometheus();
  EXPECT_TRUE(Has(text, "# HELP pp_test_frames_total Frames seen."));
  EXPECT_TRUE(Has(text, "# TYPE pp_test_frames_total counter"));
  EXPECT_TRUE(Has(text, "pp_test_frames_total{direction=\"in\"} 4")) << text;
  EXPECT_TRUE(Has(text, "pp_test_frames_total{direction=\"out\"} 2"));
  EXPECT_TRUE(Has(text, "# TYPE pp_test_sessions gauge"));
  EXPECT_TRUE(Has(text, "pp_test_sessions 5"));
}

TEST(MetricsRegistryTest, HistogramBucketsAreCumulative) {
  MetricsRegistry registry;
  auto& wait = registry.Histogram("pp_test_wait_seconds", "Wait.", {0.01, 0.1});
  wait.Observe(0.005);
  wait.Observe(0.05);
  wait.Observe(2.0);
  const std::string text = registry.RenderPrometheus();
  EXPECT_TRUE(Has(text, "pp_test_wait_seconds_bucket{le=\"0.01\"} 1")) << text;
  EXPECT_TRUE(Has(text, "pp_test_wait_seconds_bucket{le=\"0.1\"} 2")) << text;
  EXPECT_TRUE(Has(text, "pp_test_wait_seconds_bucket{le=\"+Inf\"} 3"));
  EXPECT_TRUE(Has(text, "pp_test_wait_seconds_count 3"));
}

TEST(MetricsRegistryTest, CollectorsRunAtScrapeUntilRemoved) {
  MetricsRegistry registry;
  int load = 7;
  {
    ScopedMetricsCollector collector(registry, [&load](MetricsRegistry& r) {
      r.Gauge("pp_test_load", "Load.").Set(load);
    });
    EXPECT_TRUE(Has(registry.RenderPrometheus(), "pp_test_load 7"));
    load = 9;
    EXPECT_TRUE(Has(registry.RenderPrometheus(), "pp_test_load 9"));
  }
  load = 11;
  EXPECT_TRUE(Has(registry.RenderPrometheus(), "pp_test_load 9")) << "removed: the gauge keeps its last value";
}

TEST(MetricsRegistryTest, LabelValuesAreEscaped) {
  MetricsRegistry registry;
  registry.Gauge("pp_test_info", "Info.", {{"version", "a\"b\\c"}}).Set(1);
  EXPECT_TRUE(Has(registry.RenderPrometheus(), "pp_test_info{version=\"a\\\"b\\\\c\"} 1"));
}

} // namespace
} // namespace pbr

TEST(MetricsRegistryTest, FixedLabelCountsFoldUnknownKeysIntoOther) {
  const std::vector<pbr::MetricLabelKey> known = {{"/a/1.0.0", "a"}, {"/b/1.0.0", "b"}};
  const std::unordered_map<std::string, size_t> by_key = {{"/a/1.0.0", 3}, {"/x/1.0.0", 2}, {"/y/1.0.0", 1}};
  const auto counts = pbr::FixedLabelCounts(by_key, known);
  ASSERT_EQ(counts.size(), 3u);
  EXPECT_EQ(counts[0], (std::pair<std::string, size_t>{"a", 3}));
  EXPECT_EQ(counts[1], (std::pair<std::string, size_t>{"b", 0}));  // present at zero: the series stays
  EXPECT_EQ(counts[2], (std::pair<std::string, size_t>{"other", 3}));

  const auto empty = pbr::FixedLabelCounts({}, known);
  ASSERT_EQ(empty.size(), 3u);
  EXPECT_EQ(empty[2].second, 0u);
}
