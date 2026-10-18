// Which result belongs to a detection, and the FFT helpers.
#include <gtest/gtest.h>

#include <string>
#include <vector>

#include "hertzino_driver/detection.hpp"

using namespace hertzino;

TEST(Detection, MeasurementWindow)
{
  DetectionResult r;
  r.start_time = "20230407_143129";
  r.end_time = "20230407_143154";
  EXPECT_DOUBLE_EQ(measurementWindow(r, 3), 3.0);    // the configured time wins
  EXPECT_DOUBLE_EQ(measurementWindow(r, 0), 25.0);   // else the result's own times
  r.end_time = "garbage";
  EXPECT_DOUBLE_EQ(measurementWindow(r, 0), 0.0);
}

TEST(Detection, MeasuredAfterTheStart)
{
  // 3 s detection restarted at t = 10: a result at 11 s measured mostly before the start
  EXPECT_FALSE(measuredAfter(10.0, 11.0, 3.0));
  EXPECT_FALSE(measuredAfter(10.0, 11.9, 3.0));
  EXPECT_TRUE(measuredAfter(10.0, 12.0, 3.0));       // one second of timestamp resolution
  EXPECT_TRUE(measuredAfter(10.0, 13.4, 3.0));
  // short windows: the slack is at most half of it
  EXPECT_FALSE(measuredAfter(0.0, 0.4, 1.0));
  EXPECT_TRUE(measuredAfter(0.0, 0.5, 1.0));
  // unknown window: anything after the start
  EXPECT_TRUE(measuredAfter(5.0, 5.0, 0.0));
}

TEST(Detection, DecodeInt16LittleEndian)
{
  const std::string payload("\x01\x00\xff\xff\x00\x80\x34\x12\x07", 9);
  const auto v = decodeInt16(payload);
  ASSERT_EQ(v.size(), 4u);   // the odd byte is dropped
  EXPECT_EQ(v[0], 1);
  EXPECT_EQ(v[1], -1);
  EXPECT_EQ(v[2], -32768);
  EXPECT_EQ(v[3], 0x1234);
}

TEST(Detection, BinHz)
{
  EXPECT_DOUBLE_EQ(binHz(96000, 512), 93.75);    // 512 bins over 0..48 kHz
  EXPECT_DOUBLE_EQ(binHz(130000, 512), 126.953125);
  EXPECT_DOUBLE_EQ(binHz(0, 512), 0.0);
  EXPECT_DOUBLE_EQ(binHz(96000, 0), 0.0);
}

TEST(Detection, BandPeak)
{
  std::vector<int16_t> v(512, 100);
  v[100] = 1500;    // 9.4 kHz
  v[320] = 3000;    // 30 kHz
  v[511] = 4000;    // 47.9 kHz
  const double hz = binHz(96000, v.size());
  auto p = bandPeak(v, hz, 20000.0, 40000.0);
  EXPECT_EQ(p.index, 320);
  EXPECT_EQ(p.value, 3000);
  EXPECT_DOUBLE_EQ(p.hz, 30000.0);
  p = bandPeak(v, hz, 0.0, 10000.0);
  EXPECT_EQ(p.index, 100);
  p = bandPeak(v, hz, 40000.0, 60000.0);   // clipped to the last bin
  EXPECT_EQ(p.index, 511);
  p = bandPeak(v, hz, 60000.0, 70000.0);   // above the spectrum
  EXPECT_EQ(p.index, -1);
  p = bandPeak(v, 0.0, 20000.0, 40000.0);  // unknown sample rate
  EXPECT_EQ(p.index, -1);
}
