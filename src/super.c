// SPDX-License-Identifier: GPL-2.0-or-later
#include <errno.h>
#include <stddef.h>
#include <string.h>

#include "common.h"
#include "crc32c.h"
#include "super.h"

#define VBD_VERSION	1

/* A power-of-two chunk that a record's map entries fit in, and whole chunks only. */
static bool geometry_ok(uint64_t size, uint32_t chunk_size)
{
	return IS_POWER_OF_2(chunk_size) && chunk_size >= MIN_CHUNK_SIZE &&
	       size && IS_ALIGNED(size, chunk_size);
}

static uint32_t super_crc(const struct super *sb)
{
	return crc32c(0, &sb->magic, sizeof(*sb) - offsetof(struct super, magic));
}

int super_init(struct super *sb, uint64_t size, uint32_t chunk_size)
{
	if (!geometry_ok(size, chunk_size))
		return -EINVAL;
	memset(sb, 0, sizeof(*sb));
	sb->magic = VBD_MAGIC;
	sb->version = VBD_VERSION;
	sb->chunk_size = chunk_size;
	sb->size = size;
	return 0;
}

void super_seal(struct super *sb)
{
	sb->crc = super_crc(sb);
}

/* Everything create enforces, re-checked on open so a damaged super is refused. */
bool super_valid(const struct super *sb)
{
	return sb->magic == VBD_MAGIC && sb->version == VBD_VERSION &&
	       sb->crc == super_crc(sb) &&
	       geometry_ok(sb->size, sb->chunk_size) &&
	       !sb->base[sizeof(sb->base) - 1];
}
