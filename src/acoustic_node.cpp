// hertzinno_acoustic_node: one HTTP session with the camera (token login) shared by the
// status / settings, detection and stream interfaces. Callbacks run on an AsyncSpinner, so a
// long `detect` goal or a file download never blocks the other services.
#include <ros/ros.h>

#include <array>
#include <cstdlib>
#include <memory>
#include <string>

#include "hertzinno_acoustic_driver/detection_interface.hpp"
#include "hertzinno_acoustic_driver/device.hpp"
#include "hertzinno_acoustic_driver/status_interface.hpp"
#include "hertzinno_acoustic_driver/stream_interface.hpp"

using namespace hertzinno_acoustic_driver;

namespace
{

hertzinno::AcousticCamera::Config loadConfig(ros::NodeHandle & pnh)
{
  hertzinno::AcousticCamera::Config c;
  pnh.param<std::string>("host", c.host, c.host);
  pnh.param("port", c.port, 80);
  pnh.param<std::string>("user", c.user, "admin");
  pnh.param("timeout", c.timeout, 3.0);
  pnh.param("file_timeout", c.file_timeout, 30.0);
  // roslaunch prints parameters: the password preferably comes from the environment
  pnh.param<std::string>("password", c.password, "");
  std::string env_name;
  pnh.param<std::string>("password_env", env_name, "HERTZINNO_PASSWORD");
  if (c.password.empty()) {
    if (const char * env = std::getenv(env_name.c_str())) {
      c.password = env;
    }
  }
  if (c.password.empty()) {
    c.password = "123456";
    ROS_WARN("no password (~password or $%s): trying the factory default", env_name.c_str());
  }
  return c;
}

}  // namespace

int main(int argc, char ** argv)
{
  ros::init(argc, argv, "hertzinno_acoustic_node");
  ros::NodeHandle nh;
  ros::NodeHandle pnh("~");

  const auto config = loadConfig(pnh);
  auto camera = std::make_shared<hertzinno::AcousticCamera>(config);
  auto state = std::make_shared<DeviceState>();
  ROS_INFO("acoustic camera at %s, user %s", camera->baseUrl().c_str(), config.user.c_str());

  StreamInterface streams(nh, pnh, camera, state);
  DetectionInterface detection(nh, pnh, camera, state);
  StatusInterface status(nh, pnh, camera, state, [&streams, &detection] {
      return std::array<bool, 4>{
        streams.fft(), streams.prpd(), streams.voiceprint(), detection.feedbackConnected()};
    });

  ros::AsyncSpinner spinner(4);
  spinner.start();
  ros::waitForShutdown();
  return 0;
}
