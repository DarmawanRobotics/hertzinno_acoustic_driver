// WebSocket frames (RFC 6455 examples) and the client against a scripted in-process server:
// handshake checks, pings, fragmented messages, masking of what the client sends, close.
#include <gtest/gtest.h>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <functional>
#include <string>
#include <thread>
#include <vector>

#include "hertzino_driver/websocket.hpp"

using namespace hertzino;
using namespace std::chrono_literals;

// ---------------------------------------------------------------- frames

TEST(WebSocketFrames, AcceptKeyRfcExample)
{
  EXPECT_EQ(ws::acceptKey("dGhlIHNhbXBsZSBub25jZQ=="), "s3pPLMBiTxaQ9kYGzzhZRbK+xOo=");
}

TEST(WebSocketFrames, RfcHelloExamples)
{
  EXPECT_EQ(ws::encodeFrame(ws::kText, "Hello"), std::string("\x81\x05Hello", 7));
  const uint8_t mask[4] = {0x37, 0xfa, 0x21, 0x3d};
  EXPECT_EQ(ws::encodeFrame(ws::kText, "Hello", true, mask),
    std::string("\x81\x85\x37\xfa\x21\x3d\x7f\x9f\x4d\x51\x58", 11));
  ws::Frame f;
  std::string err;
  EXPECT_EQ(ws::decodeFrame(std::string("\x81\x85\x37\xfa\x21\x3d\x7f\x9f\x4d\x51\x58", 11),
    1024, f, &err), 11);
  EXPECT_EQ(f.payload, "Hello");
  EXPECT_EQ(ws::decodeFrame(std::string("\x01\x03Hel\x80\x02lo", 9), 1024, f, &err), 5);
  EXPECT_FALSE(f.fin);
  EXPECT_EQ(f.opcode, ws::kText);
  EXPECT_EQ(f.payload, "Hel");
}

TEST(WebSocketFrames, LengthEncodingsRoundTrip)
{
  for (size_t n : {0u, 125u, 126u, 256u, 65535u, 65536u, 100000u}) {
    const std::string payload(n, 'x');
    const uint8_t mask[4] = {1, 2, 3, 4};
    for (const uint8_t * m : {static_cast<const uint8_t *>(nullptr), mask}) {
      const std::string frame = ws::encodeFrame(ws::kBinary, payload, true, m);
      const size_t header = (n < 126 ? 2 : n <= 65535 ? 4 : 10) + (m ? 4 : 0);
      ASSERT_EQ(frame.size(), header + n);
      ws::Frame f;
      std::string err;
      ASSERT_EQ(ws::decodeFrame(frame, 1u << 20, f, &err), static_cast<long>(frame.size()));
      EXPECT_EQ(f.opcode, ws::kBinary);
      EXPECT_EQ(f.payload, payload);
      // every prefix is "not complete yet", never an error
      for (size_t cut : {0ul, 1ul, header - 1, frame.size() - 1}) {
        if (cut < frame.size()) {
          EXPECT_EQ(ws::decodeFrame(frame.substr(0, cut), 1u << 20, f, &err), 0);
        }
      }
    }
  }
}

TEST(WebSocketFrames, ProtocolErrors)
{
  ws::Frame f;
  std::string err;
  EXPECT_EQ(ws::decodeFrame(std::string("\xc1\x00", 2), 1024, f, &err), -1);   // RSV1
  EXPECT_EQ(ws::decodeFrame(std::string("\x83\x00", 2), 1024, f, &err), -1);   // opcode 3
  EXPECT_EQ(ws::decodeFrame(std::string("\x09\x00", 2), 1024, f, &err), -1);   // fragmented ping
  EXPECT_EQ(ws::decodeFrame(std::string("\x89\x7e\x00\x80", 4), 1024, f, &err), -1);  // 128 B ping
  EXPECT_EQ(ws::decodeFrame(ws::encodeFrame(ws::kBinary, std::string(2048, 'a')), 1024, f, &err),
    -1);
  EXPECT_NE(err.find("limit"), std::string::npos);
}

// ---------------------------------------------------------------- client vs. scripted server

namespace
{

class ScriptedServer
{
public:
  using Script = std::function<void (int fd, const std::string & request)>;

  explicit ScriptedServer(Script script)
  {
    listen_fd_ = ::socket(AF_INET, SOCK_STREAM, 0);
    sockaddr_in a{};
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    ::bind(listen_fd_, reinterpret_cast<sockaddr *>(&a), sizeof(a));
    socklen_t len = sizeof(a);
    ::getsockname(listen_fd_, reinterpret_cast<sockaddr *>(&a), &len);
    port_ = ntohs(a.sin_port);
    ::listen(listen_fd_, 1);
    thread_ = std::thread([this, script] {
        const int fd = ::accept(listen_fd_, nullptr, nullptr);
        if (fd < 0) {
          return;
        }
        std::string req;
        char buf[4096];
        while (req.find("\r\n\r\n") == std::string::npos) {
          const ssize_t n = ::recv(fd, buf, sizeof(buf), 0);
          if (n <= 0) {
            break;
          }
          req.append(buf, static_cast<size_t>(n));
        }
        request_ = req;
        script(fd, req);
        ::close(fd);
      });
  }
  ~ScriptedServer()
  {
    ::shutdown(listen_fd_, SHUT_RDWR);
    ::close(listen_fd_);
    if (thread_.joinable()) {
      thread_.join();
    }
  }
  int port() const {return port_;}
  std::string request() const {return request_;}

