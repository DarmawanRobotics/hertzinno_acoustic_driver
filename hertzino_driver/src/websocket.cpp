#include "hertzino_driver/websocket.hpp"

#include <arpa/inet.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <sstream>

#include "hertzino_driver/crypto.hpp"

namespace hertzino
{

namespace
{

using Clock = std::chrono::steady_clock;

double secondsLeft(Clock::time_point deadline)
{
  return std::chrono::duration<double>(deadline - Clock::now()).count();
}

int toMs(double sec)
{
  if (sec <= 0.0) {
    return 0;
  }
  return static_cast<int>(std::min(sec * 1000.0 + 0.999, 2.0e9));
}

std::string trim(const std::string & s)
{
  const auto b = s.find_first_not_of(" \t\r\n");
  if (b == std::string::npos) {
    return "";
  }
  const auto e = s.find_last_not_of(" \t\r\n");
  return s.substr(b, e - b + 1);
}

std::string lower(std::string s)
{
  std::transform(s.begin(), s.end(), s.begin(),
    [](unsigned char c) {return static_cast<char>(std::tolower(c));});
  return s;
}

}  // namespace

// ---------------------------------------------------------------- frames

namespace ws
{

std::string encodeFrame(uint8_t opcode, const std::string & payload, bool fin, const uint8_t * mask)
{
  std::string f;
  f.reserve(payload.size() + 14);
  f.push_back(static_cast<char>((fin ? 0x80 : 0x00) | (opcode & 0x0f)));
  const uint8_t mask_bit = mask ? 0x80 : 0x00;
  const uint64_t n = payload.size();
  if (n < 126) {
    f.push_back(static_cast<char>(mask_bit | n));
  } else if (n <= 0xffff) {
    f.push_back(static_cast<char>(mask_bit | 126));
    f.push_back(static_cast<char>((n >> 8) & 0xff));
    f.push_back(static_cast<char>(n & 0xff));
  } else {
    f.push_back(static_cast<char>(mask_bit | 127));
    for (int i = 7; i >= 0; --i) {
      f.push_back(static_cast<char>((n >> (8 * i)) & 0xff));
    }
  }
  if (mask) {
    f.append(reinterpret_cast<const char *>(mask), 4);
    const size_t off = f.size();
    f += payload;
    for (size_t i = 0; i < n; ++i) {
      f[off + i] = static_cast<char>(f[off + i] ^ mask[i % 4]);
    }
  } else {
    f += payload;
  }
  return f;
}

long decodeFrame(const std::string & buf, size_t max_payload, Frame & out, std::string * err)
{
  const auto set_err = [err](const std::string & m) {
      if (err) {
        *err = m;
      }
      return -1L;
    };
  if (buf.size() < 2) {
    return 0;
  }
  const auto b = [&buf](size_t i) {return static_cast<uint8_t>(buf[i]);};
  const bool fin = (b(0) & 0x80) != 0;
  if (b(0) & 0x70) {
    return set_err("reserved bits set without a negotiated extension");
  }
  const uint8_t opcode = b(0) & 0x0f;
  const bool control = (opcode & 0x08) != 0;
  if (!(opcode <= kBinary || (opcode >= kClose && opcode <= kPong))) {
    return set_err("unknown opcode " + std::to_string(opcode));
  }
  const bool masked = (b(1) & 0x80) != 0;
  uint64_t len = b(1) & 0x7f;
  size_t pos = 2;
  if (len == 126) {
    if (buf.size() < 4) {
      return 0;
    }
    len = (static_cast<uint64_t>(b(2)) << 8) | b(3);
    pos = 4;
  } else if (len == 127) {
    if (buf.size() < 10) {
      return 0;
    }
    len = 0;
    for (size_t i = 0; i < 8; ++i) {
      len = (len << 8) | b(2 + i);
    }
    if (len >> 63) {
      return set_err("invalid 64-bit length");
    }
    pos = 10;
  }
  if (control && (!fin || len > 125)) {
    return set_err("fragmented or oversized control frame");
  }
  if (len > max_payload) {
    return set_err("frame of " + std::to_string(len) + " bytes is above the " +
             std::to_string(max_payload) + " byte limit");
  }
  uint8_t key[4] = {0, 0, 0, 0};
  if (masked) {   // servers must not mask; accepted anyway
    if (buf.size() < pos + 4) {
      return 0;
    }
    std::memcpy(key, buf.data() + pos, 4);
    pos += 4;
  }
  if (buf.size() < pos + len) {
    return 0;
  }
  out.fin = fin;
  out.opcode = opcode;
  out.payload.assign(buf, pos, static_cast<size_t>(len));
  if (masked) {
    for (size_t i = 0; i < out.payload.size(); ++i) {
      out.payload[i] = static_cast<char>(out.payload[i] ^ key[i % 4]);
    }
  }
  return static_cast<long>(pos + len);
}

std::string acceptKey(const std::string & key)
{
  return base64Encode(sha1(key + "258EAFA5-E914-47DA-95CA-C5AB0DC85B11"));
}

}  // namespace ws

// ---------------------------------------------------------------- client

WebSocketClient::~WebSocketClient()
{
  closeSocket();
}

void WebSocketClient::closeSocket()
{
  if (fd_ >= 0) {
    ::shutdown(fd_, SHUT_RDWR);
    ::close(fd_);
    fd_ = -1;
  }
}

bool WebSocketClient::connect(
  const std::string & host, int port, const std::string & path,
  const std::vector<std::string> & headers, double timeout, std::string * err)
{
  closeSocket();
  rx_.clear();
  partial_.clear();
  in_message_ = false;
  close_sent_ = false;
  handshake_status_ = 0;
  const auto deadline = Clock::now() + std::chrono::duration_cast<Clock::duration>(
    std::chrono::duration<double>(timeout));
  const auto fail = [this, err](const std::string & m) {
      if (err) {
        *err = m;
      }
      closeSocket();
      return false;
    };

  addrinfo hints{};
  hints.ai_family = AF_UNSPEC;
  hints.ai_socktype = SOCK_STREAM;
  addrinfo * res = nullptr;
  const int rc = ::getaddrinfo(host.c_str(), std::to_string(port).c_str(), &hints, &res);
  if (rc != 0) {
    return fail("cannot resolve " + host + ": " + gai_strerror(rc));
  }
  std::string last = "no address";
  for (addrinfo * ai = res; ai; ai = ai->ai_next) {
    const int fd = ::socket(ai->ai_family, ai->ai_socktype | SOCK_CLOEXEC, ai->ai_protocol);
    if (fd < 0) {
      last = std::strerror(errno);
      continue;
    }
    ::fcntl(fd, F_SETFL, ::fcntl(fd, F_GETFL, 0) | O_NONBLOCK);
    if (::connect(fd, ai->ai_addr, ai->ai_addrlen) != 0) {
      if (errno != EINPROGRESS) {
        last = std::strerror(errno);
        ::close(fd);
        continue;
      }
      pollfd p{fd, POLLOUT, 0};
      const int pr = ::poll(&p, 1, toMs(secondsLeft(deadline)));
      int so_error = 0;
      socklen_t len = sizeof(so_error);
      if (pr <= 0 || ::getsockopt(fd, SOL_SOCKET, SO_ERROR, &so_error, &len) != 0 || so_error) {
        last = pr == 0 ? "timed out" : std::strerror(pr < 0 ? errno : so_error);
        ::close(fd);
        continue;
      }
    }
    fd_ = fd;
    break;
  }
  ::freeaddrinfo(res);
  if (fd_ < 0) {
    return fail("cannot connect to " + host + ":" + std::to_string(port) + ": " + last);
  }

  // Small frames go out at once; a camera that vanished (cable, power) is noticed in ~25 s.
  int one = 1;
  ::setsockopt(fd_, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
  ::setsockopt(fd_, SOL_SOCKET, SO_KEEPALIVE, &one, sizeof(one));
  int idle = 10, interval = 5, count = 3;
  ::setsockopt(fd_, IPPROTO_TCP, TCP_KEEPIDLE, &idle, sizeof(idle));
  ::setsockopt(fd_, IPPROTO_TCP, TCP_KEEPINTVL, &interval, sizeof(interval));
  ::setsockopt(fd_, IPPROTO_TCP, TCP_KEEPCNT, &count, sizeof(count));
#ifdef TCP_USER_TIMEOUT
  unsigned int user_timeout_ms = 20000;
  ::setsockopt(fd_, IPPROTO_TCP, TCP_USER_TIMEOUT, &user_timeout_ms, sizeof(user_timeout_ms));
#endif

  const std::string key = base64Encode(randomBytes(16));
  std::ostringstream req;
  req << "GET " << (path.empty() ? "/" : path) << " HTTP/1.1\r\n"
      << "Host: " << host << ":" << port << "\r\n"
      << "Upgrade: websocket\r\n"
      << "Connection: Upgrade\r\n"
      << "Sec-WebSocket-Key: " << key << "\r\n"
      << "Sec-WebSocket-Version: 13\r\n";
  for (const auto & h : headers) {
    req << h << "\r\n";
  }
  req << "\r\n";
  std::string e;
  if (!writeAll(req.str(), &e)) {
    return fail("handshake: " + e);
  }

  size_t end;
  while ((end = rx_.find("\r\n\r\n")) == std::string::npos) {
    if (rx_.size() > 16384) {
      return fail("handshake: answer header too long");
    }
    const Fill r = fill(secondsLeft(deadline), &e);
    if (r == Fill::kTimeout) {
      return fail("handshake: no answer");
    }
    if (r != Fill::kData) {
      return fail("handshake: " + e);
    }
  }
  const std::string head = rx_.substr(0, end);
  rx_.erase(0, end + 4);   // anything after the header is already WebSocket data

  std::istringstream in(head);
  std::string line;
  std::getline(in, line);
  line = trim(line);
  int status = 0;
  const auto sp = line.find(' ');
  if (sp != std::string::npos) {
    status = std::atoi(line.c_str() + sp + 1);
  }
  if (status != 101) {
    handshake_status_ = status;
    return fail("handshake refused: '" + line + "'");
  }
  std::string accept;
  while (std::getline(in, line)) {
    const auto colon = line.find(':');
    if (colon == std::string::npos) {
      continue;
    }
    if (lower(trim(line.substr(0, colon))) == "sec-websocket-accept") {
      accept = trim(line.substr(colon + 1));
    }
  }
  // An embedded server that leaves the header out is tolerated; a wrong one is not.
  if (!accept.empty() && accept != ws::acceptKey(key)) {
    return fail("handshake: wrong Sec-WebSocket-Accept");
  }
  return true;
}

WebSocketClient::Fill WebSocketClient::fill(double timeout, std::string * err)
{
  if (fd_ < 0) {
    if (err) {
      *err = "not connected";
    }
    return Fill::kError;
  }
  pollfd p{fd_, POLLIN, 0};
  const int pr = ::poll(&p, 1, toMs(timeout));
  if (pr == 0) {
    return Fill::kTimeout;
  }
  if (pr < 0) {
    if (errno == EINTR) {
      return Fill::kTimeout;
    }
    if (err) {
      *err = std::strerror(errno);
    }
    return Fill::kError;
  }
  char buf[65536];
  const ssize_t n = ::recv(fd_, buf, sizeof(buf), 0);
  if (n > 0) {
    rx_.append(buf, static_cast<size_t>(n));
    return Fill::kData;
  }
  if (n == 0) {
    if (err) {
      *err = "connection closed by the camera";
    }
    return Fill::kEof;
  }
  if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) {
    return Fill::kData;
  }
  if (err) {
    *err = std::strerror(errno);
  }
  return Fill::kError;
}

bool WebSocketClient::writeAll(const std::string & data, std::string * err)
{
  size_t sent = 0;
  const auto deadline = Clock::now() + std::chrono::seconds(5);
  while (sent < data.size()) {
    const ssize_t n = ::send(fd_, data.data() + sent, data.size() - sent, MSG_NOSIGNAL);
    if (n > 0) {
      sent += static_cast<size_t>(n);
      continue;
    }
    if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR)) {
      pollfd p{fd_, POLLOUT, 0};
      if (::poll(&p, 1, toMs(secondsLeft(deadline))) <= 0) {
        if (err) {
          *err = "send timed out";
        }
        return false;
      }
      continue;
    }
    if (err) {
      *err = n == 0 ? "send failed" : std::strerror(errno);
    }
    return false;
  }
  return true;
}

