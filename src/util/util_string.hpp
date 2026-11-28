/*
 * This file is part of DXMT, Copyright (c) 2023 Feifan He
 *
 * Derived from a part of DXVK (originally under zlib License),
 * Copyright (c) 2017 Philip Rebohle
 * Copyright (c) 2019 Joshua Ashton
 *
 * See <https://github.com/doitsujin/dxvk/blob/master/LICENSE>
 */

#pragma once

#include <cstring>
#include <string>
#include <sstream>
#include <vector>
#include <cstdint>
#include <type_traits>
#include <utility>
#include "unknwn.h"

namespace mullion::str {

/**
 * \brief Decodes a single character
 *
 * Note that \c begin and \c end must not be equal.
 * \param [in] begin Pointer to current position within the input string
 * \param [in] end Pointer to the end of the input string
 * \param [out] ch Pointer to the decoded character code
 * \returns Pointer to next character in the input string
 */
template <typename T>
const T *decodeChar(const T *begin, const T *end, uint32_t &ch) {
  static_assert(sizeof(T) == 1 || sizeof(T) == 2 || sizeof(T) == 4,
                "Unsupported character size");

  if constexpr (sizeof(T) == 1) {
    uint32_t first = static_cast<uint8_t>(begin[0]);

    if (first < 0x80) {
      ch = first;
      return begin + 1;
    } else if (first < 0xC0) {
      // Character starts with a continuation byte,
      // just skip until we find the next valid prefix
      while (begin < end && (static_cast<uint8_t>(*begin) & 0xC0) == 0x80)
        begin += 1;

      ch = uint32_t('?');
      return begin;
    } else if (first < 0xE0) {
      if (end - begin < 2) {
        ch = uint32_t('?');
        return end;
      }

      ch = ((first & 0x1F) << 6) |
           (static_cast<uint8_t>(begin[1]) & 0x3F);
      return begin + 2;
    } else if (first < 0xF0) {
      if (end - begin < 3) {
        ch = uint32_t('?');
        return end;
      }

      ch = ((first & 0x0F) << 12) |
           ((static_cast<uint8_t>(begin[1]) & 0x3F) << 6) |
           (static_cast<uint8_t>(begin[2]) & 0x3F);
      return begin + 3;
    } else if (first < 0xF8) {
      if (end - begin < 4) {
        ch = uint32_t('?');
        return end;
      }

      ch = ((first & 0x07) << 18) |
           ((static_cast<uint8_t>(begin[1]) & 0x3F) << 12) |
           ((static_cast<uint8_t>(begin[2]) & 0x3F) << 6) |
           (static_cast<uint8_t>(begin[3]) & 0x3F);
      return begin + 4;
    } else {
      // Invalid prefix
      ch = uint32_t('?');
      return begin + 1;
    }
  } else if constexpr (sizeof(T) == 2) {
    uint32_t first = static_cast<uint16_t>(begin[0]);

    if (first < 0xD800) {
      ch = first;
      return begin + 1;
    } else if (first < 0xDC00) {
      if (end - begin < 2) {
        ch = uint32_t('?');
        return end;
      }

      ch = 0x10000 + ((first & 0x3FF) << 10) +
           (static_cast<uint16_t>(begin[1]) & 0x3FF);
      return begin + 2;
    } else if (first < 0xE000) {
      // Stray low surrogate
      ch = uint32_t('?');
      return begin + 1;
    } else {
      ch = first;
      return begin + 1;
    }
  } else {
    (void)end;
    ch = static_cast<uint32_t>(begin[0]);
    return begin + 1;
  }
}

/**
 * \brief Encodes a character
 *
 * Note that \c begin and \c end may be both be \c nullptr or equal, in
 * which case only the length of the encoded character will be returned.
 * \param [in] begin Pointer to current position within the output string
 * \param [in] end Pointer to the end of the output string
 * \param [in] ch Character to encode
 * \returns If begin is \c nullptr , the number of units required to encode
 *    the character. Otherwise, the number of units written to the output.
 *    This may return \c 0 for characters that cannot be written or encoded.
 */
template <typename T> size_t encodeChar(T *begin, T *end, uint32_t ch) {
  static_assert(sizeof(T) == 1 || sizeof(T) == 2 || sizeof(T) == 4,
                "Unsupported character size");

  if constexpr (sizeof(T) == 1) {
    if (ch < 0x80) {
      if (begin) {
        if (end - begin < 1)
          return 0;

        begin[0] = static_cast<T>(ch);
      }

      return 1;
    } else if (ch < 0x800) {
      if (begin) {
        if (end - begin < 2)
          return 0;

        begin[0] = static_cast<T>(0xC0 | (ch >> 6));
        begin[1] = static_cast<T>(0x80 | (ch & 0x3F));
      }

      return 2;
    } else if (ch < 0x10000) {
      if (begin) {
        if (end - begin < 3)
          return 0;

        begin[0] = static_cast<T>(0xE0 | (ch >> 12));
        begin[1] = static_cast<T>(0x80 | ((ch >> 6) & 0x3F));
        begin[2] = static_cast<T>(0x80 | (ch & 0x3F));
      }

      return 3;
    } else if (ch < 0x110000) {
      if (begin) {
        if (end - begin < 4)
          return 0;

        begin[0] = static_cast<T>(0xF0 | (ch >> 18));
        begin[1] = static_cast<T>(0x80 | ((ch >> 12) & 0x3F));
        begin[2] = static_cast<T>(0x80 | ((ch >> 6) & 0x3F));
        begin[3] = static_cast<T>(0x80 | (ch & 0x3F));
      }

      return 4;
    } else {
      // Invalid code point for UTF-8
      return 0;
    }
  } else if constexpr (sizeof(T) == 2) {
    if (ch < 0xD800) {
      if (begin) {
        if (end - begin < 1)
          return 0;

        begin[0] = static_cast<T>(ch);
      }

      return 1;
    } else if (ch < 0xE000) {
      // Surrogate code points,
      // we can't encode these
      return 0;
    } else if (ch < 0x10000) {
      if (begin) {
        if (end - begin < 1)
          return 0;

        begin[0] = static_cast<T>(ch);
      }

      return 1;
    } else if (ch < 0x110000) {
      if (begin) {
        if (end - begin < 2)
          return 0;

        ch -= 0x10000;
        begin[0] = static_cast<T>(0xD800 + (ch >> 10));
        begin[1] = static_cast<T>(0xDC00 + (ch & 0x3FF));
      }

      return 2;
    } else {
      // Invalid code point
      return 0;
    }
  } else {
    if (begin) {
      if (end - begin < 1)
        return 0;

      begin[0] = static_cast<T>(ch);
    }

    return 1;
  }
}

/**
 * \brief Computes length of a null-terminated string
 *
 * \param [in] begin Start of input string
 * \returns Number of characters in input string,
 *    excluding the terminating null character
 */
template <typename S> size_t length(const S *string) {
  size_t result = 0;

  while (string[result])
    result += 1;

  return result;
}

/**
 * \brief Converts string from one encoding to another
 *
 * The output string arguments may be \c nullptr. In that case, the
 * total length of the transcoded string will be returned, in units
 * of the output character type. The output string will only be
 * null-terminated if the input string is also null-terminated.
 * \tparam D Output character type
 * \tparam S Input character type
 * \param [in] dstBegin Start of output string
 * \param [in] dstLength Length of output string
 * \param [in] srcBegin Start of input string
 * \param [in] srcLength Length of input string
 * \returns If \c dstBegin is \c nullptr , the total number of output
 *    characters required to store the output string. Otherwise, the
 *    total number of characters written to the output string.
 */
template <typename D, typename S>
size_t transcodeString(D *dstBegin, size_t dstLength, const S *srcBegin,
                       size_t srcLength) {
  size_t totalLength = 0;

  auto dstEnd = dstBegin + dstLength;
  auto srcEnd = srcBegin + srcLength;

  while (srcBegin < srcEnd) {
    uint32_t ch;

    srcBegin = decodeChar<S>(srcBegin, srcEnd, ch);

    if (dstBegin)
      totalLength += encodeChar<D>(dstBegin + totalLength, dstEnd, ch);
    else
      totalLength += encodeChar<D>(nullptr, nullptr, ch);

    if (!ch)
      break;
  }

  return totalLength;
}

/**
 * \brief Creates string object from wide char array
 *
 * \param [in] ws Null-terminated wide string
 * \returns Regular string object
 */
std::string fromws(const WCHAR *ws);

/**
 * \brief Creates wide string object from char array
 *
 * \param [in] mbs Null-terminated string
 * \returns Wide string object
 */
std::wstring tows(const char *mbs);

#ifdef _WIN32
using path_string = std::wstring;
inline path_string topath(const char *mbs) { return tows(mbs); }
#else
using path_string = std::string;
inline path_string topath(const char *mbs) { return std::string(mbs); }
#endif

inline void format1(std::stringstream &) {}

template <typename... Tx>
void format1(std::stringstream &str, const WCHAR *arg, const Tx &...args) {
  str << fromws(arg);
  format1(str, args...);
}

template <typename T, typename... Tx>
void format1(std::stringstream &str, const T &arg, const Tx &...args) {
  // a byte is a number (a sample count, a mask), where a stream would write it as a character
  if constexpr (std::is_same_v<T, uint8_t>)
    str << unsigned(arg);
  else
    str << arg;
  format1(str, args...);
}

template <typename... Args> std::string format(const Args &...args) {
  std::stringstream stream;
  format1(stream, args...);
  return std::move(stream).str();
}

inline void strlcpy(char *dst, const char *src, size_t count) {
  if (count > 0) {
    std::strncpy(dst, src, count - 1);
    dst[count - 1] = '\0';
  }
}

/**
 * \brief Split string at one or more delimiters characters
 *
 * \param [in] string String to split
 * \param [in] delims Delimiter characters
 * \returns Vector of substring views
 */
inline std::vector<std::string_view> split(std::string_view string,
                                           std::string_view delims = " ") {
  std::vector<std::string_view> tokens;

  for (size_t start = 0; start < string.size();) {
    // Find first delimiter
    const auto end = string.find_first_of(delims, start);

    // Add non-empty tokens
    if (start != end)
      tokens.emplace_back(string.substr(start, end - start));

    // Break at the end of string
    if (end == std::string_view::npos)
      break;

    start = end + 1;
  }
  return tokens;
}

} // namespace mullion::str
