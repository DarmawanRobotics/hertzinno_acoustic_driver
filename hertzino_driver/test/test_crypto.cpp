// MD5 / SHA-1 / base64 against published vectors, and the login password hash of the manual.
#include <gtest/gtest.h>

#include <string>

#include "hertzino_driver/crypto.hpp"

using namespace hertzino;

namespace
{

std::string hex(const std::string & raw)
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

TEST(Crypto, Md5Vectors)
{
  EXPECT_EQ(md5Hex(""), "d41d8cd98f00b204e9800998ecf8427e");
  EXPECT_EQ(md5Hex("abc"), "900150983cd24fb0d6963f7d28e17f72");
}

TEST(Crypto, LoginHashIsMd5OfPasswordPlusLfxs)
{
  // manual 5.1.1: password 123456 -> md5("123456lfxs"), 32 lower-case hex digits
  EXPECT_EQ(loginPasswordHash("123456"), "7cb8406e5a99c917a60681762d7952c2");
  EXPECT_EQ(loginPasswordHash("123456"), md5Hex("123456lfxs"));
  EXPECT_EQ(loginPasswordHash("test12345"), "32373b5442fddd49054d1dc1b11715a5");
}

TEST(Crypto, Sha1Vectors)
{
  EXPECT_EQ(hex(sha1("abc")), "a9993e364706816aba3e25717850c26c9cd0d89d");
  EXPECT_EQ(sha1("").size(), 20u);
}

TEST(Crypto, Base64Rfc4648)
{
  EXPECT_EQ(base64Encode(""), "");
  EXPECT_EQ(base64Encode("f"), "Zg==");
  EXPECT_EQ(base64Encode("fo"), "Zm8=");
  EXPECT_EQ(base64Encode("foo"), "Zm9v");
  EXPECT_EQ(base64Encode("foob"), "Zm9vYg==");
  EXPECT_EQ(base64Encode("fooba"), "Zm9vYmE=");
  EXPECT_EQ(base64Encode("foobar"), "Zm9vYmFy");
  EXPECT_EQ(base64Encode(std::string("\xff\xfe\x00", 3)), "//4A");
}

TEST(Crypto, RandomBytes)
{
  const auto a = randomBytes(16);
  const auto b = randomBytes(16);
  EXPECT_EQ(a.size(), 16u);
  EXPECT_NE(a, b);
  EXPECT_TRUE(randomBytes(0).empty());
}