  static std::string header(const std::string & req, const std::string & name)
  {
    const auto at = req.find(name + ": ");
    if (at == std::string::npos) {
      return "";
    }
    const auto start = at + name.size() + 2;
    return req.substr(start, req.find("\r\n", start) - start);
  }
  static void send(int fd, const std::string & s)
  {
    ::send(fd, s.data(), s.size(), MSG_NOSIGNAL);
  }
  // Reads client frames until `count` arrived or 2 s passed.
  static std::vector<ws::Frame> readFrames(int fd, size_t count, bool * all_masked)
  {
    std::vector<ws::Frame> frames;
    std::string buf;
    *all_masked = true;
    const auto end = std::chrono::steady_clock::now() + 2s;
    while (frames.size() < count && std::chrono::steady_clock::now() < end) {
      pollfd p{fd, POLLIN, 0};
      if (::poll(&p, 1, 50) > 0) {
        char tmp[4096];
        const ssize_t n = ::recv(fd, tmp, sizeof(tmp), 0);
        if (n <= 0) {
          break;
        }
        buf.append(tmp, static_cast<size_t>(n));
      }
      while (buf.size() >= 2) {
        if (!(static_cast<uint8_t>(buf[1]) & 0x80)) {
          *all_masked = false;
        }
        ws::Frame f;
        std::string err;
        const long used = ws::decodeFrame(buf, 1 << 20, f, &err);
        if (used <= 0) {
          break;
        }
        buf.erase(0, static_cast<size_t>(used));
        frames.push_back(f);
      }
    }
    return frames;
  }

private:
  int listen_fd_{-1};
  int port_{0};
  std::thread thread_;
  std::string request_;
};

std::string upgradeAnswer(const std::string & req, const std::string & accept_override = "-")
{
  const std::string accept = accept_override == "-" ?
    ws::acceptKey(ScriptedServer::header(req, "Sec-WebSocket-Key")) : accept_override;
  std::string a = "HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n";
  if (!accept.empty()) {
    a += "Sec-WebSocket-Accept: " + accept + "\r\n";
  }
  return a + "\r\n";
}

}  // namespace

TEST(WebSocketClient, MessagesPingsFragmentsAndClose)
{
  std::vector<ws::Frame> from_client;
  bool masked = false;
  ScriptedServer server([&](int fd, const std::string & req) {
      // the 101 and the first frame in one packet: the client must keep the extra bytes
      ScriptedServer::send(fd, upgradeAnswer(req) + ws::encodeFrame(ws::kText, "hello"));
      ScriptedServer::send(fd, ws::encodeFrame(ws::kPing, "p1"));
      ScriptedServer::send(fd, ws::encodeFrame(ws::kBinary, "ab", false));
      ScriptedServer::send(fd, ws::encodeFrame(ws::kPing, "p2"));   // between fragments
      ScriptedServer::send(fd, ws::encodeFrame(ws::kContinuation, "cd", false));
      ScriptedServer::send(fd, ws::encodeFrame(ws::kContinuation, "ef", true));
      from_client = ScriptedServer::readFrames(fd, 3, &masked);     // pong, pong, heartbeat
      ScriptedServer::send(fd, ws::encodeFrame(ws::kClose, std::string("\x03\xe8", 2) + "bye"));
      ScriptedServer::readFrames(fd, 1, &masked);                    // the client's close
    });

  WebSocketClient c;
  std::string err;
  ASSERT_TRUE(c.connect("127.0.0.1", server.port(), "/fftData", {"Authorization: Bearer t0k"}, 2.0,
    &err)) << err;
  WebSocketClient::Message m;
  ASSERT_EQ(c.read(m, 2.0, &err), WebSocketClient::Status::kMessage) << err;
  EXPECT_FALSE(m.binary);
  EXPECT_EQ(m.data, "hello");
  ASSERT_EQ(c.read(m, 2.0, &err), WebSocketClient::Status::kMessage) << err;
  EXPECT_TRUE(m.binary);
  EXPECT_EQ(m.data, "abcdef");
  ASSERT_TRUE(c.sendText("heartbeat", &err)) << err;
  EXPECT_EQ(c.read(m, 3.0, &err), WebSocketClient::Status::kClosed);
  EXPECT_NE(err.find("1000: bye"), std::string::npos) << err;
  EXPECT_FALSE(c.isOpen());

  const std::string req = server.request();
  EXPECT_EQ(req.rfind("GET /fftData HTTP/1.1\r\n", 0), 0u);
  EXPECT_EQ(ScriptedServer::header(req, "Authorization"), "Bearer t0k");
  EXPECT_EQ(ScriptedServer::header(req, "Sec-WebSocket-Version"), "13");
  ASSERT_EQ(from_client.size(), 3u);
  EXPECT_EQ(from_client[0].opcode, ws::kPong);
  EXPECT_EQ(from_client[0].payload, "p1");
  EXPECT_EQ(from_client[1].payload, "p2");
  EXPECT_EQ(from_client[2].opcode, ws::kText);
  EXPECT_EQ(from_client[2].payload, "heartbeat");
  EXPECT_TRUE(masked);   // every client frame is masked
}

