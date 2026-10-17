// The camera's JSON messages (manual chapter 5): parsers that tolerate the variations seen in
// the document (numbers as strings, booleans as 0/1, file_status / files_status), request
// bodies, the limits the manual gives for each setting, and names for the result codes. No ROS.
#pragma once

#include <optional>
#include <string>
#include <vector>

#include "hertzinno_acoustic_driver/types.hpp"

namespace hertzinno
{

// ---- parsers: nullopt and *err on a body that is not the expected JSON
std::optional<std::string> parseToken(const std::string & body, std::string * err);
std::optional<DetectionConfig> parseDetectionConfig(const std::string & body, std::string * err);
std::optional<VideoConfig> parseVideoConfig(const std::string & body, std::string * err);
std::optional<LeakageConfig> parseLeakageConfig(const std::string & body, std::string * err);
std::optional<DetectionArea> parseDetectionArea(const std::string & body, std::string * err);
std::optional<bool> parseDetectStatus(const std::string & body, std::string * err);
std::optional<DetectionResult> parseDetectionResult(const std::string & body, std::string * err);
std::optional<LogPage> parseLogPage(const std::string & body, std::string * err);

// Error text of a JSON error body ("msg", "message", "error", ...), empty if there is none.
std::string jsonErrorText(const std::string & body);
// True when a 2xx body still reports a failure ({"code": <not 0/200>, ...}).
bool jsonReportsFailure(const std::string & body, std::string * message);

// ---- request bodies and paths
std::string loginBody(const std::string & user, const std::string & password);
std::string intFieldBody(const std::string & field, int value);
std::string numberFieldBody(const std::string & field, double value);
std::string bandBody(int freq_down_limit, int freq_up_limit);
std::string resolutionBody(int width, int height);
std::string areaBody(const DetectionArea & area);
std::string deleteLogsBody(const std::vector<int> & ids);
std::string logsPath(const LogQuery & query);
// /detect_file/<saving_path>/<file_name>; empty and *err when a part is not a plain name.
std::string filePath(const std::string & saving_path, const std::string & file_name,
  std::string * err);

// ---- limits (manual 4.2.2, 4.3.2, 5.1.3 - 5.1.9); empty string when the value is valid
std::string checkThreshold(int db);
// Integers, down < up, 2 <= up - down <= 20 kHz, up at most half the sample rate when known.
std::string checkBand(int freq_down_limit, int freq_up_limit, int samplerate);
std::string checkDuration(int seconds);
std::string checkResolution(int width, int height);   // 2592x1944 ... 640x480
std::string checkBitRate(int kbps);                    // 4096, 2048, 1024, 512
std::string checkFps(int fps);                         // 1..30
std::string checkPort(int port);
std::string checkArea(const DetectionArea & area);
// Leak estimation settings, one field at a time; ints are checked to be whole numbers.
std::string checkLeakageField(const std::string & field, double value);
bool leakageFieldIsInt(const std::string & field);

// ---- names
std::string modeName(int mode);                        // partial_discharge, leakage, unknown
// Partial discharge bits: 1 noise, 2 corona, 4 surface, 8 floating; leakage: 1 no_leak, 2 leak.
// Several bits are joined with '+'.
std::string statusName(int mode, int status);
bool isAbnormal(int mode, int status);                 // a discharge / a leak
int allTypes(int mode);                                // 15 / 3

// "20230407_143129" (or any text whose first 14 digits are YYYYMMDDhhmmss) to seconds on a
// linear scale (the camera's clock, no time zone): differences are durations.
std::optional<long long> parseDeviceTime(const std::string & text);

}  // namespace hertzinno
