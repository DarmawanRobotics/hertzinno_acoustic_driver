// GStreamer pipelines and decoder order for the camera's RTSP stream
// (H.264, rtsp://<ip>:443/<path>). No ROS, no OpenCV.
#pragma once

#include <algorithm>
#include <cctype>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

namespace hertzino
{

inline std::string lower(std::string s)
{
  std::transform(s.begin(), s.end(), s.begin(),
    [](unsigned char c) {return static_cast<char>(std::tolower(c));});
  return s;
}

// codec: h264 | h265 | auto; decoder: jetson | software.
// jetson needs the codec; software with codec auto lets decodebin pick.
// BGR frames out, appsink keeps only the newest one.
inline std::string gstreamerPipeline(
  const std::string & url, const std::string & codec_in, const std::string & decoder,
  int latency_ms = 0, const std::string & protocols = "tcp", int timeout_ms = 5000)
{
  const std::string codec = lower(codec_in);
  if (codec != "h264" && codec != "h265" && codec != "auto") {
    throw std::invalid_argument("codec must be h264, h265 or auto, not '" + codec_in + "'");
  }
  std::ostringstream p;
  // tcp-timeout (us): a dead stream ends read() instead of blocking forever.
  p << "rtspsrc location=" << url << " latency=" << latency_ms << " protocols=" << protocols
    << " tcp-timeout=" << static_cast<long long>(timeout_ms) * 1000 << " ! ";
  if (decoder == "jetson") {
    if (codec == "auto") {
      throw std::invalid_argument("the jetson decoder needs codec h264 or h265");
    }
    p << "rtp" << codec << "depay ! " << codec << "parse ! nvv4l2decoder ! nvvidconv ! "
      << "video/x-raw,format=BGRx ! videoconvert";
  } else if (decoder == "software") {
    if (codec == "auto") {
      p << "decodebin ! videoconvert";
    } else {
      p << "rtp" << codec << "depay ! " << codec << "parse ! avdec_" << codec << " ! videoconvert";
    }
  } else {
    throw std::invalid_argument("no GStreamer pipeline for decoder '" + decoder + "'");
  }
  p << " ! video/x-raw,format=BGR ! appsink drop=true max-buffers=1 sync=false";
  return p.str();
}

// Decoders to try, in order. auto: jetson (when the codec is known), software, ffmpeg.
inline std::vector<std::string> decoderCandidates(
  const std::string & decoder_in, const std::string & codec)
{
  const std::string decoder = lower(decoder_in);
  if (decoder == "auto") {
    std::vector<std::string> c;
    if (lower(codec) != "auto") {
      c.push_back("jetson");
    }
    c.push_back("software");
    c.push_back("ffmpeg");
    return c;
  }
  if (decoder == "jetson" || decoder == "software" || decoder == "ffmpeg") {
    return {decoder};
  }
  throw std::invalid_argument(
    "decoder must be auto, jetson, software or ffmpeg, not '" + decoder_in + "'");
}

// True when cv::getBuildInformation() lists GStreamer support.
inline bool opencvHasGstreamer(const std::string & build_information)
{
  std::istringstream in(build_information);
  std::string line;
  while (std::getline(in, line)) {
    const auto pos = line.find("GStreamer:");
    if (pos != std::string::npos) {
      return line.find("YES", pos) != std::string::npos;
    }
  }
  return false;
}

}  // namespace hertzino
