/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef VBD_UBLK_H
#define VBD_UBLK_H

#include <stdbool.h>

#include "vbd.h"

/*
 * Expose @v as /dev/ublkbN, print that path on stdout and serve it until
 * SIGINT or SIGTERM, then take the device down again. Needs the ublk_drv
 * module and /dev/ublk-control, which normally means root. @readonly makes
 * the block device read-only, for a volume opened at a snapshot.
 * Returns 0 or a negative errno.
 */
int ublk_serve(struct vbd *v, bool readonly);

#endif
