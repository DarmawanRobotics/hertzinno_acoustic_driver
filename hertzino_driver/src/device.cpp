#include "hertzino_driver/device.hpp"

#include <algorithm>
#include <cctype>

#include "hertzino_driver/api.hpp"

namespace hertzino
{

namespace
{

bool fail(std::string * err, const std::string & m)
{
  if (err) {
    *err = m;
  }
  return false;
}

bool printable(const std::string & s)
{
  return std::all_of(s.begin(), s.end(), [](unsigned char c) {
             return c == '\n' || c == '\r' || c == '\t' || (c >= 0x20 && c < 0x7f) || c >= 0x80;
           });
}

}  // namespace

AcousticCamera::AcousticCamera(Config config)
: config_(std::move(config))
{
  HttpClient::Options o;
  o.base_url = baseUrl();
  o.timeout = config_.timeout;
  o.connect_timeout = std::min(2.0, config_.timeout);
  http_ = std::make_unique<HttpClient>(o);
}

std::string AcousticCamera::baseUrl() const
{
  return "http://" + config_.host + ":" + std::to_string(config_.port);
}

std::string AcousticCamera::token() const
{
  std::lock_guard<std::mutex> lk(token_mtx_);
  return token_;
}

bool AcousticCamera::loggedIn() const
{
  return !token().empty();
}

std::vector<std::string> AcousticCamera::authHeaders() const
{
  const std::string t = token();
  if (t.empty()) {
    return {};
  }
  return {"Authorization: Bearer " + t};
}

std::string AcousticCamera::describe(const HttpResponse & r)
{
  if (!r.error.empty()) {
    return r.error;
  }
  std::string m = "HTTP " + std::to_string(r.status);
  std::string text = jsonErrorText(r.body);
  if (text.empty() && !r.body.empty() && r.body.size() <= 300 && printable(r.body)) {
    text = r.body;
    text.erase(std::remove(text.begin(), text.end(), '\r'), text.end());
    std::replace(text.begin(), text.end(), '\n', ' ');
  }
  if (!text.empty()) {
    m += ": " + text;
  }
  return m;
}

bool AcousticCamera::login(std::string * err)
{
  std::lock_guard<std::mutex> lk(login_mtx_);
  const HttpResponse r = http_->request(
    "POST", "/login", loginBody(config_.user, config_.password),
    {"Content-Type: application/json"});
  reachable_ = r.error.empty();
  if (!r.ok()) {
    std::string m = "login: " + describe(r);
    if (r.status == 401 || r.status == 403) {
      m += " (wrong user or password?)";
    }
    {
      std::lock_guard<std::mutex> tk(token_mtx_);
      token_.clear();
    }
    return fail(err, m);
  }
  std::string e;
  const auto t = parseToken(r.body, &e);
  if (!t) {
    return fail(err, e);
  }
  {
    std::lock_guard<std::mutex> tk(token_mtx_);
    token_ = *t;
  }
  ++logins_;
  return true;
}

HttpResponse AcousticCamera::call(
  const std::string & method, const std::string & path, const std::string & body, double timeout)
{
  const auto send = [&](const std::string & t) {
      std::vector<std::string> headers{"Authorization: Bearer " + t};
      if (!body.empty()) {
        headers.push_back("Content-Type: application/json");
      }
      HttpResponse r = http_->request(method, path, body, headers, timeout);
      reachable_ = r.error.empty();
      return r;
    };
  std::string t = token();
  std::string err;
  if (t.empty()) {
    if (!login(&err)) {
      HttpResponse r;
      r.error = err;
      return r;
    }
    t = token();
  }
  HttpResponse r = send(t);
  if (r.status == 401) {   // the token expired (or the camera restarted): log in once more
    const std::string current = token();
    if ((!current.empty() && current != t) || login(&err)) {
      r = send(token());
    }
  }
  return r;
}

bool AcousticCamera::expectOk(const HttpResponse & r, const std::string & what, std::string * err)
{
  if (!r.ok()) {
    return fail(err, what + ": " + describe(r));
  }
  std::string m;
  if (jsonReportsFailure(r.body, &m)) {
    return fail(err, what + ": " + m);
  }
  return true;
}

bool AcousticCamera::put(
  const std::string & path, const std::string & body, const std::string & what, std::string * err)
{
  return expectOk(call("PUT", path, body), what, err);
}

HttpResponse AcousticCamera::raw(
  const std::string & method, const std::string & path, const std::string & body, double timeout)
{
  return call(method, path, body, timeout);
}

// ---------------------------------------------------------------- /system

std::optional<DetectionConfig> AcousticCamera::detectionConfig(std::string * err)
{
  const HttpResponse r = call("GET", "/system");
  if (!expectOk(r, "GET /system", err)) {
    return std::nullopt;
  }
  auto c = parseDetectionConfig(r.body, err);
  if (c && c->samplerate > 0) {
    samplerate_ = c->samplerate;
  }
  return c;
}

bool AcousticCamera::setThreshold(int db, std::string * err)
{
  const std::string bad = checkThreshold(db);
  if (!bad.empty()) {
    return fail(err, bad);
  }
  return put("/system", intFieldBody("dB_limit", db), "PUT /system", err);
}

bool AcousticCamera::setBand(int down, int up, std::string * err)
{
  if (samplerate_ == 0) {
    detectionConfig(nullptr);   // for the upper limit; checked without it if this fails
  }
  const std::string bad = checkBand(down, up, samplerate_);
  if (!bad.empty()) {
    return fail(err, bad);
  }
  return put("/system", bandBody(down, up), "PUT /system", err);
}

bool AcousticCamera::setDuration(int seconds, std::string * err)
{
  const std::string bad = checkDuration(seconds);
  if (!bad.empty()) {
    return fail(err, bad);
  }
  return put("/system", intFieldBody("detect_times", seconds), "PUT /system", err);
}

// ---------------------------------------------------------------- /video_config

std::optional<VideoConfig> AcousticCamera::videoConfig(std::string * err)
{
  const HttpResponse r = call("GET", "/video_config");
  if (!expectOk(r, "GET /video_config", err)) {
    return std::nullopt;
  }
  return parseVideoConfig(r.body, err);
}

bool AcousticCamera::setVideo(const VideoConfig & c, std::string * err)
{
  std::vector<std::string> problems;
  if (c.width != 0 || c.height != 0) {
    problems.push_back(checkResolution(c.width, c.height));
  }
  if (c.fps != 0) {
    problems.push_back(checkFps(c.fps));
  }
  if (c.bit_rate != 0) {
    problems.push_back(checkBitRate(c.bit_rate));
  }
  if (c.rtsp_port != 0) {
    problems.push_back(checkPort(c.rtsp_port));
  }
  if (problems.empty()) {
    return fail(err, "nothing to change (all fields 0)");
  }
  for (const auto & p : problems) {
    if (!p.empty()) {
      return fail(err, p);
    }
  }
  // One setting per request, as the manual asks (the resolution as one pair).
  const std::string what = "PUT /video_config";
  if (c.rtsp_port != 0 && !put("/video_config", intFieldBody("video_port", c.rtsp_port), what, err)) {
    return false;
  }
  if (c.fps != 0 && !put("/video_config", intFieldBody("video_fps", c.fps), what, err)) {
    return false;
  }
  if (c.bit_rate != 0 &&
    !put("/video_config", intFieldBody("video_bit_rate", c.bit_rate), what, err))
  {
    return false;
  }
  if (c.width != 0 && !put("/video_config", resolutionBody(c.width, c.height), what, err)) {
    return false;
  }
  return true;
}

// ---------------------------------------------------------------- /leakage_estimation

std::optional<LeakageConfig> AcousticCamera::leakageConfig(std::string * err)
{
  const HttpResponse r = call("GET", "/leakage_estimation");
  if (!expectOk(r, "GET /leakage_estimation", err)) {
    return std::nullopt;
  }
  return parseLeakageConfig(r.body, err);
}

bool AcousticCamera::setLeakage(const std::string & field, double value, std::string * err)
{
  const std::string bad = checkLeakageField(field, value);
  if (!bad.empty()) {
    return fail(err, bad);
  }
  return put("/leakage_estimation", numberFieldBody(field, value), "PUT /leakage_estimation", err);
}

// ---------------------------------------------------------------- /signal_filtering_area

std::optional<DetectionArea> AcousticCamera::detectionArea(std::string * err)
{
  const HttpResponse r = call("GET", "/signal_filtering_area");
  if (!expectOk(r, "GET /signal_filtering_area", err)) {
    return std::nullopt;
  }
  return parseDetectionArea(r.body, err);
}

bool AcousticCamera::setDetectionArea(const DetectionArea & area, std::string * err)
{
  const std::string bad = checkArea(area);
  if (!bad.empty()) {
    return fail(err, bad);
  }
  DetectionArea a = area;
  if (!a.enabled) {
    a.x = a.y = a.width = a.height = -1;   // what the camera reports when no box is set
  }
  return put("/signal_filtering_area", areaBody(a), "PUT /signal_filtering_area", err);
}

// ---------------------------------------------------------------- detection

bool AcousticCamera::startDetection(std::string * err)
{
  return expectOk(call("POST", "/turn_on_detect"), "POST /turn_on_detect", err);
}

bool AcousticCamera::stopDetection(std::string * err)
{
  return expectOk(call("POST", "/turn_off_detect"), "POST /turn_off_detect", err);
}

std::optional<bool> AcousticCamera::detecting(std::string * err)
{
  const HttpResponse r = call("GET", "/detect_status");
  if (!expectOk(r, "GET /detect_status", err)) {
    return std::nullopt;
  }
  return parseDetectStatus(r.body, err);
}

std::optional<LogPage> AcousticCamera::logs(const LogQuery & query, std::string * err)
{
  const HttpResponse r = call("GET", logsPath(query));
  if (!expectOk(r, "GET /detect_logs", err)) {
    return std::nullopt;
  }
  return parseLogPage(r.body, err);
}

std::optional<std::string> AcousticCamera::file(
  const std::string & saving_path, const std::string & name, std::string * content_type,
  std::string * err)
{
  const std::string path = filePath(saving_path, name, err);
  if (path.empty()) {
    return std::nullopt;
  }
  const HttpResponse r = call("GET", path, "", config_.file_timeout);
  if (!expectOk(r, "GET " + path, err)) {
    return std::nullopt;
  }
  if (r.body.empty()) {
    fail(err, "GET " + path + ": empty file");
    return std::nullopt;
  }
  if (content_type) {
    *content_type = r.content_type;
  }
  return r.body;
}

bool AcousticCamera::deleteLogs(const std::vector<int> & ids, std::string * err)
{
  if (ids.empty()) {
    return fail(err, "no log ids");
  }
  return expectOk(call("DELETE", "/detect_logs", deleteLogsBody(ids)), "DELETE /detect_logs", err);
}

}  // namespace hertzino
