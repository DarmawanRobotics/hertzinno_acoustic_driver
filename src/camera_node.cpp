// hertzinno_camera_node: the camera's RTSP video (the visible image with the sound map drawn
// on it) as sensor_msgs/Image, compressed via image_transport. The manual names the port
// (video_port, 443) but not the path: with ~path "auto" the node asks the usual paths with
// DESCRIBE until one answers.
#include <cv_bridge/cv_bridge.h>
#include <image_transport/image_transport.h>
#include <opencv2/imgproc.hpp>
#include <ros/ros.h>

#include <chrono>
#include <cstdlib>
#include <memory>
#include <string>
#include <vector>

#include "hertzinno_acoustic_driver/rtsp_capture.hpp"
#include "hertzinno_acoustic_driver/rtsp_probe.hpp"

using namespace hertzinno;

int main(int argc, char ** argv)
{
  ros::init(argc, argv, "hertzinno_camera");
  ros::NodeHandle nh;
  ros::NodeHandle pnh("~");

  CaptureConfig c;
  std::string host, user, password, path, env_name;
  int rtsp_port = 443;
  double probe_period = 10.0;
  std::vector<std::string> probe_paths;
  pnh.param<std::string>("url", c.url, "");
  pnh.param<std::string>("host", host, "192.168.2.204");
  pnh.param("rtsp_port", rtsp_port, 443);
  pnh.param<std::string>("path", path, "auto");
  pnh.param("probe_paths", probe_paths, defaultRtspPaths());
  pnh.param("probe_period", probe_period, 10.0);
  pnh.param<std::string>("user", user, "admin");
  pnh.param<std::string>("password", password, "");
  pnh.param<std::string>("password_env", env_name, "HERTZINNO_PASSWORD");
  if (password.empty()) {   // roslaunch prints params: prefer the environment variable
    if (const char * env = std::getenv(env_name.c_str())) {
      password = env;
    }
  }
  if (password.empty()) {
    password = "123456";   // factory default
  }
  pnh.param<std::string>("codec", c.codec, "h264");   // the camera encodes H.264 only
  pnh.param<std::string>("decoder", c.decoder, "auto");
  pnh.param("latency_ms", c.latency_ms, 0);
  pnh.param<std::string>("protocols", c.protocols, "tcp");
  pnh.param("reconnect_sec", c.reconnect_sec, 1.0);
  pnh.param("timeout_ms", c.timeout_ms, 5000);
  std::string frame_id, encoding;
  double max_fps = 0.0;
  int width = 0, height = 0;
  pnh.param<std::string>("frame_id", frame_id, "hertzinno_acoustic_optical_frame");
  pnh.param<std::string>("encoding", encoding, "bgr8");
  pnh.param("max_fps", max_fps, 0.0);
  pnh.param("width", width, 0);
  pnh.param("height", height, 0);

  if (encoding != "bgr8" && encoding != "mono8") {
    ROS_FATAL("~encoding must be bgr8 or mono8, not '%s'", encoding.c_str());
    return 1;
  }

  std::string shown;
  if (c.url.empty()) {
    if (path == "auto") {
      std::string last;
      while (ros::ok()) {
        std::string report;
        const auto found = probeRtspPath(host, rtsp_port, probe_paths, user, password,
            std::min(3.0, c.timeout_ms / 1000.0), &report);
        if (found) {
          path = *found;
          ROS_INFO("RTSP path /%s (DESCRIBE: %s)", path.c_str(), report.c_str());
          break;
        }
        if (report != last) {
          ROS_WARN("no RTSP path found on %s:%d (%s); retrying every %.0f s, or set ~path",
            host.c_str(), rtsp_port, report.c_str(), probe_period);
          last = report;
        }
        const auto until = std::chrono::steady_clock::now() +
          std::chrono::duration<double>(probe_period);
        while (ros::ok() && std::chrono::steady_clock::now() < until) {
          ros::Duration(0.1).sleep();
        }
      }
      if (!ros::ok()) {
        return 0;
      }
    }
    c.url = rtspUrl(host, rtsp_port, path, user, password);
    shown = rtspUrl(host, rtsp_port, path, user, "");   // no password in logs
  } else {
    shown = c.url;
  }

  image_transport::ImageTransport it(nh);
  image_transport::Publisher pub = it.advertise("image_raw", 1);
  std::chrono::steady_clock::time_point last_pub{};

  std::unique_ptr<RtspCapture> capture;
  try {
    capture = std::make_unique<RtspCapture>(
      c,
      [&](const cv::Mat & frame) {
        const ros::Time stamp = ros::Time::now();
        const auto t = std::chrono::steady_clock::now();
        if (max_fps > 0.0 && t - last_pub < std::chrono::duration<double>(1.0 / max_fps)) {
          return;
        }
        if (pub.getNumSubscribers() == 0) {
          return;
        }
        last_pub = t;
        cv::Mat out = frame;
        if (width > 0 && height > 0 && (frame.cols != width || frame.rows != height)) {
          cv::resize(frame, out, cv::Size(width, height), 0, 0, cv::INTER_AREA);
        }
        if (encoding == "mono8") {
          cv::Mat gray;
          cv::cvtColor(out, gray, cv::COLOR_BGR2GRAY);
          out = gray;
        }
        std_msgs::Header header;
        header.stamp = stamp;
        header.frame_id = frame_id;
        pub.publish(cv_bridge::CvImage(header, encoding, out).toImageMsg());
      },
      [&](const std::string & m) {
        std::string msg = m;   // keep the password out of the log
        const size_t at = msg.find(c.url);
        if (at != std::string::npos) {
          msg.replace(at, c.url.size(), shown);
        }
        ROS_INFO("%s", msg.c_str());
      });
  } catch (const std::exception & e) {
    ROS_FATAL("%s", e.what());
    return 1;
  }

  ROS_INFO("streaming %s to %s", shown.c_str(), pub.getTopic().c_str());
  capture->start();
  ros::spin();
  capture->stop();
  return 0;
}
