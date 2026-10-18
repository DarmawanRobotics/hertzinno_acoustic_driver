// Shared pieces of the ROS layer: client types to messages, the camera state the interfaces
// share, and WebSocket stream options from the node's parameters.
#pragma once

#include <ros/ros.h>

#include <mutex>
#include <optional>
#include <string>

#include "hertzino_msgs/DetectionArea.h"
#include "hertzino_msgs/DetectionConfig.h"
#include "hertzino_msgs/DetectionResult.h"
#include "hertzino_msgs/LeakageConfig.h"
#include "hertzino_msgs/VideoConfig.h"
#include "hertzino_driver/device.hpp"
#include "hertzino_driver/websocket.hpp"

namespace hertzino_driver
{

using namespace hertzino_msgs;   // messages, services, actions

DetectionConfig toMsg(const hertzino::DetectionConfig & c);
VideoConfig toMsg(const hertzino::VideoConfig & v);
DetectionArea toMsg(const hertzino::DetectionArea & a);
LeakageConfig toMsg(const hertzino::LeakageConfig & c);
DetectionResult toMsg(
  const hertzino::DetectionResult & r, const ros::Time & stamp, const std::string & frame_id);

// What the status thread last learned about the camera; read by the other interfaces.
class DeviceState
{
public:
  void setConnected(bool connected, const std::string & error)
  {
    std::lock_guard<std::mutex> lk(mtx_);
    connected_ = connected;
    error_ = error;
  }
  void setDetecting(bool detecting)
  {
    std::lock_guard<std::mutex> lk(mtx_);
    detecting_ = detecting;
  }
  void setConfig(const hertzino::DetectionConfig & config)
  {
    std::lock_guard<std::mutex> lk(mtx_);
    config_ = config;
  }
  bool connected() const
  {
    std::lock_guard<std::mutex> lk(mtx_);
    return connected_;
  }
  bool detecting() const
  {
    std::lock_guard<std::mutex> lk(mtx_);
    return detecting_;
  }
  std::string error() const
  {
    std::lock_guard<std::mutex> lk(mtx_);
    return error_;
  }
  std::optional<hertzino::DetectionConfig> config() const
  {
    std::lock_guard<std::mutex> lk(mtx_);
    return config_;
  }
  // 1 partial discharge, 2 leakage, 0 not known yet
  int workMode() const
  {
    std::lock_guard<std::mutex> lk(mtx_);
    return config_ ? config_->work_mode : 0;
  }

private:
  mutable std::mutex mtx_;
  bool connected_{false};
  bool detecting_{false};
  std::string error_;
  std::optional<hertzino::DetectionConfig> config_;
};

// ws://<host>:<ws_port or port><path> with ~heartbeat_sec; logs through rosconsole.
hertzino::WebSocketStream::Options streamOptions(
  ros::NodeHandle & pnh, const hertzino::AcousticCamera & camera, const std::string & path);
void logStream(const std::string & message);

}  // namespace hertzino_driver
