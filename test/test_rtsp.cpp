// RTSP pipelines and decoder order, the capture loop against a local video file (FFmpeg), and
// finding the RTSP path with DESCRIBE against a scripted server (plain, Digest, Basic).
#include <gtest/gtest.h>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <functional>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include <opencv2/core.hpp>
#include <opencv2/videoio.hpp>

#include "hertzinno_acoustic_driver/rtsp_capture.hpp"
#include "hertzinno_acoustic_driver/crypto.hpp"
#include "hertzinno_acoustic_driver/rtsp_pipeline.hpp"
#include "hertzinno_acoustic_driver/rtsp_probe.hpp"

using namespace hertzinno;
using namespace std::chrono_literals;

TEST(RtspPipeline, SoftwareAutoUsesDecodebin)
{
  const auto p = gstreamerPipeline("rtsp://192.168.2.204:443/live", "auto", "software");
  EXPECT_EQ(
    p,
    "rtspsrc location=rtsp://192.168.2.204:443/live latency=0 protocols=tcp "
    "tcp-timeout=5000000 ! decodebin ! videoconvert ! video/x-raw,format=BGR ! "
    "appsink drop=true max-buffers=1 sync=false");
}

TEST(RtspPipeline, CodecSpecificDecoders)
{
  const auto sw = gstreamerPipeline("rtsp://x:443/live", "H265", "software", 50, "udp", 2000);
  EXPECT_NE(sw.find("latency=50 protocols=udp tcp-timeout=2000000"), std::string::npos);
  EXPECT_NE(sw.find("rtph265depay ! h265parse ! avdec_h265"), std::string::npos);
  const auto hw = gstreamerPipeline("rtsp://x:443/live", "h264", "jetson");
  EXPECT_NE(hw.find("rtph264depay ! h264parse ! nvv4l2decoder ! nvvidconv"), std::string::npos);
  EXPECT_NE(hw.find("appsink drop=true max-buffers=1"), std::string::npos);
}

TEST(RtspPipeline, RejectsBadCombinations)
{
  EXPECT_THROW(gstreamerPipeline("u", "mjpeg", "software"), std::invalid_argument);
  EXPECT_THROW(gstreamerPipeline("u", "auto", "jetson"), std::invalid_argument);
  EXPECT_THROW(gstreamerPipeline("u", "h264", "ffmpeg"), std::invalid_argument);
  EXPECT_THROW(decoderCandidates("cuda", "h264"), std::invalid_argument);
}

TEST(RtspPipeline, DecoderOrder)
{
  EXPECT_EQ(decoderCandidates("auto", "h265"),
    (std::vector<std::string>{"jetson", "software", "ffmpeg"}));
  EXPECT_EQ(decoderCandidates("AUTO", "auto"), (std::vector<std::string>{"software", "ffmpeg"}));
  EXPECT_EQ(decoderCandidates("ffmpeg", "auto"), (std::vector<std::string>{"ffmpeg"}));
}

TEST(RtspPipeline, DetectsGstreamerInBuildInfo)
{
  EXPECT_TRUE(opencvHasGstreamer("  Video I/O:\n    GStreamer:                   YES (1.20.3)\n"));
  EXPECT_FALSE(opencvHasGstreamer("  Video I/O:\n    GStreamer:                   NO\n"));
  EXPECT_FALSE(opencvHasGstreamer("  Video I/O:\n    FFMPEG: YES\n"));
}

TEST(RtspCapture, ConstructorValidatesParameters)
{
  CaptureConfig c;
  c.decoder = "cuda";
  EXPECT_THROW(RtspCapture(c, [](const cv::Mat &) {}), std::invalid_argument);
  c.decoder = "jetson";
  c.codec = "auto";
  EXPECT_THROW(RtspCapture(c, [](const cv::Mat &) {}), std::invalid_argument);
  c.decoder = "auto";
  c.codec = "vp9";
  EXPECT_THROW(RtspCapture(c, [](const cv::Mat &) {}), std::invalid_argument);
}

// The FFmpeg path end to end, with a file instead of the camera: frames arrive, and the loop
// reopens the source when it ends (like a dropped stream).
TEST(RtspCapture, FfmpegFramesAndReconnect)
{
  const std::string path = ::testing::TempDir() + "hertzinno_capture_test.avi";
  {
    cv::VideoWriter w(path, cv::VideoWriter::fourcc('M', 'J', 'P', 'G'), 10.0, cv::Size(64, 48));
    if (!w.isOpened()) {
      GTEST_SKIP() << "OpenCV cannot write test video (no FFmpeg backend)";
    }
    for (int i = 0; i < 5; ++i) {
      w.write(cv::Mat(48, 64, CV_8UC3, cv::Scalar(i * 40, 0, 0)));
    }
  }

  CaptureConfig c;
  c.url = path;
  c.decoder = "ffmpeg";
  c.reconnect_sec = 0.1;
  std::atomic<int> frames{0};
  std::atomic<int> width{0};
  std::atomic<int> opens{0};
  RtspCapture cap(
    c, [&](const cv::Mat & m) {width = m.cols; ++frames;},
    [&](const std::string & msg) {
      if (msg.rfind("streaming", 0) == 0) {
        ++opens;
      }
    });
  cap.start();
  const auto end = std::chrono::steady_clock::now() + 5s;
  while (std::chrono::steady_clock::now() < end && opens < 2) {
    std::this_thread::sleep_for(10ms);
  }
  cap.stop();
  std::remove(path.c_str());
  EXPECT_GE(opens.load(), 2);   // played through, reopened
  EXPECT_GE(frames.load(), 5);
  EXPECT_EQ(width.load(), 64);
  EXPECT_EQ(cap.activeDecoder(), "ffmpeg");
}

