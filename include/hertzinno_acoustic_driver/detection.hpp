// Helpers for detections and the FFT stream. No ROS.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "hertzinno_acoustic_driver/types.hpp"

namespace hertzinno
{

// Length of the measurement behind a result, s: the configured detection time when known,
// else end_time - start_time from the result itself, else 0.
double measurementWindow(const DetectionResult & r, int configured_seconds);

// Whether a result that arrived `arrived - started` seconds after a detection was (re)started
// measured only after that start: a whole window must have passed, less one second of
// timestamp resolution (at most half the window). A result of a measurement that was already
// running when the robot stopped in front of the target arrives earlier and is refused.
bool measuredAfter(double started, double arrived, double window);

// /fftData payload: 16-bit little-endian integers (manual 5.2.1, 512 per frame).
std::vector<int16_t> decodeInt16(const std::string & payload);

// Hz per FFT bin: `bins` values cover 0 .. samplerate / 2. 0 when the rate is unknown.
double binHz(int samplerate, size_t bins);

struct BandPeak
{
  int index{-1};             // -1 when the band holds no bin
  int16_t value{0};
  double hz{0.0};
};

// Largest value in [lo_hz, hi_hz].
BandPeak bandPeak(const std::vector<int16_t> & values, double bin_hz, double lo_hz, double hi_hz);

}  // namespace hertzinno
