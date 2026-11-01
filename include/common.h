/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Small arithmetic helpers shared across vbd. */
#ifndef VBD_COMMON_H
#define VBD_COMMON_H

#define IS_POWER_OF_2(x)	((x) != 0 && ((x) & ((x) - 1)) == 0)
/* @a must be a power of two */
#define IS_ALIGNED(x, a)	(((x) & ((a) - 1)) == 0)

#endif /* VBD_COMMON_H */
