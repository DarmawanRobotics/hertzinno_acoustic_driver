#include "hertzinno_acoustic_driver/rtsp_capture.hpp"

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <vector>

#include <opencv2/core/utility.hpp>
#include <opencv2/core/version.hpp>
#include <opencv2/videoio.hpp>

#include "hertzinno_acoustic_driver/rtsp_pipeline.hpp"

namespace hertzinno
{

RtspCapture::RtspCapture(CaptureConfig config, FrameCallback on_frame, LogCallback on_log)
: config_(std::move(config)), on_frame_(std::move(on_frame)), on_log_(std::move(on_log))
{
  // Validate here, so a bad parameter fails at startup and never inside the thread.
  config_.decoder = lower(config_.decoder);
  config_.codec = lower(config_.codec);
  decoderCandidates(config_.decoder, config_.codec);
  gstreamerPipeline(config_.url, config_.codec, "software");
  if (config_.decoder == "jetson") {
    gstreamerPipeline(config_.url, config_.codec, "jetson");
  }
}

RtspCapture::~RtspCapture() {stop();}

void RtspCapture::start()
{
  if (running_.exchange(true)) {
    return;
  }
  thread_ = std::thread(&RtspCapture::loop, this);
}

void RtspCapture::stop()
{
  if (!running_.exchange(false)) {
    return;
  }
  wake_cv_.notify_all();
  if (thread_.joinable()) {
    thread_.join();
  }
}

std::string RtspCapture::activeDecoder() const
{
  std::lock_guard<std::mutex> lk(decoder_mtx_);
  return active_decoder_;
}

void RtspCapture::log(const std::string & m) const
{
  if (on_log_) {
    on_log_(m);
  }
}

namespace
{

bool openWith(cv::VideoCapture & cap, const CaptureConfig & c, const std::string & decoder)
{
  if (decoder == "ffmpeg") {
    // Low-latency FFmpeg RTSP options, unless the user set their own. stimeout (us) bounds
    // connect/read on OpenCV builds without the timeout properties (4.2 on Focal / Foxy).
    const std::string opts = "rtsp_transport;" + c.protocols + "|stimeout;" +
      std::to_string(static_cast<long long>(c.timeout_ms) * 1000) +
      "|fflags;nobuffer|flags;low_delay";
    ::setenv("OPENCV_FFMPEG_CAPTURE_OPTIONS", opts.c_str(), 0);
#if CV_VERSION_MAJOR > 4 || (CV_VERSION_MAJOR == 4 && CV_VERSION_MINOR >= 5)
    const std::vector<int> params = {
      cv::CAP_PROP_OPEN_TIMEOUT_MSEC, c.timeout_ms,
      cv::CAP_PROP_READ_TIMEOUT_MSEC, c.timeout_ms,
    };
    if (!cap.open(c.url, cv::CAP_FFMPEG, params)) {
      return false;
    }
#else
    if (!cap.open(c.url, cv::CAP_FFMPEG)) {
      return false;
    }
#endif
    cap.set(cv::CAP_PROP_BUFFERSIZE, 1);
    return true;
  }
  const auto pipeline = gstreamerPipeline(
    c.url, c.codec, decoder, c.latency_ms, c.protocols, c.timeout_ms);
  return cap.open(pipeline, cv::CAP_GSTREAMER);
}

}  // namespace

void RtspCapture::loop()
{
  const bool has_gstreamer = opencvHasGstreamer(cv::getBuildInformation());
  const auto candidates = decoderCandidates(config_.decoder, config_.codec);
  std::string preferred;
  double delay = std::max(0.1, config_.reconnect_sec);
  bool warned_no_gst = false;

  const auto wait = [this](double sec) {
      std::unique_lock<std::mutex> lk(wake_mtx_);
      wake_cv_.wait_for(lk, std::chrono::duration<double>(sec), [this] {return !running_.load();});
    };

  while (running_) {
    std::vector<std::string> order = candidates;
    auto it = std::find(order.begin(), order.end(), preferred);
    if (it != order.end()) {
      std::rotate(order.begin(), it, it + 1);  // the decoder that worked last time goes first
    }

    cv::VideoCapture cap;
    cv::Mat frame;
    std::string used;
    std::string tried;
    for (const auto & d : order) {
      if (!running_) {
        break;
      }
      if (d != "ffmpeg" && !has_gstreamer) {
        if (!warned_no_gst) {
          log("OpenCV has no GStreamer support, skipping the " + d + " decoder");
          warned_no_gst = true;
        }
        continue;
      }
      tried += (tried.empty() ? "" : ", ") + d;
      // Opened is not enough: the first frame proves the decoder works.
      if (openWith(cap, config_, d) && cap.read(frame) && !frame.empty()) {
        used = d;
        break;
      }
      cap.release();
    }
    if (!running_) {
      break;
    }
    if (used.empty()) {
      log("cannot open " + config_.url + " (tried " + (tried.empty() ? "nothing" : tried) +
        "), retrying in " + std::to_string(static_cast<int>(delay)) + " s");
      wait(delay);
      delay = std::min(delay * 2.0, 30.0);
      continue;
    }

    preferred = used;
    {
      std::lock_guard<std::mutex> lk(decoder_mtx_);
      active_decoder_ = used;
    }
    log("streaming " + config_.url + " (" + used + " decoder, " + std::to_string(frame.cols) +
      "x" + std::to_string(frame.rows) + ")");
    delay = std::max(0.1, config_.reconnect_sec);
    streaming_ = true;
    on_frame_(frame);

    while (running_ && cap.read(frame) && !frame.empty()) {
      on_frame_(frame);
    }
    streaming_ = false;
    cap.release();
    if (running_) {
      log("lost " + config_.url + ", reconnecting");
      wait(delay);
    }
  }
}

}  // namespace hertzinno