// ---------------------------------------------------------------- path discovery

namespace
{

// Answers every RTSP request on its own connection with handler(request).
class RtspServer
{
public:
  using Handler = std::function<std::string (const std::string & request)>;

  explicit RtspServer(Handler handler)
  : handler_(std::move(handler))
  {
    fd_ = ::socket(AF_INET, SOCK_STREAM, 0);
    sockaddr_in a{};
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    ::bind(fd_, reinterpret_cast<sockaddr *>(&a), sizeof(a));
    socklen_t len = sizeof(a);
    ::getsockname(fd_, reinterpret_cast<sockaddr *>(&a), &len);
    port_ = ntohs(a.sin_port);
    ::listen(fd_, 8);
    thread_ = std::thread([this] {
        while (true) {
          const int c = ::accept(fd_, nullptr, nullptr);
          if (c < 0) {
            return;
          }
          std::string req;
          char buf[2048];
          while (req.find("\r\n\r\n") == std::string::npos) {
            const ssize_t n = ::recv(c, buf, sizeof(buf), 0);
            if (n <= 0) {
              break;
            }
            req.append(buf, static_cast<size_t>(n));
          }
          {
            std::lock_guard<std::mutex> lk(mtx_);
            requests_.push_back(req);
          }
          const std::string answer = handler_(req);
          ::send(c, answer.data(), answer.size(), MSG_NOSIGNAL);
          ::close(c);
        }
      });
  }
  ~RtspServer()
  {
    ::shutdown(fd_, SHUT_RDWR);
    ::close(fd_);
    thread_.join();
  }
  int port() const {return port_;}
  std::vector<std::string> requests()
  {
    std::lock_guard<std::mutex> lk(mtx_);
    return requests_;
  }

  static std::string answer(int status, const std::string & extra = "")
  {
    static const std::map<int, std::string> reason = {
      {200, "OK"}, {401, "Unauthorized"}, {404, "Not Found"}};
    return "RTSP/1.0 " + std::to_string(status) + " " + reason.at(status) + "\r\nCSeq: 1\r\n" +
           extra + "\r\n";
  }
  static std::string path(const std::string & req)   // of "DESCRIBE rtsp://h:p/path RTSP/1.0"
  {
    const auto url_start = req.find(' ') + 1;
    const std::string url = req.substr(url_start, req.find(' ', url_start) - url_start);
    const auto slash = url.find('/', std::string("rtsp://").size());
    return url.substr(slash);
  }
  static std::string header(const std::string & req, const std::string & name)
  {
    const auto at = req.find(name + ": ");
    if (at == std::string::npos) {
      return "";
    }
    const auto start = at + name.size() + 2;
    return req.substr(start, req.find("\r\n", start) - start);
  }
  // value of key="..." / key=... in a Digest header
  static std::string param(const std::string & h, const std::string & key)
  {
    auto at = h.find(" " + key + "=");
    if (at == std::string::npos) {
      return "";
    }
    at += key.size() + 2;
    if (h[at] == '"') {
      return h.substr(at + 1, h.find('"', at + 1) - at - 1);
    }
    return h.substr(at, h.find(',', at) - at);
  }

private:
  Handler handler_;
  int fd_{-1};
  int port_{0};
  std::thread thread_;
  std::mutex mtx_;
  std::vector<std::string> requests_;
};

}  // namespace

TEST(RtspProbe, FindsTheDescribedPath)
{
  RtspServer server([](const std::string & req) {
      return RtspServer::answer(RtspServer::path(req) == "/live" ? 200 : 404,
      "Content-Type: application/sdp\r\n");
    });
  std::string report;
  const auto found = probeRtspPath("127.0.0.1", server.port(), {"stream", "h264", "live", "main"},
      "admin", "pw", 2.0, &report);
  ASSERT_TRUE(found) << report;
  EXPECT_EQ(*found, "live");
  EXPECT_EQ(report, "stream: 404, h264: 404, live: 200");
  const auto reqs = server.requests();
  ASSERT_EQ(reqs.size(), 3u);   // stops at the first hit
  EXPECT_EQ(reqs[2].rfind("DESCRIBE rtsp://127.0.0.1:" + std::to_string(server.port()) +
    "/live RTSP/1.0\r\n", 0), 0u);
  EXPECT_EQ(RtspServer::header(reqs[2], "Accept"), "application/sdp");
  EXPECT_EQ(reqs[2].find("pw"), std::string::npos);   // no credentials unless challenged
}

