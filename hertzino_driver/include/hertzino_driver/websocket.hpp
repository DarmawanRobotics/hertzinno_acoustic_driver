// WebSocket client for the camera's real-time streams (manual 5.2: ws://<ip>:80/fftData,
// /prpdData, /voiceprintData, /detectFeedback). RFC 6455 over plain TCP: handshake, masked
// sends, fragmented messages, automatic pongs, closing handshake. No TLS, no extensions, no ROS.
#pragma once

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace hertzino
{

namespace ws
{

enum Opcode : uint8_t
{
  kContinuation = 0x0,
  kText = 0x1,
  kBinary = 0x2,
  kClose = 0x8,
  kPing = 0x9,
  kPong = 0xA,
};

struct Frame
{
  bool fin{true};
  uint8_t opcode{0};
  std::string payload;   // unmasked
};

// One frame. Client-to-server frames carry a 4-byte `mask`; null sends it unmasked.
std::string encodeFrame(
  uint8_t opcode, const std::string & payload, bool fin = true, const uint8_t * mask = nullptr);

// Decodes the frame at the start of `buf`: bytes used, 0 if it is not complete yet, -1 and
// *err on a protocol error.
long decodeFrame(const std::string & buf, size_t max_payload, Frame & out, std::string * err);

// Sec-WebSocket-Accept for a Sec-WebSocket-Key (RFC 6455 4.2.2).
std::string acceptKey(const std::string & key);

}  // namespace ws

// Blocking client, one thread at a time.
class WebSocketClient
{
public:
  struct Message
  {
    bool binary{false};
    std::string data;
  };
  enum class Status { kMessage, kTimeout, kClosed, kError };

  WebSocketClient() = default;
  ~WebSocketClient();

  WebSocketClient(const WebSocketClient &) = delete;
  WebSocketClient & operator=(const WebSocketClient &) = delete;

  // TCP connection and opening handshake within `timeout` s. `headers`: extra request lines
  // such as "Authorization: Bearer <token>".
  bool connect(const std::string & host, int port, const std::string & path,
    const std::vector<std::string> & headers, double timeout, std::string * err);
  // The next text or binary message, waiting up to `timeout` s. Pings are answered here.
  Status read(Message & out, double timeout, std::string * err);
  bool send(uint8_t opcode, const std::string & payload, std::string * err);
  bool sendText(const std::string & text, std::string * err) {return send(ws::kText, text, err);}
  // Closing handshake (best effort), then the socket is closed.
  void close(uint16_t code = 1000);

  bool isOpen() const {return fd_ >= 0;}
  // HTTP status of a refused handshake (401, 404, ...), 0 otherwise.
  int handshakeStatus() const {return handshake_status_;}
  void setMaxMessage(size_t bytes) {max_message_ = bytes;}

private:
  enum class Fill { kData, kTimeout, kEof, kError };
  Fill fill(double timeout, std::string * err);
  bool writeAll(const std::string & data, std::string * err);
  void closeSocket();

  int fd_{-1};
  std::string rx_;
  std::string partial_;
  uint8_t partial_opcode_{0};
  bool in_message_{false};
  bool close_sent_{false};
  size_t max_message_{16u << 20};
  int handshake_status_{0};
};

// A WebSocket kept open by a thread: reconnects with back-off, sends a text heartbeat (the
// camera drops clients that stay silent for a minute) and hands every message to a callback.
class WebSocketStream
{
public:
  struct Options
  {
    std::string host;
    int port{80};
    std::string path;                  // "/fftData"
    double connect_timeout{3.0};       // s
    double heartbeat_sec{20.0};        // s between heartbeats, 0: none
    std::string heartbeat_text{"heartbeat"};
    double stale_sec{0.0};             // reconnect after this long without a message, 0: never
    double reconnect_sec{1.0};         // first retry delay, doubles up to 30 s
    size_t max_message{16u << 20};
  };
  using HeaderProvider = std::function<std::vector<std::string>()>;
  using MessageCallback = std::function<void (const WebSocketClient::Message &)>;
  using LogCallback = std::function<void (const std::string &)>;
  using UnauthorizedCallback = std::function<void ()>;

  // `headers` is asked on every connection (the current login token); `on_unauthorized` runs
  // when the handshake is refused with 401.
  WebSocketStream(Options options, HeaderProvider headers, MessageCallback on_message,
    LogCallback on_log = nullptr, UnauthorizedCallback on_unauthorized = nullptr);
  ~WebSocketStream();

  WebSocketStream(const WebSocketStream &) = delete;
  WebSocketStream & operator=(const WebSocketStream &) = delete;

  void start();
  void stop();
  bool connected() const {return connected_.load();}
  uint64_t messages() const {return messages_.load();}
  uint64_t connections() const {return connections_.load();}
  std::string url() const;

private:
  void loop();
  void log(const std::string & m) const;
  void wait(double sec);

  Options options_;
  HeaderProvider headers_;
  MessageCallback on_message_;
  LogCallback on_log_;
  UnauthorizedCallback on_unauthorized_;

  std::atomic<bool> running_{false};
  std::atomic<bool> connected_{false};
  std::atomic<uint64_t> messages_{0};
  std::atomic<uint64_t> connections_{0};
  std::thread thread_;
  std::mutex wake_mtx_;
  std::condition_variable wake_cv_;
};

}  // namespace hertzino
