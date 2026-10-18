// AcousticCamera and WebSocketStream end to end against test/fake_device.py: token login and
// re-login, settings with the manual's limits, detections with their feedback, logs and files,
// and the WebSocket streams with heartbeats.
#include <gtest/gtest.h>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <signal.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <cstdlib>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include <nlohmann/json.hpp>

#include "hertzino_driver/api.hpp"
#include "hertzino_driver/device.hpp"
#include "hertzino_driver/websocket.hpp"

#ifndef FAKE_DEVICE_PY
#define FAKE_DEVICE_PY "test/fake_device.py"
#endif

using namespace hertzino;
using namespace std::chrono_literals;

namespace
{

int freePort()
{
  int fd = ::socket(AF_INET, SOCK_STREAM, 0);
  sockaddr_in a{};
  a.sin_family = AF_INET;
  a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  ::bind(fd, reinterpret_cast<sockaddr *>(&a), sizeof(a));
  socklen_t len = sizeof(a);
  ::getsockname(fd, reinterpret_cast<sockaddr *>(&a), &len);
  ::close(fd);
  return ntohs(a.sin_port);
}

bool listening(int port)
{
  int fd = ::socket(AF_INET, SOCK_STREAM, 0);
  sockaddr_in a{};
  a.sin_family = AF_INET;
  a.sin_port = htons(static_cast<uint16_t>(port));
  a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  const bool ok = ::connect(fd, reinterpret_cast<sockaddr *>(&a), sizeof(a)) == 0;
  ::close(fd);
  return ok;
}

class FakeDevice
{
public:
  explicit FakeDevice(std::vector<std::string> args = {})
  {
    port_ = freePort();
    args.insert(args.begin(), {"python3", FAKE_DEVICE_PY, "--port", std::to_string(port_),
        "--password", "test12345"});
    if (const char * log = std::getenv("FAKE_DEVICE_LOG")) {   // request log, for debugging
      args.push_back("--log");
      args.push_back(log);
    }
    pid_ = ::fork();
    if (pid_ == 0) {
      std::vector<char *> argv;
      for (auto & a : args) {
        argv.push_back(&a[0]);
      }
      argv.push_back(nullptr);
      ::execvp("python3", argv.data());
      ::_exit(127);
    }
    for (int i = 0; i < 200 && !listening(port_); ++i) {
      std::this_thread::sleep_for(25ms);
    }
  }
  ~FakeDevice()
  {
    ::kill(pid_, SIGTERM);
    int st = 0;
    ::waitpid(pid_, &st, 0);
  }
  int port() const {return port_;}

  AcousticCamera::Config config(const std::string & password = "test12345") const
  {
    AcousticCamera::Config c;
    c.host = "127.0.0.1";
    c.port = port_;
    c.password = password;
    c.timeout = 2.0;
    return c;
  }

  // counters of the fake (GET /debug/state)
  nlohmann::json state() const
  {
    HttpClient::Options o;
    o.base_url = "http://127.0.0.1:" + std::to_string(port_);
    HttpClient http(o);
    return nlohmann::json::parse(http.request("GET", "/debug/state").body, nullptr, false);
  }

  WebSocketStream::Options stream(const std::string & path) const
  {
    WebSocketStream::Options o;
    o.host = "127.0.0.1";
    o.port = port_;
    o.path = path;
    o.reconnect_sec = 0.2;
    return o;
  }

private:
  int port_{0};
  pid_t pid_{-1};
};

template<typename Pred>
bool waitFor(Pred pred, std::chrono::milliseconds timeout)
{
  const auto end = std::chrono::steady_clock::now() + timeout;
  while (std::chrono::steady_clock::now() < end) {
    if (pred()) {
      return true;
    }
    std::this_thread::sleep_for(20ms);
  }
  return pred();
}

}  // namespace

