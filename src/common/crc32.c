/**
 * @file crc32.c
 * @brief CRC-32 (IEEE 802.3) implementation using a 256-entry lookup table.
 *
 * @author Leo Walker
 * @copyright (c) Leo Walker 2025 @ MIT License @cite MIT License
 */

#include "crc32.h"

/* Reflected polynomial for CRC-32 IEEE 802.3. */
#define CRC32_POLY 0xEDB88320U

static uint32_t crc32_table[256];
static int table_ready = 0;

static void build_table(void)
{
    for (uint32_t i = 0; i < 256; i++)
    {
        uint32_t crc = i;
        for (int bit = 0; bit < 8; bit++)
            crc = (crc & 1) ? ((crc >> 1) ^ CRC32_POLY) : (crc >> 1);
        crc32_table[i] = crc;
    }
    table_ready = 1;
}

uint32_t crc32_compute(const uint8_t *data, size_t length)
{
    if (!table_ready)
        build_table();

    uint32_t crc = 0xFFFFFFFFU;
    for (size_t i = 0; i < length; i++)
        crc = (crc >> 8) ^ crc32_table[(crc ^ data[i]) & 0xFF];

    return crc ^ 0xFFFFFFFFU;
}
