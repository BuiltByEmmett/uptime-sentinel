// GET /metrics: the monitor's figures in the Prometheus text format, for
// scraping into Prometheus, VictoriaMetrics, Grafana Agent and the like.
// No password. It carries only connection health and device health: never
// the public IP address, the WiFi name, local addresses or anything about
// other devices on the network.
#pragma once
#include <Arduino.h>

namespace metrics {

String render();

}  // namespace metrics
