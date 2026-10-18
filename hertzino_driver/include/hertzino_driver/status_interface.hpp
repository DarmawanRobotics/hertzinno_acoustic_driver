// Connection, status and settings: logs in (and again after the camera restarts), applies the
// settings given as parameters, polls the detection state at status_rate, publishes `status`
// and serves the configuration services and raw HTTP requests.
#pragma once

#include <ros/ros.h>

#include <array>
#include <atomic>
#include <condition_variable>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

#include "hertzino_msgs/GetConfig.h"
#include "hertzino_msgs/RawRequest.h"
#include "hertzino_msgs/SetBand.h"
#include "hertzino_msgs/SetDetectionArea.h"
#include "hertzino_msgs/SetLeakage.h"
#include "hertzino_msgs/SetValue.h"
#include "hertzino_msgs/SetVideo.h"
#include "hertzino_driver/device.hpp"
#include "hertzino_driver/ros_utils.hpp"

namespace hertzino_driver
{

class StatusInterface
{
public:
  // fft, prpd, voiceprint, feedback stream connected
  using StreamFlags = std::function<std::array<bool, 4>()>;

  StatusInterface(ros::NodeHandle & nh, ros::NodeHandle & pnh,
    std::shared_ptr<hertzino::AcousticCamera> camera, std::shared_ptr<DeviceState> state,
    StreamFlags streams);
  ~StatusInterface();

  StatusInterface(const StatusInterface &) = delete;
  StatusInterface & operator=(const StatusInterface &) = delete;

private:
  void loop();
  void applyStartupSettings();
  bool readConfig(std::string * err);
  void publish();
  void wait(double sec);
  // after a successful change: the configuration read back into the state and `out`
  void readBack(DetectionConfig & out, std::string & message);

  bool onGetConfig(GetConfig::Request & req, GetConfig::Response & res);
  bool onSetThreshold(SetValue::Request & req, SetValue::Response & res);
  bool onSetDuration(SetValue::Request & req, SetValue::Response & res);
  bool onSetBand(SetBand::Request & req, SetBand::Response & res);
  bool onSetVideo(SetVideo::Request & req, SetVideo::Response & res);
  bool onSetArea(SetDetectionArea::Request & req, SetDetectionArea::Response & res);
  bool onSetLeakage(SetLeakage::Request & req, SetLeakage::Response & res);
  bool onRawRequest(RawRequest::Request & req, RawRequest::Response & res);

  std::shared_ptr<hertzino::AcousticCamera> cam_;
  std::shared_ptr<DeviceState> state_;
  StreamFlags streams_;

  double status_rate_{1.0};
  double config_period_{10.0};
  std::string frame_id_;
  int db_limit_{-1};
  int freq_down_limit_{-1};
  int freq_up_limit_{-1};
  int detect_times_{-1};

  ros::Publisher status_pub_;
  std::vector<ros::ServiceServer> services_;

  std::atomic<bool> running_{true};
  std::atomic<bool> refresh_{false};
  std::mutex wake_mtx_;
  std::condition_variable wake_cv_;
  std::thread thread_;
};

}  // namespace hertzino_driver
