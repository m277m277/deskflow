/*
 * Deskflow -- mouse and keyboard sharing utility
 * SPDX-FileCopyrightText: (C) 2012 - 2016 Symless Ltd.
 * SPDX-FileCopyrightText: (C) 2002 Chris Schoeneman
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */

#include "arch/ArchString.h"

#include <climits>
#include <cstdlib>
#include <cstring>
#include <mutex>

std::mutex s_mutex;

//
// use C library non-reentrant multibyte conversion with mutex
//

int ArchString::convStringWCToMB(char *dst, const wchar_t *src, uint32_t n, bool *errors) const
{
  std::scoped_lock lock{s_mutex};
  ptrdiff_t len = 0;

  bool dummyErrors;
  if (errors == nullptr) {
    errors = &dummyErrors;
  }
  *errors = false;

  if (dst == nullptr) {
    char dummy[MB_LEN_MAX];
    const wchar_t *scan = src;
    for (; n > 0; --n) {
      ptrdiff_t mblen = wctomb(dummy, *scan);
      if (mblen == -1) {
        *errors = true;
        mblen = 1;
      }
      len += mblen;
      ++scan;
    }
    if (ptrdiff_t mblen = wctomb(dummy, L'\0'); mblen != -1) {
      len += mblen - 1;
    }
  } else {
    const char *dst0 = dst;
    const wchar_t *scan = src;
    for (; n > 0; --n) {
      if (ptrdiff_t mblen = wctomb(dst, *scan); mblen == -1) {
        *errors = true;
        *dst++ = '?';
      } else {
        dst += mblen;
      }
      ++scan;
    }
    if (ptrdiff_t mblen = wctomb(dst, L'\0'); mblen != -1) {
      // don't include nul terminator
      dst += mblen - 1;
    }
    len = dst - dst0;
  }

  return static_cast<int>(len);
}

ArchString::EWideCharEncoding ArchString::getWideCharEncoding() const
{
#ifdef SYSAPI_WIN32
  return EWideCharEncoding::kUTF16;
#else
  return EWideCharEncoding::kUCS4;
#endif
}

int ArchString::convStringMBToWC(wchar_t *dst, const char *src, uint32_t n, bool *errors) const
{
  std::scoped_lock lock{s_mutex};
  ptrdiff_t len = 0;
  wchar_t dummy;

  bool dummyErrors;
  if (errors == nullptr) {
    errors = &dummyErrors;
  }
  *errors = false;

  if (dst == nullptr) {
    const char *scan = src;
    while (n > 0) {
      switch (ptrdiff_t mblen = mbtowc(&dummy, scan, n); mblen) {
      case -2:
        // incomplete last character.  convert to unknown character.
        *errors = true;
        len += 1;
        n = 0;
        break;

      case -1:
        // invalid character.  count one unknown character and
        // start at the next byte.
        *errors = true;
        len += 1;
        scan += 1;
        n -= 1;
        break;

      case 0:
        len += 1;
        scan += 1;
        n -= 1;
        break;

      default:
        // normal character
        len += 1;
        scan += static_cast<int>(mblen);
        n -= static_cast<int>(mblen);
        break;
      }
    }
  } else {
    const wchar_t *dst0 = dst;
    const char *scan = src;
    while (n > 0) {
      switch (ptrdiff_t mblen = mbtowc(dst, scan, n); mblen) {
      case -2:
        // incomplete character.  convert to unknown character.
        *errors = true;
        *dst = (wchar_t)0xfffd;
        n = 0;
        break;

      case -1:
        // invalid character.  count one unknown character and
        // start at the next byte.
        *errors = true;
        *dst = (wchar_t)0xfffd;
        scan += 1;
        n -= 1;
        break;

      case 0:
        *dst = (wchar_t)0x0000;
        scan += 1;
        n -= 1;
        break;

      default:
        // normal character
        scan += static_cast<int>(mblen);
        n -= static_cast<int>(mblen);
        break;
      }
      ++dst;
    }
    len = dst - dst0;
  }

  return static_cast<int>(len);
}
