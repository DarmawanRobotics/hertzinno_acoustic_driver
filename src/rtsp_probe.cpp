#include "hertzinno_acoustic_driver/rtsp_probe.hpp"

#include <fcntl.h>
#include <netdb.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <map>
#include <sstream>

#include "hertzinno_acoustic_driver/crypto.hpp"
#include "hertzinno_acoustic_driver/http_client.hpp"

namespace hertzinno
{

namespace
{

using Clock = std::chrono::steady_clock;

int msLeft(Clock::time_point deadline)
{
  const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
    deadline - Clock::now()).count();
  return static_cast<int>(std::max<long long>(0, std::min<long long>(ms, 1000000)));
}

std::string lower(std::string s)
{
  std::transform(s.begin(), s.end(), s.begin(),
    [](unsigned char c) {return static_cast<char>(std::tolower(c));});
  return s;
}

std::string trim(const std::string & s)
{
  const auto b = s.find_first_not_of(" \t\r\n");
  if (b == std::string::npos) {
    return "";
  }
  return s.substr(b, s.find_last_not_of(" \t\r\n") - b + 1);
}

int connectTcp(const std::string & host, int port, Clock::time_point deadline, std::string * err)
{
  addrinfo hints{};
  hints.ai_family = AF_UNSPEC;
  hints.ai_socktype = SOCK_STREAM;
  addrinfo * res = nullptr;
  const int rc = ::getaddrinfo(host.c_str(), std::to_string(port).c_str(), &hints, &res);
  if (rc != 0) {
    *err = "cannot resolve " + host + ": " + gai_strerror(rc);
    return -1;
  }
  int fd = -1;
  std::string last = "no address";
  for (addrinfo * ai = res; ai && fd < 0; ai = ai->ai_next) {
    const int s = ::socket(ai->ai_family, ai->ai_socktype | SOCK_CLOEXEC, ai->ai_protocol);
    if (s < 0) {
      last = std::strerror(errno);
      continue;
    }
    ::fcntl(s, F_SETFL, ::fcntl(s, F_GETFL, 0) | O_NONBLOCK);
    if (::connect(s, ai->ai_addr, ai->ai_addrlen) != 0) {
      if (errno != EINPROGRESS) {
        last = std::strerror(errno);
        ::close(s);
        continue;
      }
      pollfd p{s, POLLOUT, 0};
      const int pr = ::poll(&p, 1, msLeft(deadline));
      int so_error = 0;
      socklen_t len = sizeof(so_error);
      if (pr <= 0 || ::getsockopt(s, SOL_SOCKET, SO_ERROR, &so_error, &len) != 0 || so_error) {
        last = pr == 0 ? "timed out" : std::strerror(pr < 0 ? errno : so_error);
        ::close(s);
        continue;
      }
    }
    fd = s;
  }
  ::freeaddrinfo(res);
  if (fd < 0) {
    *err = "cannot connect to " + host + ":" + std::to_string(port) + ": " + last;
  }
  return fd;
}

// key=value pairs of an authentication challenge (values may be quoted and contain commas)
std::map<std::string, std::string> challengeParams(const std::string & s)
{
  std::map<std::string, std::string> out;
  size_t i = 0;
  while (i < s.size()) {
    while (i < s.size() && (s[i] == ' ' || s[i] == ',' || s[i] == '\t')) {
      ++i;
    }
    const size_t eq = s.find('=', i);
    if (eq == std::string::npos) {
      break;
    }
    const std::string key = lower(trim(s.substr(i, eq - i)));
    i = eq + 1;
    std::string value;
    if (i < s.size() && s[i] == '"') {
      ++i;
      while (i < s.size() && s[i] != '"') {
        if (s[i] == '\\' && i + 1 < s.size()) {
          ++i;
        }
        value.push_back(s[i++]);
      }
      ++i;   // closing quote
    } else {
      const size_t end = s.find(',', i);
      value = trim(s.substr(i, end == std::string::npos ? std::string::npos : end - i));
      i = end == std::string::npos ? s.size() : end;
    }
    out[key] = value;
  }
  return out;
}

std::string hexOf(const std::string & raw)
{
  static const char * digits = "0123456789abcdef";
  std::string out;
  for (unsigned char c : raw) {
    out.push_back(digits[c >> 4]);
    out.push_back(digits[c & 15]);
  }
  return out;
}

}  // namespace

