/* -----------------------------------------------------------------------
 * GGPO.net (http://ggpo.net)  -  Copyright 2009 GroundStorm Studios, LLC.
 *
 * Use of this software is governed by the MIT license that can be found
 * in the LICENSE file.
 */

#ifndef _LOG_H
#define _LOG_H

#include <stdio.h>
#include <stdarg.h>

// melonDS: whether logging is on (read once); the Log functions of the
// classes check it before formatting anything.
extern bool LogEnabled();

extern void Log(const char *fmt, ...);
extern void Logv(const char *fmt, va_list list);
extern void Logv(FILE *fp, const char *fmt, va_list args);
extern void LogFlush();

#endif
