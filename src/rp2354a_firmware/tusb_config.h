/**
 * @file tusb_config.h
 * @brief TinyUSB device stack configuration.
 * CFG_TUSB_MCU is injected by the Pico SDK build system — do not define it here.
 *
 * @author Leo Walker
 * @copyright (c) Leo Walker 2025 @ MIT License @cite MIT License
 */

#ifndef _TUSB_CONFIG_H_
#define _TUSB_CONFIG_H_

#ifndef CFG_TUSB_MCU
#error "CFG_TUSB_MCU must be defined by the build system (Pico SDK sets this automatically)."
#endif

#define CFG_TUSB_OS OPT_OS_PICO

// RP2350 uses the same Full Speed USB hardware as RP2040.
#define CFG_TUSB_RHPORT0_MODE (OPT_MODE_DEVICE | OPT_MODE_FULL_SPEED)

// ---- Device stack ----------------------------------------------------------

#define CFG_TUD_ENABLED 1

// Disable all unused device classes
#define CFG_TUD_CDC        0
#define CFG_TUD_MSC        0
#define CFG_TUD_HID        0
#define CFG_TUD_MIDI       0
#define CFG_TUD_AUDIO      0
#define CFG_TUD_VIDEO      0
#define CFG_TUD_ECM_RNDIS  0 // Renamed from CFG_TUD_NET in newer TinyUSB
#define CFG_TUD_NCM        0
#define CFG_TUD_BTH        0

// One vendor interface for CAN over USB
#define CFG_TUD_VENDOR 1

// Bulk endpoint FIFO sizes — enough to buffer several max-size CAN FD frames (76 bytes each)
#define CFG_TUD_VENDOR_RX_BUFSIZE 512
#define CFG_TUD_VENDOR_TX_BUFSIZE 512

#endif // _TUSB_CONFIG_H_