TEST(DeviceFake, LoginAndDetectionSettings)
{
  FakeDevice fake;
  AcousticCamera cam(fake.config());
  std::string err;
  EXPECT_FALSE(cam.loggedIn());
  const auto c = cam.detectionConfig(&err);   // logs in by itself
  ASSERT_TRUE(c) << err;
  EXPECT_TRUE(cam.loggedIn());
  EXPECT_TRUE(cam.reachable());
  EXPECT_EQ(cam.logins(), 1);
  EXPECT_EQ(c->work_mode, kPartialDischarge);
  EXPECT_EQ(c->samplerate, 96000);

  ASSERT_TRUE(cam.setThreshold(25, &err)) << err;
  ASSERT_TRUE(cam.setBand(25, 40, &err)) << err;
  ASSERT_TRUE(cam.setDuration(2, &err)) << err;
  const auto d = cam.detectionConfig(&err);
  ASSERT_TRUE(d) << err;
  EXPECT_EQ(d->db_limit, 25);
  EXPECT_EQ(d->freq_down_limit, 25);
  EXPECT_EQ(d->freq_up_limit, 40);
  EXPECT_EQ(d->detect_times, 2);

  // the manual's limits are checked before anything is sent
  EXPECT_FALSE(cam.setBand(10, 40, &err));
  EXPECT_NE(err.find("2 to 20 kHz"), std::string::npos) << err;
  EXPECT_FALSE(cam.setBand(30, 50, &err));       // above 48 kHz at 96 kHz
  EXPECT_NE(err.find("half"), std::string::npos) << err;
  EXPECT_FALSE(cam.setThreshold(120, &err));
  EXPECT_FALSE(cam.setDuration(0, &err));
  EXPECT_EQ(cam.logins(), 1);
}

TEST(DeviceFake, WrongPassword)
{
  FakeDevice fake;
  AcousticCamera cam(fake.config("wrong"));
  std::string err;
  EXPECT_FALSE(cam.login(&err));
  EXPECT_NE(err.find("HTTP 401"), std::string::npos) << err;
  EXPECT_NE(err.find("wrong user or password"), std::string::npos) << err;
  EXPECT_FALSE(cam.detectionConfig(&err));
  EXPECT_TRUE(cam.reachable());   // answering, just not accepting us
  EXPECT_FALSE(cam.loggedIn());
}

TEST(DeviceFake, ExpiredTokenLogsInAgain)
{
  FakeDevice fake({"--token-ttl", "0.5"});
  AcousticCamera cam(fake.config());
  std::string err;
  ASSERT_TRUE(cam.detecting(&err)) << err;
  std::this_thread::sleep_for(700ms);
  ASSERT_TRUE(cam.detecting(&err)) << err;   // 401, login, retried
  EXPECT_EQ(cam.logins(), 2);
  EXPECT_EQ(fake.state()["logins"], 2);
}

TEST(DeviceFake, UnreachableCamera)
{
  AcousticCamera::Config c;
  c.host = "127.0.0.1";
  c.port = freePort();
  c.password = "x";
  c.timeout = 1.0;
  AcousticCamera cam(c);
  std::string err;
  EXPECT_FALSE(cam.detectionConfig(&err));
  EXPECT_FALSE(cam.reachable());
  EXPECT_NE(err.find("login"), std::string::npos) << err;
}

TEST(DeviceFake, VideoAreaAndRaw)
{
  FakeDevice fake;
  AcousticCamera cam(fake.config());
  std::string err;
  const auto v = cam.videoConfig(&err);
  ASSERT_TRUE(v) << err;
  EXPECT_EQ(v->width, 1920);
  EXPECT_EQ(v->rtsp_port, 443);

  VideoConfig change;
  change.fps = 30;
  change.width = 1280;
  change.height = 720;
  ASSERT_TRUE(cam.setVideo(change, &err)) << err;
  const auto v2 = cam.videoConfig(&err);
  ASSERT_TRUE(v2) << err;
  EXPECT_EQ(v2->fps, 30);
  EXPECT_EQ(v2->height, 720);
  EXPECT_EQ(v2->bit_rate, 4096);   // untouched
  change = VideoConfig();
  change.width = 1000;
  change.height = 1000;
  EXPECT_FALSE(cam.setVideo(change, &err));
  EXPECT_NE(err.find("resolution"), std::string::npos);
  EXPECT_FALSE(cam.setVideo(VideoConfig(), &err));

  DetectionArea a;
  a.enabled = true;
  a.x = 100;
  a.y = 80;
  a.width = 320;
  a.height = 240;
  ASSERT_TRUE(cam.setDetectionArea(a, &err)) << err;
  auto got = cam.detectionArea(&err);
  ASSERT_TRUE(got) << err;
  EXPECT_TRUE(got->enabled);
  EXPECT_EQ(got->width, 320);
  a.enabled = false;
  ASSERT_TRUE(cam.setDetectionArea(a, &err)) << err;
  got = cam.detectionArea(&err);
  ASSERT_TRUE(got) << err;
  EXPECT_FALSE(got->enabled);
  EXPECT_EQ(got->x, -1);

  const auto r = cam.raw("GET", "/system", "");
  EXPECT_EQ(r.status, 200);
  EXPECT_NE(r.body.find("dB_limit"), std::string::npos);
  const auto missing = cam.raw("GET", "/no_such_thing", "");
  EXPECT_EQ(missing.status, 404);
  EXPECT_EQ(AcousticCamera::describe(missing), "HTTP 404: no such interface");
}

