#include "hertzino_driver/crypto.hpp"

#include <openssl/evp.h>
#include <openssl/rand.h>

#include <random>
#include <stdexcept>

namespace hertzino
{

namespace
{

std::string digest(const EVP_MD * md, const std::string & data)
{
  unsigned char out[EVP_MAX_MD_SIZE];
  unsigned int len = 0;
  if (EVP_Digest(data.data(), data.size(), out, &len, md, nullptr) != 1) {
    throw std::runtime_error("EVP_Digest failed");
  }
  return std::string(reinterpret_cast<const char *>(out), len);
}

}  // namespace

std::string md5Hex(const std::string & data)
{
  static const char * hex = "0123456789abcdef";
  const std::string raw = digest(EVP_md5(), data);
  std::string out;
  out.reserve(raw.size() * 2);
  for (unsigned char c : raw) {
    out.push_back(hex[c >> 4]);
    out.push_back(hex[c & 0x0f]);
  }
  return out;
}

std::string sha1(const std::string & data)
{
  return digest(EVP_sha1(), data);
}

std::string base64Encode(const std::string & data)
{
  static const char * table = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
  std::string out;
  out.reserve((data.size() + 2) / 3 * 4);
  size_t i = 0;
  for (; i + 2 < data.size(); i += 3) {
    const unsigned v = (static_cast<unsigned char>(data[i]) << 16) |
      (static_cast<unsigned char>(data[i + 1]) << 8) | static_cast<unsigned char>(data[i + 2]);
    out.push_back(table[(v >> 18) & 63]);
    out.push_back(table[(v >> 12) & 63]);
    out.push_back(table[(v >> 6) & 63]);
    out.push_back(table[v & 63]);
  }
  const size_t rest = data.size() - i;
  if (rest > 0) {
    unsigned v = static_cast<unsigned char>(data[i]) << 16;
    if (rest == 2) {
      v |= static_cast<unsigned char>(data[i + 1]) << 8;
    }
    out.push_back(table[(v >> 18) & 63]);
    out.push_back(table[(v >> 12) & 63]);
    out.push_back(rest == 2 ? table[(v >> 6) & 63] : '=');
    out.push_back('=');
  }
  return out;
}

std::string randomBytes(size_t n)
{
  std::string out(n, '\0');
  if (n == 0) {
    return out;
  }
  if (RAND_bytes(reinterpret_cast<unsigned char *>(&out[0]), static_cast<int>(n)) != 1) {
    std::random_device rd;
    for (auto & c : out) {
      c = static_cast<char>(rd() & 0xff);
    }
  }
  return out;
}

std::string loginPasswordHash(const std::string & password)
{
  return md5Hex(password + "lfxs");
}

}  // namespace hertzino
