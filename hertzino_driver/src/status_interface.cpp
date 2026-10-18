#include "hertzino_driver/status_interface.hpp"

#include <algorithm>
#include <cctype>
#include <chrono>

#include "hertzino_msgs/AcousticStatus.h"
#include "hertzino_driver/api.hpp"

namespace hertzino_driver
{

namespace
{

bool printable(const std::string & s)
{
  return std::all_of(s.begin(), s.end(), [](unsigned char c) {
             return c == '\n' || c == '\r' || c == '\t' || (c >= 0x20 && c != 0x7f);
           });
}

}  // namespace

StatusInterface::StatusInterface(
  ros::NodeHandle & nh, ros::NodeHandle & pnh, std::shared_ptr<hertzino::AcousticCamera> camera,
  std::shared_ptr<DeviceState> state, StreamFlags streams)
: cam_(std::move(camera)), state_(std::move(state)), streams_(std::move(streams))
{
  pnh.param("status_rate", status_rate_, 1.0);
  pnh.param("config_period", config_period_, 10.0);
  pnh.param<std::string>("frame_id", frame_id_, "hertzino_acoustic_optical_frame");
  pnh.param("db_limit", db_limit_, -1);
  pnh.param("freq_down_limit", freq_down_limit_, -1);
  pnh.param("freq_up_limit", freq_up_limit_, -1);
  pnh.param("detect_times", detect_times_, -1);
  status_rate_ = std::max(0.1, status_rate_);

  status_pub_ = nh.advertise<AcousticStatus>("status", 10);
  services_.push_back(nh.advertiseService("get_config", &StatusInterface::onGetConfig, this));
  services_.push_back(nh.advertiseService("set_threshold", &StatusInterface::onSetThreshold, this));
  services_.push_back(nh.advertiseService("set_duration", &StatusInterface::onSetDuration, this));
  services_.push_back(nh.advertiseService("set_band", &StatusInterface::onSetBand, this));
  services_.push_back(nh.advertiseService("set_video", &StatusInterface::onSetVideo, this));
  services_.push_back(nh.advertiseService("set_detection_area", &StatusInterface::onSetArea, this));
  services_.push_back(nh.advertiseService("set_leakage", &StatusInterface::onSetLeakage, this));
  services_.push_back(nh.advertiseService("http_request", &StatusInterface::onRawRequest, this));
  thread_ = std::thread(&StatusInterface::loop, this);
}

StatusInterface::~StatusInterface()
{
  running_ = false;
  wake_cv_.notify_all();
  if (thread_.joinable()) {
    thread_.join();
  }
}

void StatusInterface::wait(double sec)
{
  std::unique_lock<std::mutex> lk(wake_mtx_);
  wake_cv_.wait_for(lk, std::chrono::duration<double>(sec), [this] {return !running_.load();});
}

void StatusInterface::loop()
{
  using Clock = std::chrono::steady_clock;
  const double period = 1.0 / status_rate_;
  double backoff = 0.0;
  bool applied = false;
  bool answering = false;
  std::string last_login_error;
  auto next_config = Clock::now();
  while (running_ && ros::ok()) {
    const auto start = Clock::now();
    std::string err;
    if (!cam_->loggedIn()) {
      if (!cam_->login(&err)) {
        state_->setConnected(false, err);
        if (err != last_login_error) {   // once per kind of failure
          ROS_WARN("%s (retrying)", err.c_str());
          last_login_error = err;
        }
        answering = false;
        publish();
        backoff = std::min(30.0, backoff > 0.0 ? backoff * 2.0 : 2.0);
        wait(backoff);
        continue;
      }
      ROS_INFO("logged in to %s as %s", cam_->baseUrl().c_str(), cam_->config().user.c_str());
      last_login_error.clear();
      backoff = 0.0;
      next_config = start;
    }
    if (!applied) {
      applyStartupSettings();
      applied = true;
      next_config = start;
    }
    if (refresh_.exchange(false) || start >= next_config) {
      if (readConfig(&err)) {
        next_config = start + std::chrono::duration_cast<Clock::duration>(
          std::chrono::duration<double>(config_period_));
      }
    }
    const auto detecting = cam_->detecting(&err);
    if (detecting) {
      state_->setDetecting(*detecting);
    }
    const bool ok = detecting.has_value();
    state_->setConnected(ok, ok ? "" : err);
    if (ok != answering) {
      if (ok) {
        ROS_INFO("camera answering");
      } else {
        ROS_WARN("camera: %s", err.c_str());
      }
      answering = ok;
    }
    publish();
    const double left = period - std::chrono::duration<double>(Clock::now() - start).count();
    if (left > 0.0) {
      wait(left);
    }
  }
}

void StatusInterface::applyStartupSettings()
{
  std::string err;
  if (db_limit_ >= 0) {
    if (cam_->setThreshold(db_limit_, &err)) {
      ROS_INFO("imaging threshold set to %d dB", db_limit_);
    } else {
      ROS_WARN("~db_limit: %s", err.c_str());
    }
  }
  if (freq_down_limit_ >= 0 || freq_up_limit_ >= 0) {
    if (cam_->setBand(freq_down_limit_, freq_up_limit_, &err)) {
      ROS_INFO("analysis band set to %d..%d kHz", freq_down_limit_, freq_up_limit_);
    } else {
      ROS_WARN("~freq_down_limit / ~freq_up_limit: %s", err.c_str());
    }
  }
  if (detect_times_ > 0) {
    if (cam_->setDuration(detect_times_, &err)) {
      ROS_INFO("detection time set to %d s", detect_times_);
    } else {
      ROS_WARN("~detect_times: %s", err.c_str());
    }
  }
}

bool StatusInterface::readConfig(std::string * err)
{
  const auto c = cam_->detectionConfig(err);
  if (!c) {
    return false;
  }
  const auto before = state_->config();
  state_->setConfig(*c);
  if (!before || before->work_mode != c->work_mode) {
    ROS_INFO("%s mode, band %d..%d kHz, threshold %d dB, detection time %d s, %d Hz",
      hertzino::modeName(c->work_mode).c_str(), c->freq_down_limit, c->freq_up_limit,
      c->db_limit, c->detect_times, c->samplerate);
  }
  return true;
}

void StatusInterface::publish()
{
  AcousticStatus m;
  m.header.stamp = ros::Time::now();
  m.header.frame_id = frame_id_;
  m.connected = state_->connected();
  m.detecting = state_->detecting();
  if (const auto c = state_->config()) {
    m.config = toMsg(*c);
  } else {
    m.config.mode = "unknown";
  }
  if (streams_) {
    const auto s = streams_();
    m.fft_stream = s[0];
    m.prpd_stream = s[1];
    m.voiceprint_stream = s[2];
    m.feedback_stream = s[3];
  }
  m.error = state_->error();
  status_pub_.publish(m);
}

void StatusInterface::readBack(DetectionConfig & out, std::string & message)
{
  std::string err;
  if (const auto c = cam_->detectionConfig(&err)) {
    state_->setConfig(*c);
    out = toMsg(*c);
  } else {
    message += " (could not read it back: " + err + ")";
  }
}

// ---------------------------------------------------------------- services

bool StatusInterface::onGetConfig(GetConfig::Request &, GetConfig::Response & res)
{
  std::string err;
  std::vector<std::string> problems;
  const auto c = cam_->detectionConfig(&err);
  if (c) {
    state_->setConfig(*c);
    res.detection = toMsg(*c);
  } else {
    problems.push_back(err);
  }
  if (const auto v = cam_->videoConfig(&err)) {
    res.video = toMsg(*v);
  } else {
    problems.push_back(err);
  }
  if (const auto a = cam_->detectionArea(&err)) {
    res.area = toMsg(*a);
  } else {
    problems.push_back(err);
  }
  if (c && c->work_mode == hertzino::kLeakage) {
    if (const auto l = cam_->leakageConfig(&err)) {
      res.has_leakage = true;
      res.leakage = toMsg(*l);
    } else {
      problems.push_back(err);
    }
  }
  res.success = problems.empty();
  for (const auto & p : problems) {
    res.message += (res.message.empty() ? "" : "; ") + p;
  }
  return true;
}

bool StatusInterface::onSetThreshold(SetValue::Request & req, SetValue::Response & res)
{
  res.success = cam_->setThreshold(req.value, &res.message);
  if (res.success) {
    res.message = "imaging threshold " + std::to_string(req.value) + " dB";
    readBack(res.config, res.message);
  }
  return true;
}

bool StatusInterface::onSetDuration(SetValue::Request & req, SetValue::Response & res)
{
  res.success = cam_->setDuration(req.value, &res.message);
  if (res.success) {
    res.message = "detection time " + std::to_string(req.value) + " s";
    readBack(res.config, res.message);
  }
  return true;
}

bool StatusInterface::onSetBand(SetBand::Request & req, SetBand::Response & res)
{
  res.success = cam_->setBand(req.freq_down_limit, req.freq_up_limit, &res.message);
  if (res.success) {
    res.message = "analysis band " + std::to_string(req.freq_down_limit) + ".." +
      std::to_string(req.freq_up_limit) + " kHz";
    readBack(res.config, res.message);
  }
  return true;
}

bool StatusInterface::onSetVideo(SetVideo::Request & req, SetVideo::Response & res)
{
  hertzino::VideoConfig change;
  change.width = req.width;
  change.height = req.height;
  change.fps = req.fps;
  change.bit_rate = req.bit_rate;
  change.rtsp_port = req.rtsp_port;
  res.success = cam_->setVideo(change, &res.message);
  if (res.success) {
    res.message = "video settings changed";
    std::string err;
    if (const auto v = cam_->videoConfig(&err)) {
      res.config = toMsg(*v);
    } else {
      res.message += " (could not read them back: " + err + ")";
    }
  }
  return true;
}

bool StatusInterface::onSetArea(SetDetectionArea::Request & req, SetDetectionArea::Response & res)
{
  hertzino::DetectionArea a;
  a.enabled = req.enabled;
  a.x = req.x;
  a.y = req.y;
  a.width = req.width;
  a.height = req.height;
  res.success = cam_->setDetectionArea(a, &res.message);
  if (res.success) {
    res.message = req.enabled ? "detection area set" : "detection area removed";
    std::string err;
    if (const auto got = cam_->detectionArea(&err)) {
      res.area = toMsg(*got);
    } else {
      res.message += " (could not read it back: " + err + ")";
    }
  }
  return true;
}

bool StatusInterface::onSetLeakage(SetLeakage::Request & req, SetLeakage::Response & res)
{
  res.success = cam_->setLeakage(req.field, req.value, &res.message);
  if (res.success) {
    res.message = req.field + " changed";
    std::string err;
    if (const auto c = cam_->leakageConfig(&err)) {
      res.config = toMsg(*c);
    } else {
      res.message += " (could not read it back: " + err + ")";
    }
  }
  return true;
}

bool StatusInterface::onRawRequest(RawRequest::Request & req, RawRequest::Response & res)
{
  std::string method = req.method.empty() ? "GET" : req.method;
  std::transform(method.begin(), method.end(), method.begin(),
    [](unsigned char c) {return static_cast<char>(std::toupper(c));});
  if (method != "GET" && method != "PUT" && method != "POST" && method != "DELETE") {
    res.message = "method must be GET, PUT, POST or DELETE";
    return true;
  }
  if (req.path.empty() || req.path[0] != '/') {
    res.message = "path must start with '/'";
    return true;
  }
  const auto r = cam_->raw(method, req.path, req.body);
  res.status = static_cast<int32_t>(r.status);
  res.content_type = r.content_type;
  res.success = r.ok();
  if (printable(r.body)) {
    res.body = r.body;
  } else {
    res.body = "<" + std::to_string(r.body.size()) + " bytes of " +
      (r.content_type.empty() ? std::string("binary data") : r.content_type) + ">";
  }
  res.message = res.success ? "HTTP " + std::to_string(r.status) :
    hertzino::AcousticCamera::describe(r);
  return true;
}

}  // namespace hertzino_driver