bool WebSocketClient::send(uint8_t opcode, const std::string & payload, std::string * err)
{
  if (fd_ < 0) {
    if (err) {
      *err = "not connected";
    }
    return false;
  }
  const std::string mask = randomBytes(4);
  const std::string frame = ws::encodeFrame(
    opcode, payload, true, reinterpret_cast<const uint8_t *>(mask.data()));
  if (!writeAll(frame, err)) {
    closeSocket();
    return false;
  }
  if (opcode == ws::kClose) {
    close_sent_ = true;
  }
  return true;
}

void WebSocketClient::close(uint16_t code)
{
  if (fd_ < 0) {
    return;
  }
  if (!close_sent_) {
    std::string payload;
    payload.push_back(static_cast<char>(code >> 8));
    payload.push_back(static_cast<char>(code & 0xff));
    send(ws::kClose, payload, nullptr);
  }
  closeSocket();
}

WebSocketClient::Status WebSocketClient::read(Message & out, double timeout, std::string * err)
{
  const auto deadline = Clock::now() + std::chrono::duration_cast<Clock::duration>(
    std::chrono::duration<double>(timeout));
  const auto protocol_error = [this, err](const std::string & m, uint16_t code) {
      if (err) {
        *err = m;
      }
      close(code);
      return Status::kError;
    };
  while (true) {
    if (fd_ < 0) {
      if (err) {
        *err = "not connected";
      }
      return Status::kClosed;
    }
    ws::Frame f;
    std::string e;
    const long used = ws::decodeFrame(rx_, max_message_, f, &e);
    if (used < 0) {
      return protocol_error("protocol error: " + e, 1002);
    }
    if (used > 0) {
      rx_.erase(0, static_cast<size_t>(used));
      switch (f.opcode) {
        case ws::kPing:
          if (!send(ws::kPong, f.payload, err)) {
            return Status::kError;
          }
          continue;
        case ws::kPong:
          continue;
        case ws::kClose: {
            int code = 1005;
            if (f.payload.size() >= 2) {
              code = (static_cast<uint8_t>(f.payload[0]) << 8) | static_cast<uint8_t>(f.payload[1]);
            }
            if (!close_sent_) {
              send(ws::kClose, f.payload.substr(0, 2), nullptr);
            }
            closeSocket();
            if (err) {
              *err = "closed by the camera (code " + std::to_string(code) +
                (f.payload.size() > 2 ? ": " + f.payload.substr(2) : std::string()) + ")";
            }
            return Status::kClosed;
          }
        case ws::kText:
        case ws::kBinary:
          if (in_message_) {
            return protocol_error("protocol error: new message inside a fragmented one", 1002);
          }
          if (f.fin) {
            out.binary = f.opcode == ws::kBinary;
            out.data = std::move(f.payload);
            return Status::kMessage;
          }
          in_message_ = true;
          partial_opcode_ = f.opcode;
          partial_ = std::move(f.payload);
          continue;
        default:   // continuation
          if (!in_message_) {
            return protocol_error("protocol error: continuation without a message", 1002);
          }
          if (partial_.size() + f.payload.size() > max_message_) {
            return protocol_error("message above the " + std::to_string(max_message_) +
                     " byte limit", 1009);
          }
          partial_ += f.payload;
          if (f.fin) {
            out.binary = partial_opcode_ == ws::kBinary;
            out.data = std::move(partial_);
            partial_.clear();
            in_message_ = false;
            return Status::kMessage;
          }
          continue;
      }
    }
    const double left = secondsLeft(deadline);
    if (left <= 0.0) {
      return Status::kTimeout;
    }
    const Fill r = fill(left, err);
    if (r == Fill::kTimeout) {
      return Status::kTimeout;
    }
    if (r == Fill::kEof) {
      closeSocket();
      return Status::kClosed;
    }
    if (r == Fill::kError) {
      closeSocket();
      return Status::kError;
    }
  }
}

