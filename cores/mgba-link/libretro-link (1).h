/* Network link cable for the mGBA libretro core.
 *
 * Emulates the GB serial port and the GBA serial port (multiplayer and normal modes) between
 * frontends over libretro's netpacket interface (RETRO_ENVIRONMENT_SET_NETPACKET_INTERFACE).
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */
#ifndef LIBRETRO_LINK_H
#define LIBRETRO_LINK_H

#include "libretro.h"

struct mCore;

/* Private environment call: the core asks the frontend to run the next frame right away instead
 * of waiting for its regular frame time (a link partner is blocked waiting for us). */
#define RETRO_ENVIRONMENT_LINK_RUN_SOON (RETRO_ENVIRONMENT_PRIVATE | 0x4D4C)

void LinkSetEnvironment(retro_environment_t env);
void LinkAttach(struct mCore* core);
void LinkDetach(void);
void LinkFrameStart(void);
void LinkFrameEnd(void);
/* Call at the start of retro_run: true if this run only answered a link partner and no frame was
 * completed (present the previous frame again). */
bool LinkRunPartial(void);

#endif
