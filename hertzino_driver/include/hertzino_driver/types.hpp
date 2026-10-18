// Plain data types of the Hertzinno online acoustic camera API (HZ-FA-371, firmware V5.1),
// named after the fields of the HTTP / WebSocket messages. No ROS.
#pragma once

#include <string>
#include <vector>

namespace hertzino
{

// work_mode / detect_mode
constexpr int kPartialDischarge = 1;
constexpr int kLeakage = 2;

// GET /system
struct DetectionConfig
{
  int work_mode{0};          // 1 partial discharge, 2 gas leakage
  int db_limit{-1};          // imaging threshold, 0..100 dB
  int freq_down_limit{-1};   // analysis band, kHz
  int freq_up_limit{-1};
  int detect_times{-1};      // detection time, s
  int samplerate{0};         // microphone array, Hz
};

// GET /video_config (RTSP stream, H.264 only)
struct VideoConfig
{
  int width{0};
  int height{0};
  int fps{0};
  int bit_rate{0};           // kbps
  int rtsp_port{0};          // video_port
};

// GET /signal_filtering_area: only sources inside the box are reported when enabled
struct DetectionArea
{
  bool enabled{false};       // detect_area_status
  int x{-1};                 // left_top_x
  int y{-1};                 // left_top_y
  int width{-1};             // area_width
  int height{-1};            // area_height
};

// GET /leakage_estimation (leakage mode)
struct LeakageConfig
{
  int distance{0};           // leakage_distance: 0 = 0.5 m, 1 = 1 m, 2 = 2 m, 3 = 5 m
  int unit{0};               // leakage_unit: 0 l/min, 1 ml/s
  std::string unit_name;     // leakage_unit_str
  int monetary_unit{0};      // 0 CNY, 1 GBP, 2 USD, 3 EUR, 4 RUB
  std::string monetary_unit_name;
  double value_of_gas{0.0};  // currency per m3
  double energy_cost{0.0};   // currency per kWh
  double specific_power{0.0};  // kW/(m3/min)
  int working_hours{0};      // h per year
  bool cost_display{false};
};

// One detection: a /detectFeedback message or a /detect_logs entry
struct DetectionResult
{
  int log_id{-1};            // id, only in logs
  int detect_mode{0};
  std::string start_time;    // camera clock, "20230407_143129"
  std::string end_time;
  int status{0};             // bits, see statusName()
  int intensity{0};          // soundmap_intensity
  double noise_prob{0.0};
  double corona_prob{0.0};
  double floating_prob{0.0};
  double surface_prob{0.0};
  double leakage_value{0.0};
  std::string leakage_unit;
  double leakage_cost{0.0};
  std::string cost_unit;
  int file_status{-1};       // 1 files kept, 0 deleted, -1 unknown
  std::string saving_path;   // folder of the files, "2023_04_07_14_31_29_133/"
  std::string spectrum_photo;    // power_spect_photo_name
  std::string spectrum_video;    // power_spect_video_name
  std::string prpd_photo;
  std::string prpd_video;
  std::string soundmap_photo;
  std::string soundmap_video;
  std::string voiceprint_photo;
  std::string voiceprint_video;
};

// GET /detect_logs query (all optional on the camera; these are its defaults)
struct LogQuery
{
  int start{1};              // start_col, 1-based
  int count{100};            // col_count
  long long start_time{0};   // unix time, camera clock
  long long end_time{2000000000};
  int types{0};              // discharge_type bits, 0: every type of the mode
  int detect_mode{kPartialDischarge};
};

struct LogPage
{
  int total{0};              // logs_count
  std::vector<DetectionResult> logs;
};

}  // namespace hertzino
