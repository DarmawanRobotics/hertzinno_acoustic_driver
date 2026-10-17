// Shared pieces of the ROS layer: client types to messages, the camera state the interfaces
// share, and WebSocket stream options from the node's parameters.
#pragma once

#include <ros/ros.h>

#include <mutex>
#include <optional>
#include <string>

#include "hertzinno_acoustic_driver/DetectionArea.h"
#include "hertzinno_acoustic_driver/DetectionConfig.h"
#include "hertzinno_acoustic_driver/DetectionResult.h"
#include "hertzinno_acoustic_driver/LeakageConfig.h"
#include "hertzinno_acoustic_driver/VideoConfig.h"
#include "hertzinno_acoustic_driver/device.hpp"
#include "hertzinno_acoustic_driver/websocket.hpp"

namespace hertzinno_acoustic_driver
{

DetectionConfig toMsg(const hertzinno::DetectionConfig & c);
VideoConfig toMsg(const hertzinno::VideoConfig & v);
DetectionArea toMsg(const hertzinno::DetectionArea & a);
LeakageConfig toMsg(const hertzinno::LeakageConfig & c);
DetectionResult toMsg(
  const hertzinno::DetectionResult & r, const ros::Time & stamp, const std::string & frame_id);

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
  void setConfig(const hertzinno::DetectionConfig & config)
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
  std::optional<hertzinno::DetectionConfig> config() const
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
  std::optional<hertzinno::DetectionConfig> config_;
};

// ws://<host>:<ws_port or port><path> with ~heartbeat_sec; logs through rosconsole.
hertzinno::WebSocketStream::Options streamOptions(
  ros::NodeHandle & pnh, const hertzinno::AcousticCamera & camera, const std::string & path);
void logStream(const std::string & message);

}  // namespace hertzinno_acoustic_driver
