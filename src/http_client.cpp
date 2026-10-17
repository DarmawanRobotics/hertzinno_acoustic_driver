#include "hertzinno_acoustic_driver/http_client.hpp"

#include <curl/curl.h>

#include <cctype>
#include <cstdio>
#include <mutex>

namespace hertzinno
{

namespace
{

std::once_flag g_curl_init;

size_t onBody(char * data, size_t size, size_t nmemb, void * user)
{
  static_cast<std::string *>(user)->append(data, size * nmemb);
  return size * nmemb;
}

}  // namespace

HttpClient::HttpClient(Options options)
: options_(std::move(options))
{
  std::call_once(g_curl_init, [] {curl_global_init(CURL_GLOBAL_DEFAULT);});
  while (!options_.base_url.empty() && options_.base_url.back() == '/') {
    options_.base_url.pop_back();
  }
}

HttpClient::~HttpClient()
{
  std::lock_guard<std::mutex> lk(pool_mtx_);
  for (CURL * h : pool_) {
    curl_easy_cleanup(h);
  }
  pool_.clear();
}

CURL * HttpClient::acquire()
{
  {
    std::lock_guard<std::mutex> lk(pool_mtx_);
    if (!pool_.empty()) {
      CURL * h = pool_.back();
      pool_.pop_back();
      return h;
    }
  }
  return curl_easy_init();
}

void HttpClient::release(CURL * handle)
{
  std::lock_guard<std::mutex> lk(pool_mtx_);
  if (pool_.size() < options_.pool_size) {
    pool_.push_back(handle);
  } else {
    curl_easy_cleanup(handle);
  }
}

HttpResponse HttpClient::request(
  const std::string & method, const std::string & path, const std::string & body,
  const std::vector<std::string> & headers, double timeout)
{
  HttpResponse res;
  CURL * h = acquire();
  if (!h) {
    res.error = "curl_easy_init failed";
    return res;
  }

  // Keep the connection, reset the request. HTTPGET last: setting POSTFIELDS (even to null)
  // would switch the handle to POST.
  curl_easy_setopt(h, CURLOPT_POSTFIELDS, nullptr);
  curl_easy_setopt(h, CURLOPT_POSTFIELDSIZE, -1L);
  curl_easy_setopt(h, CURLOPT_CUSTOMREQUEST, nullptr);
  curl_easy_setopt(h, CURLOPT_NOBODY, 0L);
  curl_easy_setopt(h, CURLOPT_HTTPGET, 1L);

  const std::string url = options_.base_url + path;
  const auto ms = static_cast<long>(1000.0 * (timeout > 0.0 ? timeout : options_.timeout));
  curl_easy_setopt(h, CURLOPT_URL, url.c_str());
  curl_easy_setopt(h, CURLOPT_CONNECTTIMEOUT_MS, static_cast<long>(1000.0 * options_.connect_timeout));
  curl_easy_setopt(h, CURLOPT_TIMEOUT_MS, ms);
  curl_easy_setopt(h, CURLOPT_NOSIGNAL, 1L);
  curl_easy_setopt(h, CURLOPT_FOLLOWLOCATION, 0L);
  curl_easy_setopt(h, CURLOPT_PROXY, "");   // the camera is on the LAN: never through a proxy
  curl_easy_setopt(h, CURLOPT_WRITEFUNCTION, onBody);
  curl_easy_setopt(h, CURLOPT_WRITEDATA, &res.body);

  struct curl_slist * list = nullptr;
  for (const auto & line : headers) {
    list = curl_slist_append(list, line.c_str());
  }
  if (method != "GET") {
    if (method != "DELETE" || !body.empty()) {
      curl_easy_setopt(h, CURLOPT_POSTFIELDSIZE, static_cast<long>(body.size()));
      curl_easy_setopt(h, CURLOPT_POSTFIELDS, body.c_str());
    }
    curl_easy_setopt(h, CURLOPT_CUSTOMREQUEST, method.c_str());
  }
  list = curl_slist_append(list, "Expect:");
  curl_easy_setopt(h, CURLOPT_HTTPHEADER, list);

  const CURLcode rc = curl_easy_perform(h);
  if (rc != CURLE_OK) {
    res.error = curl_easy_strerror(rc);
  } else {
    curl_easy_getinfo(h, CURLINFO_RESPONSE_CODE, &res.status);
    char * ct = nullptr;
    if (curl_easy_getinfo(h, CURLINFO_CONTENT_TYPE, &ct) == CURLE_OK && ct) {
      res.content_type = ct;
    }
  }
  curl_easy_setopt(h, CURLOPT_HTTPHEADER, nullptr);
  curl_slist_free_all(list);

  if (rc == CURLE_OK) {
    release(h);
  } else {
    curl_easy_cleanup(h);  // a broken connection is not worth keeping
  }
  return res;
}

std::string urlEncode(const std::string & s)
{
  std::string out;
  out.reserve(s.size() * 3);
  for (unsigned char c : s) {
    if (std::isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') {
      out.push_back(static_cast<char>(c));
    } else {
      char buf[4];
      std::snprintf(buf, sizeof(buf), "%%%02X", c);
      out += buf;
    }
  }
  return out;
}

}  // namespace hertzinno
