/*
 * osgfx.h - the software rasteriser handed to guests as api->gfx.
 *
 * One table, defined in osgfx.c, pointed at from gapi.c. Nothing in it touches
 * hardware: every routine writes into a gb_surf_t the caller owns, so there is
 * nothing to guard and nothing to reclaim. See the graphics section of
 * greenbox_abi.h for why it lives on this side of the table at all.
 */
#pragma once

#include "greenbox_abi.h"

extern const gb_gfx_t g_gb_gfx;
