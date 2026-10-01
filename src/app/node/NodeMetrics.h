#pragma once

#include "app/node/NodeBootstrap.h"
#include "common/metrics/MetricsRegistry.h"

#include "common/PbrCompat.h"

namespace pbr {

/**
 * pp-node's scrape-time metrics (projects/node-monitoring M1; names in NODE_METRICS.md): process,
 * build / capabilities, media relay load and DHT ops, read from `boot` at each scrape. Reset the
 * returned collector before `boot`'s mesh stops.
 */
ScopedMetricsCollector RegisterNodeMetrics(NodeBootstrapResult& boot);

} // namespace pbr
