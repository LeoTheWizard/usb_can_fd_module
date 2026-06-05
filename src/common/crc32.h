/**
 * @file crc32.h
 * @brief CRC-32 (IEEE 802.3) checksum.
 *
 * Shared between the RP2350 firmware and host-side drivers/tools.
 * Used to validate usb_can_packet_t transfers over the bulk endpoints.
 *
 * @author Leo Walker
 * @copyright (c) Leo Walker 2025 @ MIT License @cite MIT License
 */

#ifndef _CRC32_H_
#define _CRC32_H_

#include <stdint.h>
#include <stddef.h>

/**
 * @brief Computes a CRC-32 (IEEE 802.3, poly 0xEDB88320) checksum.
 *
 * @param data   Pointer to the byte buffer to checksum.
 * @param length Number of bytes to process.
 * @return 32-bit CRC value.
 */
uint32_t crc32_compute(const uint8_t *data, size_t length);

#endif /* _CRC32_H_ */
