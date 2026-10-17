#include "hertzinno_acoustic_driver/stream_interface.hpp"

#include "hertzinno_acoustic_driver/RawFrame.h"
#include "hertzinno_acoustic_driver/Spectrum.h"
#include "hertzinno_acoustic_driver/detection.hpp"

namespace hertzinno_acoustic_driver
{

namespace
{

// A printable text frame: an answer to the heartbeat, not stream data. Binary data sent
// in a text frame (non-printable bytes) is still taken.
bool isTextReply(const hertzinno::WebSocketClient::Message & m)
{
  if (m.binary) {
    return false;
  }
  for (unsigned char c : m.data) {
    if (c < 0x20 && c != '\n' && c != '\r' && c != '\t') {
      return false;
    }
  }
  return true;
}

}  // namespace

StreamInterface::StreamInterface(
  ros::NodeHandle & nh, ros::NodeHandle & pnh, std::shared_ptr<hertzinno::AcousticCamera> camera,
  std::shared_ptr<DeviceState> state)
: cam_(std::move(camera)), state_(std::move(state))
{
  pnh.param<std::string>("frame_id", frame_id_, "hertzinno_acoustic_optical_frame");
  bool fft = true, prpd = true, voiceprint = true;
  pnh.param("streams/fft", fft, true);
  pnh.param("streams/prpd", prpd, true);
  pnh.param("streams/voiceprint", voiceprint, true);

  if (fft) {
    spectrum_pub_ = nh.advertise<Spectrum>("spectrum", 10);
    fft_ = open(pnh, "/fftData", [this](const hertzinno::WebSocketClient::Message & m) {onFft(m);});
  }
  if (prpd) {
    prpd_pub_ = nh.advertise<RawFrame>("prpd", 10);
    prpd_ = open(pnh, "/prpdData", [this](const hertzinno::WebSocketClient::Message & m) {
          onRaw("prpdData", prpd_pub_, m);
        });
  }
  if (voiceprint) {
    voiceprint_pub_ = nh.advertise<RawFrame>("voiceprint", 10);
    voiceprint_ = open(pnh, "/voiceprintData", [this](const hertzinno::WebSocketClient::Message & m) {
          onRaw("voiceprintData", voiceprint_pub_, m);
        });
  }
}

StreamInterface::~StreamInterface()
{
  // stop the threads before the publishers go
  fft_.reset();
  prpd_.reset();
  voiceprint_.reset();
}

std::unique_ptr<hertzinno::WebSocketStream> StreamInterface::open(
  ros::NodeHandle & pnh, const std::string & path,
  hertzinno::WebSocketStream::MessageCallback on_message)
{
  auto cam = cam_;
  auto stream = std::make_unique<hertzinno::WebSocketStream>(
    streamOptions(pnh, *cam_, path), [cam] {return cam->authHeaders();}, std::move(on_message),
    logStream, [cam] {
      std::string err;
      cam->login(&err);
    });
  stream->start();
  return stream;
}

void StreamInterface::onFft(const hertzinno::WebSocketClient::Message & m)
{
  if (isTextReply(m) || m.data.size() < 2 || spectrum_pub_.getNumSubscribers() == 0) {
    return;
  }
  Spectrum s;
  s.header.stamp = ros::Time::now();
  s.header.frame_id = frame_id_;
  s.magnitude = hertzinno::decodeInt16(m.data);
  const auto c = state_->config();
  s.bin_hz = static_cast<float>(hertzinno::binHz(c ? c->samplerate : 0, s.magnitude.size()));
  s.freq_down_limit = c ? c->freq_down_limit : -1;
  s.freq_up_limit = c ? c->freq_up_limit : -1;
  s.band_peak_hz = 0.0f;
  if (c && c->freq_down_limit >= 0 && c->freq_up_limit > c->freq_down_limit) {
    const auto p = hertzinno::bandPeak(
      s.magnitude, s.bin_hz, c->freq_down_limit * 1000.0, c->freq_up_limit * 1000.0);
    if (p.index >= 0) {
      s.band_peak = p.value;
      s.band_peak_hz = static_cast<float>(p.hz);
    }
  }
  spectrum_pub_.publish(s);
}

void StreamInterface::onRaw(
  const std::string & stream, ros::Publisher & pub, const hertzinno::WebSocketClient::Message & m)
{
  if (isTextReply(m) || pub.getNumSubscribers() == 0) {
    return;
  }
  RawFrame f;
  f.header.stamp = ros::Time::now();
  f.header.frame_id = frame_id_;
  f.stream = stream;
  f.data.assign(m.data.begin(), m.data.end());
  pub.publish(f);
}

}  // namespace hertzinno_acoustic_driver
