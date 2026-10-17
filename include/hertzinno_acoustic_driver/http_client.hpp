// Small libcurl wrapper: HTTP requests with extra headers (the bearer token), a pool of reused
// handles (keep-alive while polling) and per-request timeouts. Thread-safe. No ROS.
#pragma once

#include <mutex>
#include <string>
#include <vector>

typedef void CURL;

namespace hertzinno
{

struct HttpResponse
{
  long status{0};              // HTTP status, 0 when the request never got an answer
  std::string content_type;
  std::string body;
  std::string error;           // transport error (timeout, refused, ...)

  bool ok() const {return error.empty() && status >= 200 && status < 300;}
};

class HttpClient
{
public:
  struct Options
  {
    std::string base_url;        // e.g. http://192.168.2.204:80
    double connect_timeout{2.0}; // s
    double timeout{3.0};         // s, whole request
    size_t pool_size{4};
  };

  explicit HttpClient(Options options);
  ~HttpClient();

  HttpClient(const HttpClient &) = delete;
  HttpClient & operator=(const HttpClient &) = delete;

  // `path` starts with '/'. `headers` are complete lines ("Authorization: Bearer ...").
  // `timeout` <= 0 uses Options::timeout.
  HttpResponse request(
    const std::string & method, const std::string & path, const std::string & body = "",
    const std::vector<std::string> & headers = {}, double timeout = 0.0);

  const Options & options() const {return options_;}

private:
  CURL * acquire();
  void release(CURL * handle);

  Options options_;
  std::mutex pool_mtx_;
  std::vector<CURL *> pool_;
};

// Percent-encoding (RFC 3986 unreserved characters kept): query values, URL credentials.
std::string urlEncode(const std::string & s);

}  // namespace hertzinno