// ---------------------------------------------------------------- stream

WebSocketStream::WebSocketStream(
  Options options, HeaderProvider headers, MessageCallback on_message, LogCallback on_log,
  UnauthorizedCallback on_unauthorized)
: options_(std::move(options)), headers_(std::move(headers)), on_message_(std::move(on_message)),
  on_log_(std::move(on_log)), on_unauthorized_(std::move(on_unauthorized))
{
  if (options_.path.empty() || options_.path[0] != '/') {
    options_.path = "/" + options_.path;
  }
}

WebSocketStream::~WebSocketStream()
{
  stop();
}

std::string WebSocketStream::url() const
{
  return "ws://" + options_.host + ":" + std::to_string(options_.port) + options_.path;
}

void WebSocketStream::start()
{
  if (running_.exchange(true)) {
    return;
  }
  thread_ = std::thread(&WebSocketStream::loop, this);
}

void WebSocketStream::stop()
{
  if (!running_.exchange(false)) {
    return;
  }
  wake_cv_.notify_all();
  if (thread_.joinable()) {
    thread_.join();
  }
}

void WebSocketStream::log(const std::string & m) const
{
  if (on_log_) {
    on_log_(m);
  }
}

void WebSocketStream::wait(double sec)
{
  std::unique_lock<std::mutex> lk(wake_mtx_);
  wake_cv_.wait_for(lk, std::chrono::duration<double>(sec), [this] {return !running_.load();});
}

