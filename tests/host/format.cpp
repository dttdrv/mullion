// contract: str::format writes a byte as its number, as it writes every other integer (a sample count of 8 is "8",
// not the character 8, backspace), and a char still as its character
#include "../../src/util/util_string.hpp"
#include <cstdio>

int
main() {
  using mullion::str::format;
  unsigned failures = 0;
  auto check = [&](const std::string &got, const std::string &want) {
    if (got != want)
      failures++, std::printf("got \"%s\", want \"%s\"\n", got.c_str(), want.c_str());
  };
  for (unsigned value : {0u, 8u, 65u, 255u}) {
    check(format(uint8_t(value)), std::to_string(value));
    check(format("x", uint8_t(value), 'y'), "x" + std::to_string(value) + "y");
  }
  check(format('a', "b"), "ab");
  // The result owns the whole formatted sequence after the local stream is destroyed.
  const std::string payload(4096, 'x');
  check(mullion::str::format("shader:", payload, '/', 17), "shader:" + payload + "/17");
  check(mullion::str::format(std::string("a\0b", 3), 7), std::string("a\0b7", 4));

  // UTF-8 / UTF-16 / UTF-32 transcoding including 4-byte supplementary-plane code points (U+1F600).
  const std::string utf8 = "a\xC3\xA9\xE4\xB8\xAD\xF0\x9F\x98\x80z";
  const std::wstring wide = L"a\u00E9\u4E2D\U0001F600z";
  if (mullion::str::tows(utf8.c_str()) != wide)
    failures++, std::printf("tows mismatch\n");
  check(mullion::str::fromws(wide.c_str()), utf8);
  check(mullion::str::fromws(mullion::str::tows(utf8.c_str()).c_str()), utf8);

  const std::u16string utf16 = u"a\u00E9\u4E2D\U0001F600z";
  std::u16string u16Out(mullion::str::transcodeString<char16_t>(nullptr, 0, utf8.data(), utf8.size()), u'\0');
  mullion::str::transcodeString(u16Out.data(), u16Out.size(), utf8.data(), utf8.size());
  if (u16Out != utf16)
    failures++, std::printf("utf8->utf16 mismatch\n");

  std::string u8FromU16(mullion::str::transcodeString<char>(nullptr, 0, utf16.data(), utf16.size()), '\0');
  mullion::str::transcodeString(u8FromU16.data(), u8FromU16.size(), utf16.data(), utf16.size());
  check(u8FromU16, utf8);

  // Tight buffer truncation for 4-byte UTF-8 and surrogate-pair UTF-16 must not write out of bounds.
  const wchar_t grin[] = L"\U0001F600";
  const size_t grinLen = mullion::str::length(grin);
  for (size_t cap = 0; cap < 4; ++cap) {
    char buf[8];
    std::memset(buf, 0x5A, sizeof(buf));
    size_t written = mullion::str::transcodeString(buf, cap, grin, grinLen);
    if (written != 0)
      failures++, std::printf("4-byte utf8 cap=%zu wrote %zu, want 0\n", cap, written);
    for (char c : buf) {
      if (c != 0x5A) {
        failures++, std::printf("4-byte utf8 cap=%zu overwrote buffer\n", cap);
        break;
      }
    }
  }
  {
    char buf[5];
    std::memset(buf, 0x5A, sizeof(buf));
    size_t written = mullion::str::transcodeString(buf, 4, grin, grinLen);
    if (written != 4 || std::string(buf, 4) != "\xF0\x9F\x98\x80" || buf[4] != 0x5A)
      failures++, std::printf("4-byte utf8 exact cap=4 failed (written=%zu)\n", written);
  }
  {
    char16_t u16buf[2] = {0x5A5A, 0x5A5A};
    if (mullion::str::encodeChar(u16buf, u16buf + 1, 0x1F600) != 0 || u16buf[0] != 0x5A5A || u16buf[1] != 0x5A5A)
      failures++, std::printf("utf16 surrogate tight cap=1 failed\n");
    if (mullion::str::encodeChar(u16buf, u16buf + 2, 0x1F600) != 2)
      failures++, std::printf("utf16 surrogate exact cap=2 failed\n");
  }
  if (mullion::str::encodeChar<char>(nullptr, nullptr, 0x110000) != 0)
    failures++, std::printf("invalid utf8 codepoint 0x110000 should return 0\n");

  std::printf("%s: %u wrong results\n", failures ? "failed" : "passed", failures);
  return failures != 0;
}