TEST(DeviceFake, LeakageSettingsOnlyInLeakageMode)
{
  {
    FakeDevice fake;
    AcousticCamera cam(fake.config());
    std::string err;
    EXPECT_FALSE(cam.leakageConfig(&err));
    EXPECT_NE(err.find("HTTP 400: leakage estimation is only available"), std::string::npos)
      << err;
  }
  FakeDevice fake({"--mode", "2"});
  AcousticCamera cam(fake.config());
  std::string err;
  ASSERT_TRUE(cam.setLeakage("monetary_unit", 2, &err)) << err;
  ASSERT_TRUE(cam.setLeakage("value_of_gas", 0.35, &err)) << err;
  const auto c = cam.leakageConfig(&err);
  ASSERT_TRUE(c) << err;
  EXPECT_EQ(c->monetary_unit_name, "USD");
  EXPECT_DOUBLE_EQ(c->value_of_gas, 0.35);
  EXPECT_FALSE(cam.setLeakage("leakage_distance", 1.5, &err));
  EXPECT_FALSE(cam.setLeakage("price", 1, &err));
}

TEST(DeviceFake, DetectionFeedbackLogsAndFiles)
{
  FakeDevice fake({"--ws-fragment"});   // the feedback JSON split in 3 frames, a ping between
  AcousticCamera cam(fake.config());
  std::string err;
  ASSERT_TRUE(cam.setDuration(1, &err)) << err;

  std::mutex mtx;
  std::vector<std::string> messages;
  WebSocketStream feedback(
    fake.stream("/detectFeedback"), [&cam] {return cam.authHeaders();},
    [&](const WebSocketClient::Message & m) {
      std::lock_guard<std::mutex> lk(mtx);
      messages.push_back(m.data);
    });
  feedback.start();
  ASSERT_TRUE(waitFor([&] {return feedback.connected();}, 3s));

  ASSERT_TRUE(cam.startDetection(&err)) << err;
  EXPECT_EQ(cam.detecting(&err), std::optional<bool>(true));
  ASSERT_TRUE(waitFor([&] {std::lock_guard<std::mutex> lk(mtx); return !messages.empty();}, 5s));
  feedback.stop();
  std::string text;
  {
    std::lock_guard<std::mutex> lk(mtx);
    text = messages.front();
  }
  const auto r = parseDetectionResult(text, &err);
  ASSERT_TRUE(r) << err << " in " << text;
  EXPECT_EQ(statusName(r->detect_mode, r->status), "surface");
  EXPECT_EQ(r->intensity, 17);
  EXPECT_DOUBLE_EQ(r->surface_prob, 0.9);
  ASSERT_TRUE(waitFor([&] {return cam.detecting(nullptr) == std::optional<bool>(false);}, 2s));

  LogQuery q;
  auto page = cam.logs(q, &err);
  ASSERT_TRUE(page) << err;
  ASSERT_EQ(page->total, 1);
  const DetectionResult log = page->logs.at(0);   // a copy: page is reused below
  EXPECT_EQ(log.saving_path, r->saving_path);
  EXPECT_EQ(log.file_status, 1);
  EXPECT_GE(log.log_id, 1);

  std::string ctype;
  const auto jpg = cam.file(log.saving_path, log.soundmap_photo, &ctype, &err);
  ASSERT_TRUE(jpg) << err;
  EXPECT_EQ(ctype, "image/jpeg");
  ASSERT_GE(jpg->size(), 2u);
  EXPECT_EQ(static_cast<unsigned char>((*jpg)[0]), 0xFF);
  EXPECT_EQ(static_cast<unsigned char>((*jpg)[1]), 0xD8);
  EXPECT_FALSE(cam.file(log.saving_path, "nope.jpg", &ctype, &err));
  EXPECT_NE(err.find("404"), std::string::npos) << err;
  EXPECT_FALSE(cam.file("../..", "x.jpg", &ctype, &err));

  q.types = 2;   // corona only
  page = cam.logs(q, &err);
  ASSERT_TRUE(page) << err;
  EXPECT_EQ(page->total, 0);

  ASSERT_TRUE(cam.deleteLogs({log.log_id}, &err)) << err;
  page = cam.logs(LogQuery(), &err);
  ASSERT_TRUE(page) << err;
  EXPECT_EQ(page->total, 0);
  EXPECT_FALSE(cam.deleteLogs({}, &err));
}