void WebSocketStream::loop()
{
  double delay = std::max(0.1, options_.reconnect_sec);
  std::string last_error;
  while (running_) {
    WebSocketClient client;
    client.setMaxMessage(options_.max_message);
    std::string err;
    const auto headers = headers_ ? headers_() : std::vector<std::string>{};
    if (!client.connect(options_.host, options_.port, options_.path, headers,
      options_.connect_timeout, &err))
    {
      if (err != last_error) {   // once per kind of failure, not every retry
        log("cannot open " + url() + ": " + err + " (retrying)");
        last_error = err;
      }
      if (client.handshakeStatus() == 401 && on_unauthorized_) {
        on_unauthorized_();
        wait(std::min(delay, 1.0));
      } else {
        wait(delay);
        delay = std::min(delay * 2.0, 30.0);
      }
      continue;
    }
    ++connections_;
    connected_ = true;
    last_error.clear();
    delay = std::max(0.1, options_.reconnect_sec);
    log("connected to " + url());

    auto last_send = Clock::now();
    auto last_message = Clock::now();
    while (running_) {
      WebSocketClient::Message msg;
      const auto st = client.read(msg, 0.25, &err);
      const auto now = Clock::now();
      if (st == WebSocketClient::Status::kMessage) {
        last_message = now;
        ++messages_;
        if (on_message_) {
          on_message_(msg);
        }
      } else if (st != WebSocketClient::Status::kTimeout) {
        log(url() + ": " + (err.empty() ? std::string("connection lost") : err) + ", reconnecting");
        break;
      }
      if (options_.heartbeat_sec > 0.0 &&
        std::chrono::duration<double>(now - last_send).count() >= options_.heartbeat_sec)
      {
        if (!client.sendText(options_.heartbeat_text, &err)) {
          log(url() + ": heartbeat failed (" + err + "), reconnecting");
          break;
        }
        last_send = now;
      }
      if (options_.stale_sec > 0.0 &&
        std::chrono::duration<double>(now - last_message).count() > options_.stale_sec)
      {
        log(url() + ": nothing for " + std::to_string(static_cast<int>(options_.stale_sec)) +
          " s, reconnecting");
        break;
      }
    }
    connected_ = false;
    client.close();
    if (running_) {
      wait(delay);
    }
  }
}

}  // namespace hertzino
