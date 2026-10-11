/* -----------------------------------------------------------------------
 * GGPO.net (http://ggpo.net)  -  Copyright 2009 GroundStorm Studios, LLC.
 *
 * Use of this software is governed by the MIT license that can be found
 * in the LICENSE file.
 */

#include <mutex>

#include "types.h"

/*
 * melonDS: logging is off unless the environment sets GGPO_LOG=1 (written to
 * ggpo-<pid>.log in the working directory); GGPO_LOG_TIMESTAMPS=1 adds times.
 * The settings are read once.
 */

static FILE *logfile = NULL;
static std::mutex logmutex;

bool LogEnabled()
{
   static const bool enabled = Platform::GetConfigBool("ggpo.log");
   return enabled;
}

static bool LogTimestamps()
{
   static const bool enabled = Platform::GetConfigBool("ggpo.log.timestamps");
   return enabled;
}

void LogFlush()
{
   std::lock_guard<std::mutex> lock(logmutex);
   if (logfile) {
      fflush(logfile);
   }
}

void Log(const char *fmt, ...)
{
   if (!LogEnabled()) {
      return;
   }
   va_list args;
   va_start(args, fmt);
   Logv(fmt, args);
   va_end(args);
}

void Logv(const char *fmt, va_list args)
{
   if (!LogEnabled()) {
      return;
   }
   std::lock_guard<std::mutex> lock(logmutex);
   if (!logfile) {
      char name[64];
      snprintf(name, sizeof(name), "ggpo-%d.log", Platform::GetProcessID());
      logfile = fopen(name, "w");
      if (!logfile) {
         return;
      }
   }
   Logv(logfile, fmt, args);
}

void Logv(FILE *fp, const char *fmt, va_list args)
{
   if (LogTimestamps()) {
      unsigned int t = Platform::GetCurrentTimeMS();
      fprintf(fp, "%u.%03u : ", t / 1000, t % 1000);
   }

   vfprintf(fp, fmt, args);
   fflush(fp);
}
