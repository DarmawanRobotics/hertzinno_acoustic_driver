// Detections: the `detect` action (start, wait for the result of a measurement that began after
// the goal, optional photos), start/stop for continuous monitoring, every result of
// /detectFeedback on `detection`, and the camera's records (logs, files, deletion).
#pragma once

#include <actionlib/server/simple_action_server.h>
#include <ros/ros.h>
#include <sensor_msgs/CompressedImage.h>
#include <std_srvs/Trigger.h>

#include <chrono>
#include <condition_variable>
#include <deque>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>

#include "hertzino_msgs/DeleteLogs.h"
#include "hertzino_msgs/DetectAction.h"
#include "hertzino_msgs/GetFile.h"
#include "hertzino_msgs/GetLogs.h"
#include "hertzino_driver/device.hpp"
#include "hertzino_driver/ros_utils.hpp"
#include "hertzino_driver/websocket.hpp"

namespace hertzino_driver
{

class DetectionInterface
{
public:
  DetectionInterface(ros::NodeHandle & nh, ros::NodeHandle & pnh,
    std::shared_ptr<hertzino::AcousticCamera> camera, std::shared_ptr<DeviceState> state);
  ~DetectionInterface();

  DetectionInterface(const DetectionInterface &) = delete;
  DetectionInterface & operator=(const DetectionInterface &) = delete;

  bool feedbackConnected() const {return feedback_ && feedback_->connected();}

private:
  using Clock = std::chrono::steady_clock;
  using Server = actionlib::SimpleActionServer<DetectAction>;

  struct Arrival
  {
    hertzino::DetectionResult result;
    Clock::time_point at;
    ros::Time stamp;
  };

  void onFeedback(const hertzino::WebSocketClient::Message & m);
  void execute(const DetectGoalConstPtr & goal);
  // Highest log id of `mode` on the camera, 0 when there is none, nullopt if unreadable.
  std::optional<int> newestLogId(int mode);
  std::optional<sensor_msgs::CompressedImage> photo(
    const hertzino::DetectionResult & r, const std::string & name, const ros::Time & stamp,
    std::string * err);

  bool onStart(std_srvs::Trigger::Request & req, std_srvs::Trigger::Response & res);
  bool onStop(std_srvs::Trigger::Request & req, std_srvs::Trigger::Response & res);
  bool onGetLogs(GetLogs::Request & req, GetLogs::Response & res);
  bool onGetFile(GetFile::Request & req, GetFile::Response & res);
  bool onDeleteLogs(DeleteLogs::Request & req, DeleteLogs::Response & res);

  std::shared_ptr<hertzino::AcousticCamera> cam_;
  std::shared_ptr<DeviceState> state_;
  std::string frame_id_;
  double timeout_margin_{15.0};
  double photo_wait_{8.0};
  double result_grace_{3.0};

  ros::Publisher detection_pub_;
  std::vector<ros::ServiceServer> services_;

  std::mutex results_mtx_;
  std::condition_variable results_cv_;
  std::deque<Arrival> results_;

  std::unique_ptr<hertzino::WebSocketStream> feedback_;
  std::unique_ptr<Server> server_;
};

}  // namespace hertzino_driver