TEST(RtspProbe, NothingFound)
{
  RtspServer server([](const std::string &) {return RtspServer::answer(404);});
  std::string report;
  EXPECT_FALSE(probeRtspPath("127.0.0.1", server.port(), {"a", ""}, "", "", 2.0, &report));
  EXPECT_EQ(report, "a: 404, /: 404");
}

TEST(RtspProbe, DigestChallenge)
{
  const std::string realm = "HZ", nonce = "a1b2c3";
  RtspServer server([&](const std::string & req) {
      const std::string auth = RtspServer::header(req, "Authorization");
      if (auth.rfind("Digest ", 0) != 0) {
        return RtspServer::answer(401,
        "WWW-Authenticate: Basic realm=\"HZ\"\r\nWWW-Authenticate: Digest realm=\"" + realm +
        "\", nonce=\"" + nonce + "\"\r\n");
      }
      const std::string uri = RtspServer::param(auth, "uri");
      const std::string expected = md5Hex(
        md5Hex("admin:" + realm + ":secret") + ":" + nonce + ":" + md5Hex("DESCRIBE:" + uri));
      const bool ok = RtspServer::param(auth, "response") == expected &&
      RtspServer::param(auth, "username") == "admin";
      return RtspServer::answer(ok && RtspServer::path(req) == "/stream" ? 200 : (ok ? 404 : 401));
    });
  std::string report;
  auto found = probeRtspPath("127.0.0.1", server.port(), {"live", "stream"}, "admin", "secret",
      2.0, &report);
  ASSERT_TRUE(found) << report;
  EXPECT_EQ(*found, "stream");
  found = probeRtspPath("127.0.0.1", server.port(), {"stream"}, "admin", "wrong", 2.0, &report);
  EXPECT_FALSE(found);
  EXPECT_EQ(report, "stream: 401");
}

TEST(RtspProbe, DigestWithQopAndBasic)
{
  const std::string uri = "rtsp://cam:443/live";
  const std::string digest = rtspAuthorization(
    "Digest realm=\"r\", nonce=\"n\", qop=\"auth,auth-int\", opaque=\"o\", algorithm=MD5",
    "DESCRIBE", uri, "admin", "pw");
  const std::string nc = RtspServer::param(digest, "nc");
  const std::string cnonce = RtspServer::param(digest, "cnonce");
  EXPECT_EQ(nc, "00000001");
  EXPECT_EQ(cnonce.size(), 16u);
  EXPECT_EQ(RtspServer::param(digest, "response"),
    md5Hex(md5Hex("admin:r:pw") + ":n:" + nc + ":" + cnonce + ":auth:" + md5Hex("DESCRIBE:" + uri)));
  EXPECT_NE(digest.find("opaque=\"o\""), std::string::npos);
  EXPECT_EQ(rtspAuthorization("Basic realm=\"x\"", "DESCRIBE", uri, "admin", "pw"),
    "Basic YWRtaW46cHc=");
  EXPECT_EQ(rtspAuthorization("Digest realm=\"r\", nonce=\"n\", algorithm=SHA-256", "DESCRIBE",
    uri, "a", "b"), "");
}

TEST(RtspProbe, UnreachableStopsAtOnce)
{
  int port = 0;
  {
    RtspServer s([](const std::string &) {return std::string();});
    port = s.port();
  }
  std::string report;
  const auto t0 = std::chrono::steady_clock::now();
  EXPECT_FALSE(probeRtspPath("127.0.0.1", port, defaultRtspPaths(), "admin", "pw", 2.0, &report));
  EXPECT_LT(std::chrono::steady_clock::now() - t0, 1s);
  EXPECT_NE(report.find("cannot connect"), std::string::npos) << report;
}

TEST(RtspProbe, UrlsAndDefaults)
{
  EXPECT_EQ(rtspUrl("192.168.2.204", 443, "live", "admin", "p@ss:w/rd"),
    "rtsp://admin:p%40ss%3Aw%2Frd@192.168.2.204:443/live");
  EXPECT_EQ(rtspUrl("192.168.2.204", 443, "/live/main", "admin", ""),
    "rtsp://admin@192.168.2.204:443/live/main");
  EXPECT_EQ(rtspUrl("cam", 554, "", "", "x"), "rtsp://cam:554/");
  const auto paths = defaultRtspPaths();
  EXPECT_EQ(paths.front(), "live");
  EXPECT_EQ(paths.back(), "");
}
