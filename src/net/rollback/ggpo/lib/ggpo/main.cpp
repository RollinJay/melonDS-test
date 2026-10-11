/* -----------------------------------------------------------------------
 * GGPO.net (http://ggpo.net)  -  Copyright 2009 GroundStorm Studios, LLC.
 *
 * Use of this software is governed by the MIT license that can be found
 * in the LICENSE file.
 */

#include "types.h"
#include "backends/p2p.h"
#include "ggponet.h"

/*
 * melonDS: GGPO is built into the program (no DllMain), and every API function
 * catches what the library throws: a failed assertion (GGPOAssertion), or an
 * exception from one of the application's callbacks. The call then returns
 * GGPO_ERRORCODE_GENERAL_FAILURE, after which the session must be closed.
 * The synctest and spectator backends are not included.
 */
#define GGPO_TRY        try {
#define GGPO_CATCH      } catch (const std::exception &e) {             \
                           Log("exception: %s\n", e.what());            \
                           return GGPO_ERRORCODE_GENERAL_FAILURE;       \
                        } catch (...) {                                 \
                           return GGPO_ERRORCODE_GENERAL_FAILURE;       \
                        }

void
ggpo_log(GGPOSession *ggpo, const char *fmt, ...)
{
   va_list args;
   va_start(args, fmt);
   ggpo_logv(ggpo, fmt, args);
   va_end(args);
}

void
ggpo_logv(GGPOSession *ggpo, const char *fmt, va_list args)
{
   if (ggpo) {
      ggpo->Logv(fmt, args);
   }
}

GGPOErrorCode
ggpo_start_session_ex(GGPOSession **session,
                      GGPOSessionCallbacks *cb,
                      const char *game,
                      int num_players,
                      int input_size,
                      const GGPOSessionOptions *options)
{
   if (!session || !cb || !options) {
      return GGPO_ERRORCODE_INVALID_REQUEST;
   }
   *session = NULL;
   if (num_players < 1 || num_players > GGPO_MAX_PLAYERS ||
       input_size < 1 || input_size > GAMEINPUT_MAX_BYTES) {
      return GGPO_ERRORCODE_INVALID_REQUEST;
   }
   if (options->transport && (!options->transport->send_to || !options->transport->recv_from)) {
      return GGPO_ERRORCODE_INVALID_REQUEST;
   }
   GGPO_TRY
      *session = (GGPOSession *)new Peer2PeerBackend(cb,
                                                    game,
                                                    *options,
                                                    num_players,
                                                    input_size);
      return GGPO_OK;
   GGPO_CATCH
}

GGPOErrorCode
ggpo_start_session(GGPOSession **session,
                   GGPOSessionCallbacks *cb,
                   const char *game,
                   int num_players,
                   int input_size,
                   unsigned short localport)
{
   GGPOSessionOptions options;
   memset(&options, 0, sizeof options);
   options.size = sizeof options;
   options.local_port = localport;
   return ggpo_start_session_ex(session, cb, game, num_players, input_size, &options);
}

GGPOErrorCode
ggpo_add_player(GGPOSession *ggpo,
                GGPOPlayer *player,
                GGPOPlayerHandle *handle)
{
   if (!ggpo) {
      return GGPO_ERRORCODE_INVALID_SESSION;
   }
   GGPO_TRY
      return ggpo->AddPlayer(player, handle);
   GGPO_CATCH
}

GGPOErrorCode
ggpo_set_frame_delay(GGPOSession *ggpo,
                     GGPOPlayerHandle player,
                     int frame_delay)
{
   if (!ggpo) {
      return GGPO_ERRORCODE_INVALID_SESSION;
   }
   GGPO_TRY
      return ggpo->SetFrameDelay(player, frame_delay);
   GGPO_CATCH
}

GGPOErrorCode
ggpo_idle(GGPOSession *ggpo, int timeout)
{
   if (!ggpo) {
      return GGPO_ERRORCODE_INVALID_SESSION;
   }
   GGPO_TRY
      return ggpo->DoPoll(timeout);
   GGPO_CATCH
}

GGPOErrorCode
ggpo_add_local_input(GGPOSession *ggpo,
                     GGPOPlayerHandle player,
                     void *values,
                     int size)
{
   if (!ggpo) {
      return GGPO_ERRORCODE_INVALID_SESSION;
   }
   GGPO_TRY
      return ggpo->AddLocalInput(player, values, size);
   GGPO_CATCH
}

GGPOErrorCode
ggpo_synchronize_input(GGPOSession *ggpo,
                       void *values,
                       int size,
                       int *disconnect_flags)
{
   if (!ggpo) {
      return GGPO_ERRORCODE_INVALID_SESSION;
   }
   GGPO_TRY
      return ggpo->SyncInput(values, size, disconnect_flags);
   GGPO_CATCH
}

GGPOErrorCode ggpo_disconnect_player(GGPOSession *ggpo,
                                     GGPOPlayerHandle player)
{
   if (!ggpo) {
      return GGPO_ERRORCODE_INVALID_SESSION;
   }
   GGPO_TRY
      return ggpo->DisconnectPlayer(player);
   GGPO_CATCH
}

GGPOErrorCode
ggpo_advance_frame(GGPOSession *ggpo)
{
   if (!ggpo) {
      return GGPO_ERRORCODE_INVALID_SESSION;
   }
   GGPO_TRY
      return ggpo->IncrementFrame();
   GGPO_CATCH
}

GGPOErrorCode
ggpo_get_network_stats(GGPOSession *ggpo,
                       GGPOPlayerHandle player,
                       GGPONetworkStats *stats)
{
   if (!ggpo) {
      return GGPO_ERRORCODE_INVALID_SESSION;
   }
   GGPO_TRY
      return ggpo->GetNetworkStats(stats, player);
   GGPO_CATCH
}


GGPOErrorCode
ggpo_close_session(GGPOSession *ggpo)
{
   if (!ggpo) {
      return GGPO_ERRORCODE_INVALID_SESSION;
   }
   GGPO_TRY
      delete ggpo;
      return GGPO_OK;
   GGPO_CATCH
}

GGPOErrorCode
ggpo_set_disconnect_timeout(GGPOSession *ggpo, int timeout)
{
   if (!ggpo) {
      return GGPO_ERRORCODE_INVALID_SESSION;
   }
   GGPO_TRY
      return ggpo->SetDisconnectTimeout(timeout);
   GGPO_CATCH
}

GGPOErrorCode
ggpo_set_disconnect_notify_start(GGPOSession *ggpo, int timeout)
{
   if (!ggpo) {
      return GGPO_ERRORCODE_INVALID_SESSION;
   }
   GGPO_TRY
      return ggpo->SetDisconnectNotifyStart(timeout);
   GGPO_CATCH
}
