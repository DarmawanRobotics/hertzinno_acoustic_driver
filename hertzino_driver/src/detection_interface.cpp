#include "hertzino_driver/detection_interface.hpp"

#include <sys/stat.h>

#include <algorithm>
#include <cerrno>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <sstream>
#include <thread>

#include "hertzino_driver/api.hpp"
#include "hertzino_driver/detection.hpp"

namespace hertzino_driver
{

namespace
{

using Clock = std::chrono::steady_clock;

double toSec(Clock::duration d)
{
  return std::chrono::duration<double>(d).count();
}

std::string expandHome(const std::string & path)
{
  if (path == "~" || path.rfind("~/", 0) == 0) {
    if (const char * home = std::getenv("HOME")) {
      return std::string(home) + path.substr(1);
    }
  }
  return path;
}

bool makeDirs(const std::string & path, std::string * err)
{
  size_t pos = 0;
  while (pos != std::string::npos) {
    pos = path.find('/', pos + 1);
    const std::string part = path.substr(0, pos);
    if (part.empty() || part == "/") {
      continue;
    }
    if (::mkdir(part.c_str(), 0755) != 0 && errno != EEXIST) {
      *err = "cannot create " + part + ": " + std::strerror(errno);
      return false;
    }
  }
  return true;
}

std::string trimSlashes(std::string s)
{
  while (!s.empty() && s.back() == '/') {
    s.pop_back();
  }
  while (!s.empty() && s.front() == '/') {
    s.erase(s.begin());
  }
  return s;
}

std::string summary(const DetectionResult & r)
{
  std::ostringstream o;
  o << r.type << ", intensity " << r.intensity;
  if (r.detect_mode == DetectionResult::LEAKAGE && r.abnormal) {
    o << ", " << r.leakage_value << " " << r.leakage_unit;
  }
  return o.str();
}

std::string seconds(double s)
{
  char buf[32];
  std::snprintf(buf, sizeof(buf), "%.0f", s);
  return buf;
}

}  // namespace

DetectionInterface::DetectionInterface(
  ros::NodeHandle & nh, ros::NodeHandle & pnh, std::shared_ptr<hertzino::AcousticCamera> camera,
  std::shared_ptr<DeviceState> state)
: cam_(std::move(camera)), state_(std::move(state))
{
  pnh.param<std::string>("frame_id", frame_id_, "hertzino_acoustic_optical_frame");
  pnh.param("detect_timeout_margin", timeout_margin_, 15.0);
  pnh.param("photo_wait", photo_wait_, 8.0);
  pnh.param("result_grace", result_grace_, 5.0);
  bool feedback = true;
  pnh.param("streams/feedback", feedback, true);

  detection_pub_ = nh.advertise<DetectionResult>("detection", 10);
  services_.push_back(nh.advertiseService("start_detection", &DetectionInterface::onStart, this));
  services_.push_back(nh.advertiseService("stop_detection", &DetectionInterface::onStop, this));
  services_.push_back(nh.advertiseService("get_logs", &DetectionInterface::onGetLogs, this));
  services_.push_back(nh.advertiseService("get_file", &DetectionInterface::onGetFile, this));
  services_.push_back(nh.advertiseService("delete_logs", &DetectionInterface::onDeleteLogs, this));

  if (feedback) {
    auto cam = cam_;
    feedback_ = std::make_unique<hertzino::WebSocketStream>(
      streamOptions(pnh, *cam_, "/detectFeedback"), [cam] {return cam->authHeaders();},
      [this](const hertzino::WebSocketClient::Message & m) {onFeedback(m);}, logStream,
      [cam] {
        std::string err;
        cam->login(&err);
      });
    feedback_->start();
  }
  server_ = std::make_unique<Server>(
    nh, "detect", [this](const DetectGoalConstPtr & goal) {execute(goal);}, false);
  server_->start();
}

DetectionInterface::~DetectionInterface()
{
  feedback_.reset();   // no more callbacks
  server_.reset();     // waits for a running goal, which ends with ros::ok()
}

void DetectionInterface::onFeedback(const hertzino::WebSocketClient::Message & m)
{
  std::string err;
  const auto r = hertzino::parseDetectionResult(m.data, &err);
  if (!r) {
    ROS_DEBUG("detectFeedback: %s", err.c_str());   // e.g. an answer to the heartbeat
    return;
  }
  const ros::Time stamp = ros::Time::now();
  const auto msg = toMsg(*r, stamp, frame_id_);
  ROS_INFO("detection: %s", summary(msg).c_str());
  detection_pub_.publish(msg);
  {
    std::lock_guard<std::mutex> lk(results_mtx_);
    results_.push_back(Arrival{*r, Clock::now(), stamp});
    while (results_.size() > 32) {
      results_.pop_front();
    }
  }
  results_cv_.notify_all();
}

std::optional<int> DetectionInterface::newestLogId(int mode)
{
  hertzino::LogQuery q;
  q.detect_mode = mode;
  std::string err;
  const auto page = cam_->logs(q, &err);
  if (!page) {
    ROS_WARN("detect: cannot read the records (%s)", err.c_str());
    return std::nullopt;
  }
  int newest = 0;
  for (const auto & log : page->logs) {
    newest = std::max(newest, log.log_id);
  }
  return newest;
}

std::optional<sensor_msgs::CompressedImage> DetectionInterface::photo(
  const hertzino::DetectionResult & r, const std::string & name, const ros::Time & stamp,
  std::string * err)
{
  // the camera may still be writing the file: try again for photo_wait seconds
  const auto until = Clock::now() + std::chrono::duration_cast<Clock::duration>(
    std::chrono::duration<double>(photo_wait_));
  while (true) {
    std::string type;
    if (const auto data = cam_->file(r.saving_path, name, &type, err)) {
      sensor_msgs::CompressedImage img;
      img.header.stamp = stamp;
      img.header.frame_id = frame_id_;
      img.format = type.find("png") != std::string::npos ? "png" : "jpeg";
      img.data.assign(data->begin(), data->end());
      return img;
    }
    if (Clock::now() >= until || !ros::ok() || server_->isPreemptRequested() ||
      err->find("plain names") != std::string::npos)
    {
      return std::nullopt;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(500));
  }
}

void DetectionInterface::execute(const DetectGoalConstPtr & goal)
{
  DetectResult res;
  DetectFeedback fb;
  std::string err;
  const auto finish = [&](const std::string & state, const std::string & message) {
      res.state = state;
      res.message = message;
      res.success = state == "detected";
      if (res.success) {
        ROS_INFO("detect: %s", message.c_str());
        server_->setSucceeded(res, message);
      } else if (state == "preempted") {
        ROS_INFO("detect: cancelled");
        server_->setPreempted(res, message);
      } else {
        ROS_WARN("detect %s: %s", state.c_str(), message.c_str());
        server_->setAborted(res, message);
      }
    };
  const auto stop = [&] {
      std::string e;
      if (!goal->keep_running && !cam_->stopDetection(&e)) {
        ROS_WARN("detect: %s", e.c_str());
      }
    };

  // the detection time: the goal's, else the camera's
  const auto config = cam_->detectionConfig(&err);
  if (!config) {
    finish("error", err);
    return;
  }
  state_->setConfig(*config);
  int duration = config->detect_times;
  if (goal->duration > 0.0f) {
    const int want = std::max(1, static_cast<int>(std::ceil(goal->duration - 1e-3)));
    if (want != duration) {
      if (!cam_->setDuration(want, &err)) {
        finish("error", err);
        return;
      }
      duration = want;
    }
  }
  const int mode = config->work_mode > 0 ? config->work_mode : hertzino::kPartialDischarge;
  const double timeout = goal->timeout > 0.0f ? goal->timeout :
    std::max(duration, 1) + timeout_margin_;

  fb.state = "starting";
  server_->publishFeedback(fb);

  // A detection that is already running (continuous monitoring) is restarted, so that the
  // result describes what is in front of the camera now.
  const auto running = cam_->detecting(&err);
  if (running && *running) {
    if (!cam_->stopDetection(&err)) {
      finish("error", err);
      return;
    }
    const auto until = Clock::now() + std::chrono::seconds(2);
    while (Clock::now() < until && cam_->detecting(nullptr) == std::optional<bool>(true)) {
      std::this_thread::sleep_for(std::chrono::milliseconds(200));
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(500));   // its record, if any
  }
  // records already on the camera: a newer one is a result even without live feedback
  const std::optional<int> newest_before = newestLogId(mode);
  const auto t0 = Clock::now();
  if (!cam_->startDetection(&err)) {
    finish("error", err);
    return;
  }
  ROS_INFO("detect: started, %d s", duration);

  std::optional<Arrival> found;
  std::map<int, double> first_seen;   // record id -> seconds after the start it appeared
  bool detecting = true;
  std::optional<Clock::time_point> ended;
  auto next_poll = t0 + std::chrono::seconds(1);
  auto next_feedback = t0;
  while (!found) {
    if (server_->isPreemptRequested() || !ros::ok()) {
      stop();
      finish("preempted", "cancelled");
      return;
    }
    // live results (/detectFeedback)
    std::deque<Arrival> arrived;
    {
      std::unique_lock<std::mutex> lk(results_mtx_);
      results_cv_.wait_for(lk, std::chrono::milliseconds(100), [this] {return !results_.empty();});
      arrived.swap(results_);
    }
    for (const auto & a : arrived) {
      if (a.at < t0) {
        continue;
      }
      const double after = toSec(a.at - t0);
      if (hertzino::measuredAfter(0.0, after, hertzino::measurementWindow(a.result, duration))) {
        found = a;
        break;
      }
      ROS_INFO("detect: skipped a result %.1f s after the start (measured before it)", after);
    }
    if (found) {
      break;
    }

    const auto now = Clock::now();
    const double elapsed = toSec(now - t0);
    if (now >= next_poll) {
      next_poll = now + std::chrono::seconds(1);
      if (const auto d = cam_->detecting(nullptr)) {
        detecting = *d;
        // stopped by itself once the detection time is over
        if (!*d && !ended && elapsed >= duration - 1.0) {
          ended = now;
        }
      }
      // Without live feedback, or once the detection is over, new records tell the result:
      // a record that appeared a whole detection time after the start, or after the end the
      // newest one.
      if (newest_before && (!feedbackConnected() || ended)) {
        hertzino::LogQuery q;
        q.detect_mode = mode;
        if (const auto page = cam_->logs(q, nullptr)) {
          for (const auto & log : page->logs) {
            if (log.log_id <= *newest_before) {
              continue;
            }
            const double seen = first_seen.emplace(log.log_id, elapsed).first->second;
            const bool ours = ended ||
              hertzino::measuredAfter(0.0, seen, hertzino::measurementWindow(log, duration));
            if (ours && (!found || log.log_id > found->result.log_id)) {
              found = Arrival{log, now, ros::Time::now()};
            }
          }
        }
      }
      if (found) {
        break;
      }
      if (ended && toSec(now - *ended) > result_grace_) {
        stop();
        finish("no_result", std::string("the detection ended without a result") +
          (feedbackConnected() ? "" : " (no connection to /detectFeedback)"));
        return;
      }
    }
    if (elapsed > timeout) {
      stop();
      finish("timeout", "no result within " + seconds(timeout) + " s");
      return;
    }
    if (now >= next_feedback) {
      fb.elapsed = static_cast<float>(elapsed);
      fb.detecting = detecting;
      fb.state = "detecting";
      server_->publishFeedback(fb);
      next_feedback = now + std::chrono::milliseconds(200);
    }
  }

  stop();
  hertzino::DetectionResult r = found->result;
  if (r.log_id < 0 && !r.saving_path.empty()) {   // live feedback has no record id
    hertzino::LogQuery q;
    q.detect_mode = mode;
    if (const auto page = cam_->logs(q, nullptr)) {
      for (const auto & log : page->logs) {
        if (log.saving_path == r.saving_path) {
          r.log_id = log.log_id;
          r.file_status = log.file_status;
          break;
        }
      }
    }
  }
  res.result = toMsg(r, found->stamp, frame_id_);
  std::string notes;
  if (goal->fetch_photos) {
    if (r.soundmap_photo.empty()) {
      notes += "; the result names no sound map photo";
    } else if (const auto p = photo(r, r.soundmap_photo, found->stamp, &err)) {
      res.soundmap_photo = *p;
    } else {
      notes += "; sound map photo: " + err;
    }
    if (!r.prpd_photo.empty()) {
      if (const auto p = photo(r, r.prpd_photo, found->stamp, &err)) {
        res.prpd_photo = *p;
      } else {
        notes += "; PRPD photo: " + err;
      }
    }
  }
  finish("detected", summary(res.result) + notes);
}

// ---------------------------------------------------------------- services

bool DetectionInterface::onStart(std_srvs::Trigger::Request &, std_srvs::Trigger::Response & res)
{
  res.success = cam_->startDetection(&res.message);
  if (res.success) {
    res.message = "detection started, results on 'detection'";
  }
  return true;
}

bool DetectionInterface::onStop(std_srvs::Trigger::Request &, std_srvs::Trigger::Response & res)
{
  res.success = cam_->stopDetection(&res.message);
  if (res.success) {
    res.message = "detection stopped";
  }
  return true;
}

bool DetectionInterface::onGetLogs(GetLogs::Request & req, GetLogs::Response & res)
{
  hertzino::LogQuery q;
  q.start = req.start > 0 ? static_cast<int>(req.start) : 1;
  q.count = req.count > 0 ? static_cast<int>(req.count) : 100;
  q.start_time = req.start_time;
  q.end_time = req.end_time > 0 ? req.end_time : 2000000000LL;
  q.types = req.types;
  q.detect_mode = req.detect_mode > 0 ? req.detect_mode :
    (state_->workMode() > 0 ? state_->workMode() : hertzino::kPartialDischarge);
  const auto page = cam_->logs(q, &res.message);
  if (!page) {
    return true;
  }
  const ros::Time stamp = ros::Time::now();
  for (const auto & log : page->logs) {
    res.logs.push_back(toMsg(log, stamp, frame_id_));
  }
  res.total = page->total;
  res.success = true;
  res.message = std::to_string(res.logs.size()) + " of " + std::to_string(page->total) +
    " records";
  return true;
}

bool DetectionInterface::onGetFile(GetFile::Request & req, GetFile::Response & res)
{
  std::string type;
  const auto data = cam_->file(req.saving_path, req.file_name, &type, &res.message);
  if (!data) {
    return true;
  }
  res.content_type = type;
  res.size = static_cast<uint32_t>(data->size());
  if (req.save_to.empty()) {
    res.data.assign(data->begin(), data->end());
    res.success = true;
    res.message = std::to_string(data->size()) + " bytes";
    return true;
  }
  // names already checked by the client: plain names, no '/' or ".."
  const std::string dir = expandHome(req.save_to) + "/" + trimSlashes(req.saving_path);
  if (!makeDirs(dir, &res.message)) {
    return true;
  }
  const std::string path = dir + "/" + req.file_name;
  std::ofstream out(path, std::ios::binary | std::ios::trunc);
  out.write(data->data(), static_cast<std::streamsize>(data->size()));
  out.close();
  if (!out) {
    res.message = "cannot write " + path;
    return true;
  }
  res.path = path;
  res.success = true;
  res.message = "saved " + std::to_string(data->size()) + " bytes to " + path;
  return true;
}

bool DetectionInterface::onDeleteLogs(DeleteLogs::Request & req, DeleteLogs::Response & res)
{
  res.success = cam_->deleteLogs(std::vector<int>(req.ids.begin(), req.ids.end()), &res.message);
  if (res.success) {
    res.message = "deleted " + std::to_string(req.ids.size()) + " records";
  }
  return true;
}

}  // namespace hertzino_driver
