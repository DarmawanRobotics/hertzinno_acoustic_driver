// The camera's real-time WebSocket streams as topics: `spectrum` (fftData, 5 Hz), `prpd` and
// `voiceprint` (1 Hz, raw frames: their layout is not documented).
#pragma once

#include <ros/ros.h>

#include <memory>
#include <string>

#include "hertzinno_acoustic_driver/device.hpp"
#include "hertzinno_acoustic_driver/ros_utils.hpp"
#include "hertzinno_acoustic_driver/websocket.hpp"

namespace hertzinno_acoustic_driver
{

class StreamInterface
{
public:
  StreamInterface(ros::NodeHandle & nh, ros::NodeHandle & pnh,
    std::shared_ptr<hertzinno::AcousticCamera> camera, std::shared_ptr<DeviceState> state);
  ~StreamInterface();

  StreamInterface(const StreamInterface &) = delete;
  StreamInterface & operator=(const StreamInterface &) = delete;

  bool fft() const {return fft_ && fft_->connected();}
  bool prpd() const {return prpd_ && prpd_->connected();}
  bool voiceprint() const {return voiceprint_ && voiceprint_->connected();}

private:
  std::unique_ptr<hertzinno::WebSocketStream> open(
    ros::NodeHandle & pnh, const std::string & path,
    hertzinno::WebSocketStream::MessageCallback on_message);
  void onFft(const hertzinno::WebSocketClient::Message & m);
  void onRaw(const std::string & stream, ros::Publisher & pub,
    const hertzinno::WebSocketClient::Message & m);

  std::shared_ptr<hertzinno::AcousticCamera> cam_;
  std::shared_ptr<DeviceState> state_;
  std::string frame_id_;
  ros::Publisher spectrum_pub_;
  ros::Publisher prpd_pub_;
  ros::Publisher voiceprint_pub_;
  std::unique_ptr<hertzinno::WebSocketStream> fft_;
  std::unique_ptr<hertzinno::WebSocketStream> prpd_;
  std::unique_ptr<hertzinno::WebSocketStream> voiceprint_;
};

}  // namespace hertzinno_acoustic_driver
