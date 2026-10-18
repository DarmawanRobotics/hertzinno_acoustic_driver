// Finding the RTSP path of the camera: the manual gives the port (video_port, 443) but not the
// path, so candidate paths are asked with DESCRIBE (authenticating when the server challenges)
// until one answers 200. No ROS.
#pragma once

#include <optional>
#include <string>
#include <vector>

namespace hertzino
{

struct RtspAnswer
{
  int status{0};                  // RTSP status, 0 when no answer came
  std::string www_authenticate;   // the challenge of a 401
  std::string error;              // connection error
};

// One request on a fresh TCP connection, e.g. DESCRIBE rtsp://host:port/path.
RtspAnswer rtspRequest(
  const std::string & host, int port, const std::string & method, const std::string & url,
  const std::vector<std::string> & headers, double timeout);

// "Authorization" value for a 401 challenge: Basic, or Digest MD5 (with qop=auth when offered).
std::string rtspAuthorization(
  const std::string & challenge, const std::string & method, const std::string & uri,
  const std::string & user, const std::string & password);

// Paths tried by default, most common first.
std::vector<std::string> defaultRtspPaths();

// The first of `paths` the camera describes; nullopt when none does or the camera does not
// answer. `report` gets "live: 404, stream: 200" or the connection error.
std::optional<std::string> probeRtspPath(
  const std::string & host, int port, const std::vector<std::string> & paths,
  const std::string & user, const std::string & password, double timeout, std::string * report);

// rtsp://[user[:password]@]host:port/path with percent-encoded credentials; an empty password
// leaves it out (for logs).
std::string rtspUrl(const std::string & host, int port, const std::string & path,
  const std::string & user, const std::string & password);

}  // namespace hertzino
