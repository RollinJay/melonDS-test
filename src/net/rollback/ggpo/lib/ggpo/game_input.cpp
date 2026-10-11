/* -----------------------------------------------------------------------
 * GGPO.net (http://ggpo.net)  -  Copyright 2009 GroundStorm Studios, LLC.
 *
 * Use of this software is governed by the MIT license that can be found
 * in the LICENSE file.
 */

#include "types.h"
#include "game_input.h"
#include "log.h"

void
GameInput::init(int iframe, char *ibits, int isize, int offset)
{
   ASSERT(isize);
   ASSERT(isize <= GAMEINPUT_MAX_BYTES);
   frame = iframe;
   size = isize;
   memset(bits, 0, sizeof(bits));
   if (ibits) {
      memcpy(bits + (offset * isize), ibits, isize);
   }
}

void
GameInput::init(int iframe, char *ibits, int isize)
{
   ASSERT(isize);
   ASSERT(isize <= GAMEINPUT_MAX_BYTES * GAMEINPUT_MAX_PLAYERS);
   frame = iframe;
   size = isize;
   memset(bits, 0, sizeof(bits));
   if (ibits) {
      memcpy(bits, ibits, isize);
   }
}

// melonDS: portable string functions; the text is cut at buf_size
void
GameInput::desc(char *buf, size_t buf_size, bool show_frame) const
{
   ASSERT(size);
   if (buf_size == 0) {
      return;
   }
   size_t used;
   if (show_frame) {
      snprintf(buf, buf_size, "(frame:%d size:%d ", frame, size);
   } else {
      snprintf(buf, buf_size, "(size:%d ", size);
   }
   used = strlen(buf);

   for (int i = 0; i < size * 8 && used + 1 < buf_size; i++) {
      if (value(i)) {
         snprintf(buf + used, buf_size - used, "%2d ", i);
         used += strlen(buf + used);
      }
   }
   if (used + 1 < buf_size) {
      buf[used++] = ')';
      buf[used] = '\0';
   }
}

void
GameInput::log(char *prefix, bool show_frame) const
{
   if (!LogEnabled()) {
      return;
   }
   char buf[1024];
   snprintf(buf, ARRAY_SIZE(buf), "%s", prefix);
   size_t c = strlen(buf);
   desc(buf + c, ARRAY_SIZE(buf) - c, show_frame);
   Log("%s\n", buf);
}

bool
GameInput::equal(GameInput &other, bool bitsonly)
{
   if (!bitsonly && frame != other.frame) {
      Log("frames don't match: %d, %d\n", frame, other.frame);
   }
   if (size != other.size) {
      Log("sizes don't match: %d, %d\n", size, other.size);
   }
   if (memcmp(bits, other.bits, size)) {
      Log("bits don't match\n");
   }
   ASSERT(size && other.size);
   return (bitsonly || frame == other.frame) &&
          size == other.size &&
          memcmp(bits, other.bits, size) == 0;
}