TEST(DeviceFake, StopDetection)
{
  FakeDevice fake({"--continuous"});
  AcousticCamera cam(fake.config());
  std::string err;
  ASSERT_TRUE(cam.startDetection(&err)) << err;
  EXPECT_EQ(cam.detecting(&err), std::optional<bool>(true));
  ASSERT_TRUE(cam.stopDetection(&err)) << err;
  EXPECT_EQ(cam.detecting(&err), std::optional<bool>(false));
}

TEST(DeviceFake, StreamsSurviveWithHeartbeats)
{
  // the fake drops clients silent for 1.5 s and pings its clients
  FakeDevice fake({"--ws-idle", "1.5", "--ws-ping"});
  std::atomic<size_t> size{0};
  auto o = fake.stream("/fftData");
  o.heartbeat_sec = 0.5;
  WebSocketStream fft(o, nullptr, [&](const WebSocketClient::Message & m) {
      if (m.binary) {
        size = m.data.size();
      }
    });
  fft.start();
  std::this_thread::sleep_for(4s);
  EXPECT_TRUE(fft.connected());
  EXPECT_EQ(fft.connections(), 1u);   // never dropped
  EXPECT_GE(fft.messages(), 12u);     // 5 Hz
  EXPECT_EQ(size.load(), 1024u);      // 512 int16
  fft.stop();
  const auto st = fake.state();
  EXPECT_GE(st["heartbeats"].get<int>(), 4);
  EXPECT_GE(st["pongs"].get<int>(), 1);
  EXPECT_EQ(st["unmasked"].get<int>(), 0);
  EXPECT_EQ(st["ws_idle_closed"].get<int>(), 0);
}

TEST(DeviceFake, SilentStreamIsDroppedAndReconnects)
{
  FakeDevice fake({"--ws-idle", "0.6"});
  auto o = fake.stream("/prpdData");
  o.heartbeat_sec = 0.0;   // no heartbeat: the camera closes the connection
  WebSocketStream prpd(o, nullptr, nullptr);
  prpd.start();
  EXPECT_TRUE(waitFor([&] {return prpd.connections() >= 2;}, 4s));
  prpd.stop();
  EXPECT_GE(fake.state()["ws_idle_closed"].get<int>(), 1);
}

TEST(DeviceFake, StreamLogsInAgainOn401)
{
  FakeDevice fake({"--ws-auth"});
  AcousticCamera cam(fake.config());
  std::atomic<int> unauthorized{0};
  WebSocketStream s(
    fake.stream("/voiceprintData"), [&cam] {return cam.authHeaders();}, nullptr, nullptr,
    [&] {
      ++unauthorized;
      std::string err;
      cam.login(&err);
    });
  s.start();   // no token yet: 401, login, connected
  EXPECT_TRUE(waitFor([&] {return s.connected();}, 4s));
  s.stop();
  EXPECT_GE(unauthorized.load(), 1);
  EXPECT_EQ(cam.logins(), 1);
}
