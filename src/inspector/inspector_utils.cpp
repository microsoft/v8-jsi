// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.
// This code is based on the old node inspector implementation. See LICENSE_NODE for Node.js' project license details
#include "inspector_utils.h"

// Only for MultibyteToWideChar .. Should be removed.
#include "windows.h"

#include <stdexcept>

namespace inspector {
namespace utils {

// These are copied from react-native code.
const uint16_t kUtf8OneByteBoundary = 0x80;
const uint16_t kUtf8TwoBytesBoundary = 0x800;
const uint16_t kUtf16HighSubLowBoundary = 0xD800;
const uint16_t kUtf16HighSubHighBoundary = 0xDC00;
const uint16_t kUtf16LowSubHighBoundary = 0xE000;

size_t utf16toUTF8Length(const uint16_t* utf16String, size_t utf16StringLen) {
  if (!utf16String || utf16StringLen == 0) {
    return 0;
  }

  uint32_t utf8StringLen = 0;
  auto utf16StringEnd = utf16String + utf16StringLen;
  auto idx16 = utf16String;
  while (idx16 < utf16StringEnd) {
    auto ch = *idx16++;
    if (ch < kUtf8OneByteBoundary) {
      utf8StringLen++;
    }
    else if (ch < kUtf8TwoBytesBoundary) {
      utf8StringLen += 2;
    }
    else if (
      (ch >= kUtf16HighSubLowBoundary) && (ch < kUtf16HighSubHighBoundary) &&
      (idx16 < utf16StringEnd) &&
      (*idx16 >= kUtf16HighSubHighBoundary) && (*idx16 < kUtf16LowSubHighBoundary)) {
      utf8StringLen += 4;
      idx16++;
    }
    else {
      utf8StringLen += 3;
    }
  }

  return utf8StringLen;
}

std::string utf16toUTF8(const uint16_t* utf16String, size_t utf16StringLen) noexcept {
  if (!utf16String || utf16StringLen <= 0) {
    return "";
  }

  std::string utf8String(utf16toUTF8Length(utf16String, utf16StringLen), '\0');
  auto idx8 = utf8String.begin();
  auto idx16 = utf16String;
  auto utf16StringEnd = utf16String + utf16StringLen;
  while (idx16 < utf16StringEnd) {
    auto ch = *idx16++;
    if (ch < kUtf8OneByteBoundary) {
      *idx8++ = (ch & 0x7F);
    }
    else if (ch < kUtf8TwoBytesBoundary) {
#ifdef _MSC_VER
#pragma warning(suppress: 4244)
      *idx8++ = 0b11000000 | (ch >> 6);
#else
      *idx8++ = 0b11000000 | (ch >> 6);
#endif
      *idx8++ = 0b10000000 | (ch & 0x3F);
    }
    else if (
      (ch >= kUtf16HighSubLowBoundary) && (ch < kUtf16HighSubHighBoundary) &&
      (idx16 < utf16StringEnd) &&
      (*idx16 >= kUtf16HighSubHighBoundary) && (*idx16 < kUtf16LowSubHighBoundary)) {
      auto ch2 = *idx16++;
      uint8_t trunc_byte = (((ch >> 6) & 0x0F) + 1);
      *idx8++ = 0b11110000 | (trunc_byte >> 2);
      *idx8++ = 0b10000000 | ((trunc_byte & 0x03) << 4) | ((ch >> 2) & 0x0F);
      *idx8++ = 0b10000000 | ((ch & 0x03) << 4) | ((ch2 >> 6) & 0x0F);
      *idx8++ = 0b10000000 | (ch2 & 0x3F);
    }
    else {
      *idx8++ = 0b11100000 | (ch >> 12);
      *idx8++ = 0b10000000 | ((ch >> 6) & 0x3F);
      *idx8++ = 0b10000000 | (ch & 0x3F);
    }
  }

  return utf8String;
}

std::wstring Utf8ToUtf16(const char* utf8, size_t utf8Len)
{
  std::wstring utf16{};

  if (utf8Len == 0)
  {
    return utf16;
  }

  // Extra parentheses needed here to prevent expanding max as a
  // Windows-specific preprocessor macro.
  if (utf8Len > static_cast<size_t>((std::numeric_limits<int>::max)()))
  {
    throw std::overflow_error("Input string too long: size_t-length doesn't fit into int.");
  }

  const int utf8Length = static_cast<int>(utf8Len);

  // Fail if an invalid UTF-8 character is encountered in the input string.
  constexpr DWORD flags = MB_ERR_INVALID_CHARS;

  const int utf16Length = ::MultiByteToWideChar(
    CP_UTF8,       // Source string is in UTF-8.
    flags,         // Conversion flags.
    utf8,          // Source UTF-8 string pointer.
    utf8Length,    // Length of the source UTF-8 string, in chars.
    nullptr,       // Do not convert during this step, instead
    0              //   request size of destination buffer, in wchar_ts.
  );

  if (utf16Length == 0)
  {
    throw std::runtime_error("Cannot get result string length when converting from UTF-8 to UTF-16 (MultiByteToWideChar failed).");
  }

  utf16.resize(utf16Length);

  // Convert from UTF-8 to UTF-16
  // Note that MultiByteToWideChar converts the UTF-8 BOM into the UTF-16BE BOM.
  int result = ::MultiByteToWideChar(
    CP_UTF8,       // Source string is in UTF-8.
    flags,         // Conversion flags.
    utf8,          // Source UTF-8 string pointer.
    utf8Length,    // Length of source UTF-8 string, in chars.
    &utf16[0],     // Pointer to destination buffer.
    utf16Length    // Size of destination buffer, in wchar_ts.
  );

  if (result == 0)
  {
    throw std::runtime_error("Cannot convert from UTF-8 to UTF-16 (MultiByteToWideChar failed).");
  }

  return utf16;
}

char ToLower(char c) {
  return c >= 'A' && c <= 'Z' ? c + ('a' - 'A') : c;
}

std::string ToLower(const std::string& in) {
  std::string out(in.size(), 0);
  for (size_t i = 0; i < in.size(); ++i)
    out[i] = ToLower(in[i]);
  return out;
}

bool StringEqualNoCase(const char* a, const char* b) {
  do {
    if (*a == '\0')
      return *b == '\0';
    if (*b == '\0')
      return *a == '\0';
  } while (ToLower(*a++) == ToLower(*b++));
  return false;
}

bool StringEqualNoCaseN(const char* a, const char* b, size_t length) {
  for (size_t i = 0; i < length; i++) {
    if (ToLower(a[i]) != ToLower(b[i]))
      return false;
    if (a[i] == '\0')
      return true;
  }
  return true;
}

size_t base64_encode(const char* src, size_t slen, char* dst, size_t dlen) {
  // We know how much we'll write, just make sure that there's space.
  // CHECK(dlen >= base64_encoded_size(slen) && "not enough space provided for base64 encode");

  dlen = base64_encoded_size(slen);

  unsigned a;
  unsigned b;
  unsigned c;
  unsigned i;
  unsigned k;
  unsigned n;

  static const char table[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZ"
    "abcdefghijklmnopqrstuvwxyz"
    "0123456789+/";

  i = 0;
  k = 0;
  n = static_cast<int>(slen) / 3 * 3;

  while (i < n) {
    a = src[i + 0] & 0xff;
    b = src[i + 1] & 0xff;
    c = src[i + 2] & 0xff;

    dst[k + 0] = table[a >> 2];
    dst[k + 1] = table[((a & 3) << 4) | (b >> 4)];
    dst[k + 2] = table[((b & 0x0f) << 2) | (c >> 6)];
    dst[k + 3] = table[c & 0x3f];

    i += 3;
    k += 4;
  }

  if (n != slen) {
    switch (slen - n) {
    case 1:
      a = src[i + 0] & 0xff;
      dst[k + 0] = table[a >> 2];
      dst[k + 1] = table[(a & 3) << 4];
      dst[k + 2] = '=';
      dst[k + 3] = '=';
      break;

    case 2:
      a = src[i + 0] & 0xff;
      b = src[i + 1] & 0xff;
      dst[k + 0] = table[a >> 2];
      dst[k + 1] = table[((a & 3) << 4) | (b >> 4)];
      dst[k + 2] = table[(b & 0x0f) << 2];
      dst[k + 3] = '=';
      break;
    }
  }

  return dlen;
}

}
}