TEST(WebSocketClient, ReadTimesOutWithoutData)
{
  ScriptedServer server([](int fd, const std::string & req) {
      ScriptedServer::send(fd, upgradeAnswer(req));
      std::this_thread::sleep_for(600ms);
    });
  WebSocketClient c;
  std::string err;
  ASSERT_TRUE(c.connect("127.0.0.1", server.port(), "/x", {}, 2.0, &err)) << err;
  WebSocketClient::Message m;
  const auto t0 = std::chrono::steady_clock::now();
  EXPECT_EQ(c.read(m, 0.2, &err), WebSocketClient::Status::kTimeout);
  EXPECT_LT(std::chrono::steady_clock::now() - t0, 500ms);
  EXPECT_TRUE(c.isOpen());
}

TEST(WebSocketClient, RejectsWrongAcceptKey)
{
  ScriptedServer server([](int fd, const std::string & req) {
      ScriptedServer::send(fd, upgradeAnswer(req, "AAAAAAAAAAAAAAAAAAAAAAAAAAA="));
    });
  WebSocketClient c;
  std::string err;
  EXPECT_FALSE(c.connect("127.0.0.1", server.port(), "/x", {}, 2.0, &err));
  EXPECT_NE(err.find("Sec-WebSocket-Accept"), std::string::npos) << err;
}

TEST(WebSocketClient, ToleratesMissingAcceptKey)
{
  ScriptedServer server([](int fd, const std::string & req) {
      ScriptedServer::send(fd, upgradeAnswer(req, "") + ws::encodeFrame(ws::kText, "ok"));
    });
  WebSocketClient c;
  std::string err;
  ASSERT_TRUE(c.connect("127.0.0.1", server.port(), "/x", {}, 2.0, &err)) << err;
  WebSocketClient::Message m;
  EXPECT_EQ(c.read(m, 2.0, &err), WebSocketClient::Status::kMessage);
  EXPECT_EQ(m.data, "ok");
}

TEST(WebSocketClient, ReportsRefusedHandshake)
{
  ScriptedServer server([](int fd, const std::string &) {
      ScriptedServer::send(fd,
        "HTTP/1.1 401 Unauthorized\r\nContent-Length: 0\r\nConnection: close\r\n\r\n");
    });
  WebSocketClient c;
  std::string err;
  EXPECT_FALSE(c.connect("127.0.0.1", server.port(), "/x", {}, 2.0, &err));
  EXPECT_EQ(c.handshakeStatus(), 401);
  EXPECT_NE(err.find("401"), std::string::npos);
}

TEST(WebSocketClient, ConnectFailsFastOnClosedPort)
{
  // a port nobody listens on
  const int fd = ::socket(AF_INET, SOCK_STREAM, 0);
  sockaddr_in a{};
  a.sin_family = AF_INET;
  a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  ::bind(fd, reinterpret_cast<sockaddr *>(&a), sizeof(a));
  socklen_t len = sizeof(a);
  ::getsockname(fd, reinterpret_cast<sockaddr *>(&a), &len);
  const int port = ntohs(a.sin_port);
  ::close(fd);

  WebSocketClient c;
  std::string err;
  const auto t0 = std::chrono::steady_clock::now();
  EXPECT_FALSE(c.connect("127.0.0.1", port, "/x", {}, 2.0, &err));
  EXPECT_LT(std::chrono::steady_clock::now() - t0, 1s);
  EXPECT_NE(err.find("cannot connect"), std::string::npos) << err;
}

TEST(WebSocketClient, OversizedMessageIsAnError)
{
  ScriptedServer server([](int fd, const std::string & req) {
      ScriptedServer::send(fd, upgradeAnswer(req));
      ScriptedServer::send(fd, ws::encodeFrame(ws::kBinary, std::string(600, 'a'), false));
      ScriptedServer::send(fd, ws::encodeFrame(ws::kContinuation, std::string(600, 'b'), true));
      std::this_thread::sleep_for(300ms);
    });
  WebSocketClient c;
  c.setMaxMessage(1000);
  std::string err;
  ASSERT_TRUE(c.connect("127.0.0.1", server.port(), "/x", {}, 2.0, &err)) << err;
  WebSocketClient::Message m;
  EXPECT_EQ(c.read(m, 2.0, &err), WebSocketClient::Status::kError);
  EXPECT_NE(err.find("limit"), std::string::npos) << err;
  EXPECT_FALSE(c.isOpen());
}
