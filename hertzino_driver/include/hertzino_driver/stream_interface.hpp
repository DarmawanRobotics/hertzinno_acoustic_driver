// The camera's real-time WebSocket streams as topics: `spectrum` (fftData, 5 Hz), `prpd` and
// `voiceprint` (1 Hz, raw frames: their layout is not documented).
#pragma once

#include <ros/ros.h>

#include <memory>
#include <string>

#include "hertzino_driver/device.hpp"
#include "hertzino_driver/ros_utils.hpp"
#include "hertzino_driver/websocket.hpp"

namespace hertzino_driver
{

class StreamInterface
{
public:
  StreamInterface(ros::NodeHandle & nh, ros::NodeHandle & pnh,
    std::shared_ptr<hertzino::AcousticCamera> camera, std::shared_ptr<DeviceState> state);
  ~StreamInterface();

  StreamInterface(const StreamInterface &) = delete;
  StreamInterface & operator=(const StreamInterface &) = delete;

  bool fft() const {return fft_ && fft_->connected();}
  bool prpd() const {return prpd_ && prpd_->connected();}
  bool voiceprint() const {return voiceprint_ && voiceprint_->connected();}

private:
  std::unique_ptr<hertzino::WebSocketStream> open(
    ros::NodeHandle & pnh, const std::string & path,
    hertzino::WebSocketStream::MessageCallback on_message);
  void onFft(const hertzino::WebSocketClient::Message & m);
  void onRaw(const std::string & stream, ros::Publisher & pub,
    const hertzino::WebSocketClient::Message & m);

  std::shared_ptr<hertzino::AcousticCamera> cam_;
  std::shared_ptr<DeviceState> state_;
  std::string frame_id_;
  ros::Publisher spectrum_pub_;
  ros::Publisher prpd_pub_;
  ros::Publisher voiceprint_pub_;
  std::unique_ptr<hertzino::WebSocketStream> fft_;
  std::unique_ptr<hertzino::WebSocketStream> prpd_;
  std::unique_ptr<hertzino::WebSocketStream> voiceprint_;
};

}  // namespace hertzino_driver
