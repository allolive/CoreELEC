/*
 *  SPDX-License-Identifier: GPL-2.0-or-later
 *
 *  Kodi's logger pulls in spdlog, which is a built dependency and not in the
 *  source tree. A suite that compiles a production translation unit only to
 *  exercise its logic should not have to build a logging stack for it, so
 *  this stands in. It replaces the logger and nothing else: the code under
 *  test is the real thing.
 */

#pragma once

// The real header exposes fmt to everything that logs; callers use it.
#include <fmt/format.h>

constexpr int LOGDEBUG = 0;
constexpr int LOGINFO = 1;
constexpr int LOGWARNING = 2;
constexpr int LOGERROR = 3;
constexpr int LOGFATAL = 4;
constexpr int LOGNONE = 5;

// Component flags, the second argument of the two-argument Log().
constexpr int LOGSAMBA = 1 << 0;
constexpr int LOGCURL = 1 << 1;
constexpr int LOGFFMPEG = 1 << 2;
constexpr int LOGDBUS = 1 << 3;
constexpr int LOGJSONRPC = 1 << 4;
constexpr int LOGAUDIO = 1 << 5;
constexpr int LOGAIRTUNES = 1 << 6;
constexpr int LOGUPNP = 1 << 7;
constexpr int LOGCEC = 1 << 8;
constexpr int LOGVIDEO = 1 << 9;
constexpr int LOGWEBSERVER = 1 << 10;
constexpr int LOGDATABASE = 1 << 11;
constexpr int LOGAVTIMING = 1 << 12;
constexpr int LOGWINDOWING = 1 << 13;
constexpr int LOGPVR = 1 << 14;
constexpr int LOGEPG = 1 << 15;
constexpr int LOGANNOUNCE = 1 << 16;

class CLog
{
public:
  template<typename... Args>
  static void Log(int, const char*, const Args&...)
  {
  }

  // The component overload: Log(level, component, format, ...).
  template<typename... Args>
  static void Log(int, int, const char*, const Args&...)
  {
  }

  template<typename... Args>
  static void LogF(int, const char*, const Args&...)
  {
  }

  template<typename... Args>
  static void LogF(int, int, const char*, const Args&...)
  {
  }
};
