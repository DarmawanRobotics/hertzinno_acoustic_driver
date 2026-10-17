#include "hertzinno_acoustic_driver/api.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cctype>
#include <climits>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <sstream>

#include "hertzinno_acoustic_driver/crypto.hpp"

namespace hertzinno
{

using json = nlohmann::json;

namespace
{

std::string shorten(const std::string & s, size_t n = 200)
{
  std::string out = s.size() > n ? s.substr(0, n) + "..." : s;
  for (auto & c : out) {
    if (c == '\n' || c == '\r') {
      c = ' ';
    }
  }
  return out;
}

// A JSON object, or nullopt + *err. {"data": {...}} is unwrapped when the object itself has
// none of the expected keys (some firmware wraps answers that way).
std::optional<json> parseObject(
  const std::string & body, const std::vector<const char *> & keys, const char * what,
  std::string * err)
{
  json j = json::parse(body, nullptr, false);
  if (j.is_discarded() || !j.is_object()) {
    if (err) {
      *err = std::string(what) + ": not a JSON object: '" + shorten(body) + "'";
    }
    return std::nullopt;
  }
  const auto has_key = [&keys](const json & o) {
      return std::any_of(keys.begin(), keys.end(), [&o](const char * k) {return o.contains(k);});
    };
  if (!has_key(j)) {
    const auto it = j.find("data");
    if (it != j.end() && it->is_object() && has_key(*it)) {
      return *it;
    }
    if (err) {
      *err = std::string(what) + ": unexpected answer '" + shorten(body) + "'";
    }
    return std::nullopt;
  }
  return j;
}

std::optional<double> number(const json & v)
{
  if (v.is_number()) {
    return v.get<double>();
  }
  if (v.is_boolean()) {
    return v.get<bool>() ? 1.0 : 0.0;
  }
  if (v.is_string()) {
    const std::string s = v.get<std::string>();
    char * end = nullptr;
    const double d = std::strtod(s.c_str(), &end);
    if (end != s.c_str()) {
      while (*end && std::isspace(static_cast<unsigned char>(*end))) {
        ++end;
      }
      if (*end == '\0') {
        return d;
      }
    }
  }
  return std::nullopt;
}

const json * field(const json & j, std::initializer_list<const char *> keys)
{
  for (const char * k : keys) {
    const auto it = j.find(k);
    if (it != j.end() && !it->is_null()) {
      return &*it;
    }
  }
  return nullptr;
}

int toInt(double d)
{
  if (!std::isfinite(d)) {
    return 0;
  }
  return static_cast<int>(std::max<double>(INT_MIN, std::min<double>(INT_MAX, std::round(d))));
}

int getInt(const json & j, std::initializer_list<const char *> keys, int def)
{
  const json * v = field(j, keys);
  if (!v) {
    return def;
  }
  const auto n = number(*v);
  return n ? toInt(*n) : def;
}

double getDouble(const json & j, std::initializer_list<const char *> keys, double def)
{
  const json * v = field(j, keys);
  if (!v) {
    return def;
  }
  const auto n = number(*v);
  return n && std::isfinite(*n) ? *n : def;
}

std::string getString(const json & j, std::initializer_list<const char *> keys)
{
  const json * v = field(j, keys);
  if (!v) {
    return "";
  }
  if (v->is_string()) {
    return v->get<std::string>();
  }
  if (v->is_number_integer() || v->is_number_unsigned()) {
    return std::to_string(v->get<long long>());
  }
  if (v->is_number()) {
    std::ostringstream o;
    o << v->get<double>();
    return o.str();
  }
  return "";
}

DetectionResult resultFromJson(const json & j)
{
  DetectionResult r;
  r.log_id = getInt(j, {"id", "log_id"}, -1);
  r.detect_mode = getInt(j, {"detect_mode", "work_mode"}, 0);
  r.start_time = getString(j, {"start_time"});
  r.end_time = getString(j, {"end_time"});
  r.status = getInt(j, {"status"}, 0);
  r.intensity = getInt(j, {"soundmap_intensity", "intensity"}, 0);
  r.noise_prob = getDouble(j, {"noise_prob"}, 0.0);
  r.corona_prob = getDouble(j, {"corona_prob"}, 0.0);
  r.floating_prob = getDouble(j, {"floating_prob", "suspension_prob"}, 0.0);
  r.surface_prob = getDouble(j, {"surface_prob"}, 0.0);
  r.leakage_value = getDouble(j, {"leakage_value"}, 0.0);
  const json * unit = field(j, {"leakage_unit"});
  if (unit && unit->is_number()) {   // the settings use 0 / 1 for the same field
    r.leakage_unit = number(*unit).value_or(0.0) == 1.0 ? "ml/s" : "l/min";
  } else {
    r.leakage_unit = getString(j, {"leakage_unit", "leakage_unit_str"});
  }
  r.leakage_cost = getDouble(j, {"leakage_cost"}, 0.0);
  r.cost_unit = getString(j, {"cost_unit", "monetary_unit_str"});
  r.file_status = getInt(j, {"files_status", "file_status"}, -1);
  r.saving_path = getString(j, {"saving_path"});
  r.spectrum_photo = getString(j, {"power_spect_photo_name"});
  r.spectrum_video = getString(j, {"power_spect_video_name"});
  r.prpd_photo = getString(j, {"prpd_photo_name"});
  r.prpd_video = getString(j, {"prpd_video_name"});
  r.soundmap_photo = getString(j, {"soundmap_photo_name"});
  r.soundmap_video = getString(j, {"soundmap_video_name"});
  r.voiceprint_photo = getString(j, {"voiceprint_photo_name"});
  r.voiceprint_video = getString(j, {"voiceprint_video_name"});
  return r;
}

bool plainName(const std::string & s)
{
  if (s.empty() || s == "." || s == "..") {
    return false;
  }
  return std::all_of(s.begin(), s.end(), [](unsigned char c) {
             return std::isalnum(c) || c == '_' || c == '-' || c == '.';
           });
}

bool isWhole(double v)
{
  return std::isfinite(v) && std::floor(v) == v;
}

}  // namespace

// ---------------------------------------------------------------- parsers

std::optional<std::string> parseToken(const std::string & body, std::string * err)
{
  const auto j = parseObject(body, {"token", "access_token"}, "login", err);
  if (!j) {
    return std::nullopt;
  }
  const std::string token = getString(*j, {"token", "access_token"});
  if (token.empty()) {
    if (err) {
      *err = "login: empty token";
    }
    return std::nullopt;
  }
  return token;
}

std::optional<DetectionConfig> parseDetectionConfig(const std::string & body, std::string * err)
{
  const auto j = parseObject(
    body, {"work_mode", "dB_limit", "db_limit", "freq_down_limit", "freq_up_limit",
      "detect_times", "samplerate"}, "system", err);
  if (!j) {
    return std::nullopt;
  }
  DetectionConfig c;
  c.work_mode = getInt(*j, {"work_mode"}, 0);
  c.db_limit = getInt(*j, {"dB_limit", "db_limit"}, -1);
  c.freq_down_limit = getInt(*j, {"freq_down_limit"}, -1);
  c.freq_up_limit = getInt(*j, {"freq_up_limit"}, -1);
  c.detect_times = getInt(*j, {"detect_times"}, -1);
  c.samplerate = getInt(*j, {"samplerate", "sample_rate"}, 0);
  return c;
}

std::optional<VideoConfig> parseVideoConfig(const std::string & body, std::string * err)
{
  const auto j = parseObject(
    body, {"video_width", "video_height", "video_fps", "video_bit_rate", "video_port"},
    "video_config", err);
  if (!j) {
    return std::nullopt;
  }
  VideoConfig v;
  v.width = getInt(*j, {"video_width"}, 0);
  v.height = getInt(*j, {"video_height"}, 0);
  v.fps = getInt(*j, {"video_fps"}, 0);
  v.bit_rate = getInt(*j, {"video_bit_rate"}, 0);
  v.rtsp_port = getInt(*j, {"video_port"}, 0);
  return v;
}

std::optional<LeakageConfig> parseLeakageConfig(const std::string & body, std::string * err)
{
  const auto j = parseObject(
    body, {"leakage_distance", "leakage_unit", "monetary_unit", "value_of_gas", "energy_cost",
      "specific_power", "working_hours", "cost_display"}, "leakage_estimation", err);
  if (!j) {
    return std::nullopt;
  }
  LeakageConfig c;
  c.distance = getInt(*j, {"leakage_distance"}, 0);
  c.unit = getInt(*j, {"leakage_unit"}, 0);
  c.unit_name = getString(*j, {"leakage_unit_str"});
  c.monetary_unit = getInt(*j, {"monetary_unit"}, 0);
  c.monetary_unit_name = getString(*j, {"monetary_unit_str"});
  c.value_of_gas = getDouble(*j, {"value_of_gas"}, 0.0);
  c.energy_cost = getDouble(*j, {"energy_cost"}, 0.0);
  c.specific_power = getDouble(*j, {"specific_power"}, 0.0);
  c.working_hours = getInt(*j, {"working_hours"}, 0);
  c.cost_display = getInt(*j, {"cost_display"}, 0) != 0;
  return c;
}

std::optional<DetectionArea> parseDetectionArea(const std::string & body, std::string * err)
{
  const auto j = parseObject(
    body, {"detect_area_status", "left_top_x", "left_top_y", "area_width", "area_height"},
    "signal_filtering_area", err);
  if (!j) {
    return std::nullopt;
  }
  DetectionArea a;
  a.enabled = getInt(*j, {"detect_area_status"}, 0) != 0;
  a.x = getInt(*j, {"left_top_x"}, -1);
  a.y = getInt(*j, {"left_top_y"}, -1);
  a.width = getInt(*j, {"area_width"}, -1);
  a.height = getInt(*j, {"area_height"}, -1);
  return a;
}

std::optional<bool> parseDetectStatus(const std::string & body, std::string * err)
{
  const auto j = parseObject(body, {"detect_status"}, "detect_status", err);
  if (!j) {
    return std::nullopt;
  }
  const auto n = number(j->at("detect_status"));
  if (!n) {
    if (err) {
      *err = "detect_status: unexpected value '" + shorten(body) + "'";
    }
    return std::nullopt;
  }
  return *n != 0.0;
}

std::optional<DetectionResult> parseDetectionResult(const std::string & body, std::string * err)
{
  const auto j = parseObject(
    body, {"status", "detect_mode", "soundmap_intensity"}, "detection result", err);
  if (!j) {
    return std::nullopt;
  }
  return resultFromJson(*j);
}

std::optional<LogPage> parseLogPage(const std::string & body, std::string * err)
{
  const auto j = parseObject(body, {"logs", "logs_count"}, "detect_logs", err);
  if (!j) {
    return std::nullopt;
  }
  LogPage page;
  const auto it = j->find("logs");
  if (it != j->end() && it->is_array()) {
    for (const auto & entry : *it) {
      if (entry.is_object()) {
        page.logs.push_back(resultFromJson(entry));
      }
    }
  }
  page.total = getInt(*j, {"logs_count"}, static_cast<int>(page.logs.size()));
  return page;
}

std::string jsonErrorText(const std::string & body)
{
  const json j = json::parse(body, nullptr, false);
  if (j.is_discarded() || !j.is_object()) {
    return "";
  }
  for (const char * k : {"msg", "message", "error", "errmsg", "error_msg", "detail", "reason"}) {
    const auto it = j.find(k);
    if (it != j.end() && it->is_string() && !it->get<std::string>().empty()) {
      return it->get<std::string>();
    }
  }
  return "";
}

bool jsonReportsFailure(const std::string & body, std::string * message)
{
  const json j = json::parse(body, nullptr, false);
  if (j.is_discarded() || !j.is_object()) {
    return false;
  }
  bool failed = false;
  const auto code = j.find("code");
  if (code != j.end() && code->is_number()) {
    const double c = code->get<double>();
    failed = c != 0.0 && c != 200.0;
  }
  const auto success = j.find("success");
  if (success != j.end() && success->is_boolean() && !success->get<bool>()) {
    failed = true;
  }
  if (failed && message) {
    *message = jsonErrorText(body);
    if (message->empty()) {
      *message = shorten(body);
    }
  }
  return failed;
}

// ---------------------------------------------------------------- bodies and paths

std::string loginBody(const std::string & user, const std::string & password)
{
  return json{{"username", user}, {"password", loginPasswordHash(password)}}.dump();
}

std::string intFieldBody(const std::string & name, int value)
{
  return json{{name, value}}.dump();
}

std::string numberFieldBody(const std::string & name, double value)
{
  if (leakageFieldIsInt(name)) {
    return intFieldBody(name, toInt(value));
  }
  return json{{name, value}}.dump();
}

std::string bandBody(int freq_down_limit, int freq_up_limit)
{
  return json{{"freq_down_limit", freq_down_limit}, {"freq_up_limit", freq_up_limit}}.dump();
}

std::string resolutionBody(int width, int height)
{
  return json{{"video_height", height}, {"video_width", width}}.dump();
}

std::string areaBody(const DetectionArea & a)
{
  return json{
    {"area_height", a.height}, {"area_width", a.width},
    {"detect_area_status", a.enabled ? 1 : 0}, {"left_top_x", a.x}, {"left_top_y", a.y}}.dump();
}

std::string deleteLogsBody(const std::vector<int> & ids)
{
  json list = json::array();
  for (int id : ids) {
    list.push_back(std::to_string(id));   // the manual sends the ids as strings
  }
  return json{{"id", list}}.dump();
}

std::string logsPath(const LogQuery & q)
{
  std::ostringstream p;
  p << "/detect_logs?start_col=" << std::max(1, q.start) << "&col_count=" << std::max(1, q.count)
    << "&start_time=" << q.start_time << "&end_time=" << q.end_time
    << "&discharge_type=" << (q.types > 0 ? q.types : allTypes(q.detect_mode))
    << "&detect_mode=" << q.detect_mode;
  return p.str();
}

std::string filePath(const std::string & saving_path, const std::string & file_name,
  std::string * err)
{
  std::string folder = saving_path;
  while (!folder.empty() && folder.back() == '/') {
    folder.pop_back();
  }
  while (!folder.empty() && folder.front() == '/') {
    folder.erase(folder.begin());
  }
  if (!plainName(folder) || !plainName(file_name)) {
    if (err) {
      *err = "saving_path and file_name must be plain names (letters, digits, '_', '-', '.'), "
        "not '" + saving_path + "' / '" + file_name + "'";
    }
    return "";
  }
  return "/detect_file/" + folder + "/" + file_name;
}

// ---------------------------------------------------------------- limits

std::string checkThreshold(int db)
{
  if (db < 0 || db > 100) {
    return "the imaging threshold must be 0..100 dB, not " + std::to_string(db);
  }
  return "";
}

std::string checkBand(int down, int up, int samplerate)
{
  if (down < 0 || up <= down) {
    return "the band needs 0 <= freq_down_limit < freq_up_limit, not " + std::to_string(down) +
           ".." + std::to_string(up) + " kHz";
  }
  if (up - down < 2 || up - down > 20) {
    return "the band must be 2 to 20 kHz wide, not " + std::to_string(up - down) + " kHz";
  }
  if (samplerate > 0 && up > samplerate / 2000) {
    return "freq_up_limit " + std::to_string(up) + " kHz is above half the " +
           std::to_string(samplerate) + " Hz sample rate";
  }
  return "";
}

std::string checkDuration(int seconds)
{
  if (seconds < 1 || seconds > 3600) {
    return "the detection time must be 1..3600 s, not " + std::to_string(seconds);
  }
  return "";
}

std::string checkResolution(int width, int height)
{
  static const int sizes[][2] = {{2592, 1944}, {1920, 1080}, {1280, 720}, {720, 480}, {640, 480}};
  for (const auto & s : sizes) {
    if (s[0] == width && s[1] == height) {
      return "";
    }
  }
  return "the resolution must be 2592x1944, 1920x1080, 1280x720, 720x480 or 640x480, not " +
         std::to_string(width) + "x" + std::to_string(height);
}

std::string checkBitRate(int kbps)
{
  if (kbps == 4096 || kbps == 2048 || kbps == 1024 || kbps == 512) {
    return "";
  }
  return "the bit rate must be 4096, 2048, 1024 or 512 kbps, not " + std::to_string(kbps);
}

std::string checkFps(int fps)
{
  if (fps < 1 || fps > 30) {
    return "the frame rate must be 1..30, not " + std::to_string(fps);
  }
  return "";
}

std::string checkPort(int port)
{
  if (port < 1 || port > 65535) {
    return "invalid port " + std::to_string(port);
  }
  return "";
}

std::string checkArea(const DetectionArea & a)
{
  if (!a.enabled) {
    return "";
  }
  if (a.x < 0 || a.y < 0 || a.width <= 0 || a.height <= 0) {
    return "the area needs x, y >= 0 and width, height > 0";
  }
  return "";
}

bool leakageFieldIsInt(const std::string & f)
{
  return f == "leakage_distance" || f == "leakage_unit" || f == "monetary_unit" ||
         f == "cost_display" || f == "working_hours";
}

std::string checkLeakageField(const std::string & f, double v)
{
  struct Limit
  {
    const char * name;
    double lo;
    double hi;
  };
  static const Limit limits[] = {
    {"leakage_distance", 0, 3}, {"leakage_unit", 0, 1}, {"monetary_unit", 0, 4},
    {"cost_display", 0, 1}, {"working_hours", 0, 8784}, {"value_of_gas", 0, 1e9},
    {"energy_cost", 0, 1e9}, {"specific_power", 0, 1e9},
  };
  for (const auto & l : limits) {
    if (f == l.name) {
      if (!std::isfinite(v) || v < l.lo || v > l.hi || (leakageFieldIsInt(f) && !isWhole(v))) {
        std::ostringstream o;
        o << f << " must be " << (leakageFieldIsInt(f) ? "a whole number " : "") << "in "
          << l.lo << ".." << l.hi << ", not " << v;
        return o.str();
      }
      return "";
    }
  }
  return "unknown leakage setting '" + f + "' (leakage_distance, leakage_unit, monetary_unit, "
         "cost_display, value_of_gas, energy_cost, specific_power, working_hours)";
}

// ---------------------------------------------------------------- names

std::string modeName(int mode)
{
  switch (mode) {
    case kPartialDischarge: return "partial_discharge";
    case kLeakage: return "leakage";
    default: return "unknown";
  }
}

std::string statusName(int mode, int status)
{
  std::vector<std::pair<int, const char *>> bits;
  if (mode == kLeakage) {
    bits = {{1, "no_leak"}, {2, "leak"}};
  } else {
    bits = {{1, "noise"}, {2, "corona"}, {4, "surface"}, {8, "floating"}};
  }
  std::string out;
  int rest = status;
  for (const auto & b : bits) {
    if (status & b.first) {
      out += (out.empty() ? "" : "+") + std::string(b.second);
      rest &= ~b.first;
    }
  }
  if (rest != 0 || out.empty()) {
    out += (out.empty() ? "" : "+") + std::string("unknown(") + std::to_string(rest) + ")";
  }
  return out;
}

bool isAbnormal(int mode, int status)
{
  return mode == kLeakage ? (status & 2) != 0 : (status & (2 | 4 | 8)) != 0;
}

int allTypes(int mode)
{
  return mode == kLeakage ? 3 : 15;
}

std::optional<long long> parseDeviceTime(const std::string & text)
{
  std::string d;
  for (char c : text) {
    if (std::isdigit(static_cast<unsigned char>(c))) {
      d.push_back(c);
    }
  }
  if (d.size() < 14) {
    return std::nullopt;
  }
  const auto num = [&d](size_t pos, size_t len) {return std::stoll(d.substr(pos, len));};
  const long long y = num(0, 4);
  const unsigned m = static_cast<unsigned>(num(4, 2));
  const unsigned day = static_cast<unsigned>(num(6, 2));
  const long long hh = num(8, 2), mm = num(10, 2), ss = num(12, 2);
  if (m < 1 || m > 12 || day < 1 || day > 31 || hh > 23 || mm > 59 || ss > 60) {
    return std::nullopt;
  }
  // days from 1970-01-01 (H. Hinnant's days_from_civil)
  const long long yy = y - (m <= 2 ? 1 : 0);
  const long long era = (yy >= 0 ? yy : yy - 399) / 400;
  const long long yoe = yy - era * 400;
  const long long doy = (153 * (m > 2 ? m - 3 : m + 9) + 2) / 5 + day - 1;
  const long long doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
  const long long days = era * 146097 + doe - 719468;
  return days * 86400 + hh * 3600 + mm * 60 + ss;
}

}  // namespace hertzinno
