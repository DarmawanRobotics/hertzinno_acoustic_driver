// AcousticCamera: the HTTP API of the Hertzinno online acoustic camera (manual 5.1). Logs in
// with POST /login (MD5 of password + "lfxs"), sends the token as "Authorization: Bearer" and
// logs in again by itself when the camera answers 401. Thread-safe. No ROS.
#pragma once

#include <atomic>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

#include "hertzinno_acoustic_driver/http_client.hpp"
#include "hertzinno_acoustic_driver/types.hpp"

namespace hertzinno
{

class AcousticCamera
{
public:
  struct Config
  {
    std::string host{"192.168.2.204"};   // factory default
    int port{80};
    std::string user{"admin"};
    std::string password;
    double timeout{3.0};                 // s, ordinary requests
    double file_timeout{30.0};           // s, photos and videos of a detection
  };

  explicit AcousticCamera(Config config);

  const Config & config() const {return config_;}
  std::string baseUrl() const;

  // POST /login. Done automatically before the first request and after a 401.
  bool login(std::string * err);
  bool loggedIn() const;
  // The last request got an HTTP answer (any status): the camera is on the network.
  bool reachable() const {return reachable_.load();}
  int logins() const {return logins_.load();}
  // {"Authorization: Bearer <token>"} once logged in (for the WebSocket handshakes).
  std::vector<std::string> authHeaders() const;

  // GET / PUT /system. The setters check the manual's limits first; only the changed field is
  // sent, as the manual asks (the band as one pair).
  std::optional<DetectionConfig> detectionConfig(std::string * err);
  bool setThreshold(int db, std::string * err);
  bool setBand(int freq_down_limit, int freq_up_limit, std::string * err);
  bool setDuration(int seconds, std::string * err);

  // GET / PUT /video_config. Fields left at 0 in `change` are not touched.
  std::optional<VideoConfig> videoConfig(std::string * err);
  bool setVideo(const VideoConfig & change, std::string * err);

  // GET / PUT /leakage_estimation (leakage mode), one field per request.
  std::optional<LeakageConfig> leakageConfig(std::string * err);
  bool setLeakage(const std::string & field, double value, std::string * err);

  // GET / PUT /signal_filtering_area. A disabled area is sent with -1 coordinates.
  std::optional<DetectionArea> detectionArea(std::string * err);
  bool setDetectionArea(const DetectionArea & area, std::string * err);

  // POST /turn_on_detect, /turn_off_detect; GET /detect_status.
  bool startDetection(std::string * err);
  bool stopDetection(std::string * err);
  std::optional<bool> detecting(std::string * err);

  // GET /detect_logs, GET /detect_file/<saving_path>/<name>, DELETE /detect_logs.
  std::optional<LogPage> logs(const LogQuery & query, std::string * err);
  std::optional<std::string> file(const std::string & saving_path, const std::string & name,
    std::string * content_type, std::string * err);
  bool deleteLogs(const std::vector<int> & ids, std::string * err);

  // Any request with the token; `body` is sent as application/json.
  HttpResponse raw(const std::string & method, const std::string & path, const std::string & body,
    double timeout = 0.0);

  // "HTTP 400: <message from the body>" / the transport error.
  static std::string describe(const HttpResponse & r);

private:
  HttpResponse call(const std::string & method, const std::string & path,
    const std::string & body = "", double timeout = 0.0);
  bool expectOk(const HttpResponse & r, const std::string & what, std::string * err);
  bool put(const std::string & path, const std::string & body, const std::string & what,
    std::string * err);
  std::string token() const;

  Config config_;
  std::unique_ptr<HttpClient> http_;
  mutable std::mutex token_mtx_;
  std::string token_;
  std::mutex login_mtx_;
  std::atomic<bool> reachable_{false};
  std::atomic<int> logins_{0};
  std::atomic<int> samplerate_{0};
};

}  // namespace hertzinno
