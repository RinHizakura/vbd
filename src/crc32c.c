// SPDX-License-Identifier: GPL-2.0-or-later
/* CRC-32C (Castagnoli), reflected, table driven. Same polynomial as ext4/btrfs. */
#include "crc32c.h"

static uint32_t table[256];

static void init_table(void)
{
	for (uint32_t i = 0; i < 256; i++) {
		uint32_t c = i;

		for (int k = 0; k < 8; k++)
			c = (c & 1) ? (c >> 1) ^ 0x82F63B78u : c >> 1;
		table[i] = c;
	}
}

/* ponytail: byte-at-a-time; switch to SSE4.2 _mm_crc32 if it shows in profiles */
uint32_t crc32c(uint32_t crc, const void *buf, size_t len)
{
	const uint8_t *p = buf;

	if (!table[1])
		init_table();
	crc = ~crc;
	while (len--)
		crc = table[(crc ^ *p++) & 0xff] ^ (crc >> 8);
	return ~crc;
}
