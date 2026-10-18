// RTSP capture thread: tries the decoders in order, keeps the one that worked, reconnects with
// back-off. No ROS.
#pragma once

#include <atomic>
#include <condition_variable>
#include <functional>
#include <mutex>
#include <string>
#include <thread>

#include <opencv2/core.hpp>

namespace hertzino
{

struct CaptureConfig
{
  std::string url;
  std::string codec{"auto"};      // h264 | h265 | auto
  std::string decoder{"auto"};    // auto | jetson | software | ffmpeg
  int latency_ms{0};              // rtspsrc jitter buffer
  std::string protocols{"tcp"};   // rtspsrc / ffmpeg rtsp_transport
  double reconnect_sec{1.0};      // first retry delay, doubles up to 30 s
  int timeout_ms{5000};           // ffmpeg open/read timeout
};

class RtspCapture
{
public:
  using FrameCallback = std::function<void (const cv::Mat &)>;
  using LogCallback = std::function<void (const std::string &)>;

  RtspCapture(CaptureConfig config, FrameCallback on_frame, LogCallback on_log = nullptr);
  ~RtspCapture();

  RtspCapture(const RtspCapture &) = delete;
  RtspCapture & operator=(const RtspCapture &) = delete;

  void start();
  void stop();
  bool streaming() const {return streaming_.load();}
  std::string activeDecoder() const;

private:
  void loop();
  void log(const std::string & m) const;

  CaptureConfig config_;
  FrameCallback on_frame_;
  LogCallback on_log_;

  std::atomic<bool> running_{false};
  std::atomic<bool> streaming_{false};
  std::thread thread_;
  std::mutex wake_mtx_;
  std::condition_variable wake_cv_;
  mutable std::mutex decoder_mtx_;
  std::string active_decoder_;
};

}  // namespace hertzino
