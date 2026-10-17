// Hashes and encodings the camera protocol needs (OpenSSL libcrypto underneath). No ROS.
#pragma once

#include <string>

namespace hertzinno
{

// Lower-case hex MD5.
std::string md5Hex(const std::string & data);

// Raw 20-byte SHA-1.
std::string sha1(const std::string & data);

// Standard base64 with padding, no line breaks.
std::string base64Encode(const std::string & data);

// `n` random bytes (OpenSSL RAND_bytes, std::random_device if that fails).
std::string randomBytes(size_t n);

// The password field of POST /login: MD5 of the password followed by "lfxs", 32 lower-case
// hex digits (manual 5.1.1: password 123456 -> md5("123456lfxs")).
std::string loginPasswordHash(const std::string & password);

}  // namespace hertzinno