RtspAnswer rtspRequest(
  const std::string & host, int port, const std::string & method, const std::string & url,
  const std::vector<std::string> & headers, double timeout)
{
  RtspAnswer a;
  const auto deadline = Clock::now() + std::chrono::duration_cast<Clock::duration>(
    std::chrono::duration<double>(timeout));
  const int fd = connectTcp(host, port, deadline, &a.error);
  if (fd < 0) {
    return a;
  }
  std::ostringstream req;
  req << method << " " << url << " RTSP/1.0\r\nCSeq: 1\r\nUser-Agent: hertzinno_acoustic_driver\r\n";
  if (method == "DESCRIBE") {
    req << "Accept: application/sdp\r\n";
  }
  for (const auto & h : headers) {
    req << h << "\r\n";
  }
  req << "\r\n";
  const std::string out = req.str();
  size_t sent = 0;
  while (sent < out.size()) {
    const ssize_t n = ::send(fd, out.data() + sent, out.size() - sent, MSG_NOSIGNAL);
    if (n > 0) {
      sent += static_cast<size_t>(n);
      continue;
    }
    pollfd p{fd, POLLOUT, 0};
    if ((n < 0 && errno != EAGAIN && errno != EWOULDBLOCK) || ::poll(&p, 1, msLeft(deadline)) <= 0) {
      a.error = "cannot send to " + host + ":" + std::to_string(port);
      ::close(fd);
      return a;
    }
  }
  std::string in;
  while (in.find("\r\n\r\n") == std::string::npos && in.size() < 16384) {
    pollfd p{fd, POLLIN, 0};
    if (::poll(&p, 1, msLeft(deadline)) <= 0) {
      break;
    }
    char buf[4096];
    const ssize_t n = ::recv(fd, buf, sizeof(buf), 0);
    if (n <= 0) {
      break;
    }
    in.append(buf, static_cast<size_t>(n));
  }
  ::close(fd);

  std::istringstream lines(in);
  std::string line;
  std::getline(lines, line);
  if (line.rfind("RTSP/", 0) != 0) {
    a.error = in.empty() ? "no answer from " + host + ":" + std::to_string(port) :
      "not an RTSP answer: '" + trim(line).substr(0, 80) + "'";
    return a;
  }
  const auto sp = line.find(' ');
  a.status = sp == std::string::npos ? 0 : std::atoi(line.c_str() + sp + 1);
  while (std::getline(lines, line)) {
    const auto colon = line.find(':');
    if (colon == std::string::npos) {
      continue;
    }
    if (lower(trim(line.substr(0, colon))) == "www-authenticate") {
      const std::string value = trim(line.substr(colon + 1));
      // Digest is preferred when a server offers both
      if (a.www_authenticate.empty() || lower(value).rfind("digest", 0) == 0) {
        a.www_authenticate = value;
      }
    }
  }
  return a;
}

std::string rtspAuthorization(
  const std::string & challenge, const std::string & method, const std::string & uri,
  const std::string & user, const std::string & password)
{
  const auto sp = challenge.find(' ');
  const std::string scheme = lower(challenge.substr(0, sp));
  if (scheme == "basic") {
    return "Basic " + base64Encode(user + ":" + password);
  }
  if (scheme != "digest" || sp == std::string::npos) {
    return "";
  }
  auto p = challengeParams(challenge.substr(sp + 1));
  if (!p["algorithm"].empty() && lower(p["algorithm"]) != "md5") {
    return "";
  }
  const std::string ha1 = md5Hex(user + ":" + p["realm"] + ":" + password);
  const std::string ha2 = md5Hex(method + ":" + uri);
  std::ostringstream h;
  h << "Digest username=\"" << user << "\", realm=\"" << p["realm"] << "\", nonce=\"" <<
    p["nonce"] << "\", uri=\"" << uri << "\"";
  bool qop_auth = false;
  std::istringstream qops(p["qop"]);
  std::string q;
  while (std::getline(qops, q, ',')) {
    qop_auth = qop_auth || trim(q) == "auth";
  }
  if (qop_auth) {
    const std::string nc = "00000001";
    const std::string cnonce = hexOf(randomBytes(8));
    h << ", qop=auth, nc=" << nc << ", cnonce=\"" << cnonce << "\", response=\"" <<
      md5Hex(ha1 + ":" + p["nonce"] + ":" + nc + ":" + cnonce + ":auth:" + ha2) << "\"";
  } else {
    h << ", response=\"" << md5Hex(ha1 + ":" + p["nonce"] + ":" + ha2) << "\"";
  }
  if (!p["opaque"].empty()) {
    h << ", opaque=\"" << p["opaque"] << "\"";
  }
  if (!p["algorithm"].empty()) {
    h << ", algorithm=MD5";
  }
  return h.str();
}

std::vector<std::string> defaultRtspPaths()
{
  return {"live", "stream", "h264", "main", "video", "live/main", "stream1", "ch0", "av0_0",
    "live.sdp", "video1", "1", "11", ""};
}

std::optional<std::string> probeRtspPath(
  const std::string & host, int port, const std::vector<std::string> & paths,
  const std::string & user, const std::string & password, double timeout, std::string * report)
{
  std::string tried;
  for (const auto & path : paths) {
    const std::string url = rtspUrl(host, port, path, "", "");
    RtspAnswer a = rtspRequest(host, port, "DESCRIBE", url, {}, timeout);
    if (a.status == 401 && !user.empty()) {
      const std::string auth = rtspAuthorization(a.www_authenticate, "DESCRIBE", url, user, password);
      if (!auth.empty()) {
        a = rtspRequest(host, port, "DESCRIBE", url, {"Authorization: " + auth}, timeout);
      }
    }
    if (a.status == 0) {   // nobody answering: no point in trying the other paths
      if (report) {
        *report = a.error;
      }
      return std::nullopt;
    }
    tried += (tried.empty() ? "" : ", ") + (path.empty() ? std::string("/") : path) + ": " +
      std::to_string(a.status);
    if (a.status == 200) {
      if (report) {
        *report = tried;
      }
      return path;
    }
  }
  if (report) {
    *report = tried;
  }
  return std::nullopt;
}

std::string rtspUrl(const std::string & host, int port, const std::string & path,
  const std::string & user, const std::string & password)
{
  std::string cred;
  if (!user.empty()) {
    cred = urlEncode(user) + (password.empty() ? "" : ":" + urlEncode(password)) + "@";
  }
  std::string p = path;
  while (!p.empty() && p.front() == '/') {
    p.erase(p.begin());
  }
  return "rtsp://" + cred + host + ":" + std::to_string(port) + "/" + p;
}

}  // namespace hertzinno
