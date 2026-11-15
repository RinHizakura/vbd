// SPDX-License-Identifier: GPL-2.0-or-later
#include <errno.h>
#include <stddef.h>
#include <stdlib.h>
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
	sb->parent_at = NO_PARENT;
	return 0;
}

bool super_has_base(const struct super *sb)
{
	return sb->base[0] && sb->parent_at == NO_PARENT;
}

int super_set_parent(struct super *sb, const char *parent_log, uint64_t snapshot)
{
	char *abs = realpath(parent_log, NULL);
	int ret = 0;

	if (!abs)
		return -errno;
	if (strlen(abs) >= sizeof(sb->base))
		ret = -ENAMETOOLONG;
	else
		strcpy(sb->base, abs);
	free(abs);
	sb->parent_at = snapshot;
	return ret;
}

const char *super_parent(const struct super *sb)
{
	return sb->parent_at == NO_PARENT ? NULL : sb->base;
}

int super_set_base(struct super *sb, const char *base)
{
	return base_ops_for(base)->probe(base, sb->base, sizeof(sb->base),
					 &sb->base_size, sb->base_tag,
					 sizeof(sb->base_tag));
}

int super_check_base(const struct super *sb)
{
	char tag[BASE_TAG_LEN], stored[sizeof(sb->base)];
	uint64_t size;
	int ret = base_ops_for(sb->base)->probe(sb->base, stored, sizeof(stored),
						&size, tag, sizeof(tag));

	if (ret)
		return ret;
	return size == sb->base_size && !strcmp(tag, sb->base_tag) ? 0 : -ESTALE;
}

bool super_has_manifest(const struct super *sb)
{
	static const uint8_t none[SHA256_LEN];

	return memcmp(sb->manifest_hash, none, SHA256_LEN) != 0;
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
	       !sb->base[sizeof(sb->base) - 1] &&
	       (sb->parent_at == NO_PARENT || sb->base[0]);	/* a fork names its parent */
}
