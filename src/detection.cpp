#include "hertzinno_acoustic_driver/detection.hpp"

#include <algorithm>
#include <cmath>

#include "hertzinno_acoustic_driver/api.hpp"

namespace hertzinno
{

double measurementWindow(const DetectionResult & r, int configured_seconds)
{
  if (configured_seconds > 0) {
    return configured_seconds;
  }
  const auto start = parseDeviceTime(r.start_time);
  const auto end = parseDeviceTime(r.end_time);
  if (start && end && *end >= *start) {
    return static_cast<double>(*end - *start);
  }
  return 0.0;
}

bool measuredAfter(double started, double arrived, double window)
{
  const double slack = std::min(1.0, 0.5 * std::max(0.0, window));
  return arrived - started >= window - slack;
}

std::vector<int16_t> decodeInt16(const std::string & payload)
{
  std::vector<int16_t> out(payload.size() / 2);
  for (size_t i = 0; i < out.size(); ++i) {
    const auto lo = static_cast<uint8_t>(payload[2 * i]);
    const auto hi = static_cast<uint8_t>(payload[2 * i + 1]);
    out[i] = static_cast<int16_t>(static_cast<uint16_t>(lo | (hi << 8)));
  }
  return out;
}

double binHz(int samplerate, size_t bins)
{
  if (samplerate <= 0 || bins == 0) {
    return 0.0;
  }
  return samplerate / (2.0 * static_cast<double>(bins));
}

BandPeak bandPeak(const std::vector<int16_t> & values, double bin_hz, double lo_hz, double hi_hz)
{
  BandPeak p;
  if (bin_hz <= 0.0 || values.empty() || hi_hz < lo_hz) {
    return p;
  }
  const long first = std::max(0L, static_cast<long>(std::ceil(lo_hz / bin_hz - 1e-9)));
  const long last = std::min(
    static_cast<long>(values.size()) - 1, static_cast<long>(std::floor(hi_hz / bin_hz + 1e-9)));
  for (long i = first; i <= last; ++i) {
    if (p.index < 0 || values[i] > p.value) {
      p.index = static_cast<int>(i);
      p.value = values[i];
    }
  }
  if (p.index >= 0) {
    p.hz = p.index * bin_hz;
  }
  return p;
}

}  // namespace hertzinno
