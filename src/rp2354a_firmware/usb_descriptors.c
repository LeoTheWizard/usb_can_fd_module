/**
 * @file usb_descriptors.c
 * @brief TinyUSB descriptor callbacks — device, configuration, and string descriptors.
 *
 * @author Leo Walker
 * @copyright (c) Leo Walker 2025 @ MIT License @cite MIT License
 */

#include <string.h>
#include <tusb.h>
#include <pico/unique_id.h>
#include <usb_can_protocol.h>

// ---- Device descriptor -----------------------------------------------------

static const tusb_desc_device_t device_desc = {
    .bLength = sizeof(tusb_desc_device_t),
    .bDescriptorType = TUSB_DESC_DEVICE,
    .bcdUSB = 0x0200,
    .bDeviceClass = 0xFF, // Vendor-specific
    .bDeviceSubClass = 0x00,
    .bDeviceProtocol = 0x00,
    .bMaxPacketSize0 = CFG_TUD_ENDPOINT0_SIZE,
    .idVendor = USB_CAN_VID,
    .idProduct = USB_CAN_PID,
    .bcdDevice = 0x0100,
    .iManufacturer = 0x01,
    .iProduct = 0x02,
    .iSerialNumber = 0x03,
    .bNumConfigurations = 0x01,
};

uint8_t const *tud_descriptor_device_cb(void)
{
    return (uint8_t const *)&device_desc;
}

// ---- Configuration descriptor ----------------------------------------------

enum
{
    ITF_CAN = 0,
    ITF_COUNT,
};

#define EP_CAN_OUT 0x01
#define EP_CAN_IN 0x81
#define EP_BULK_SIZE 64

#define CONFIG_TOTAL_LEN (TUD_CONFIG_DESC_LEN + TUD_VENDOR_DESC_LEN)

static const uint8_t config_desc[] = {
    TUD_CONFIG_DESCRIPTOR(1, ITF_COUNT, 0, CONFIG_TOTAL_LEN, 0x00, 100),
    TUD_VENDOR_DESCRIPTOR(ITF_CAN, 4, EP_CAN_OUT, EP_CAN_IN, EP_BULK_SIZE),
};

uint8_t const *tud_descriptor_configuration_cb(uint8_t index)
{
    (void)index;
    return config_desc;
}

// ---- String descriptors ----------------------------------------------------

enum
{
    STRID_LANGID = 0,
    STRID_MANUFACTURER,
    STRID_PRODUCT,
    STRID_SERIAL,
    STRID_INTERFACE,
};

static const char *string_descs[] = {
    [STRID_LANGID] = (const char[]){0x09, 0x04}, // English (US)
    [STRID_MANUFACTURER] = "Leo Walker",
    [STRID_PRODUCT] = "USB CAN-FD Module",
    [STRID_SERIAL] = NULL, // Built at runtime from chip unique ID
    [STRID_INTERFACE] = "CAN FD",
};

static uint16_t desc_str_buf[32];

uint16_t const *tud_descriptor_string_cb(uint8_t index, uint16_t langid)
{
    (void)langid;
    uint8_t chr_count;

    if (index == STRID_LANGID)
    {
        memcpy(&desc_str_buf[1], string_descs[STRID_LANGID], 2);
        chr_count = 1;
    }
    else if (index == STRID_SERIAL)
    {
        pico_unique_board_id_t uid;
        pico_get_unique_board_id(&uid);
        chr_count = 2 * PICO_UNIQUE_BOARD_ID_SIZE_BYTES;
        for (uint8_t i = 0; i < PICO_UNIQUE_BOARD_ID_SIZE_BYTES; i++)
        {
            desc_str_buf[1 + i * 2 + 0] = "0123456789ABCDEF"[(uid.id[i] >> 4) & 0xF];
            desc_str_buf[1 + i * 2 + 1] = "0123456789ABCDEF"[(uid.id[i]) & 0xF];
        }
    }
    else
    {
        if (index >= (sizeof(string_descs) / sizeof(string_descs[0])))
            return NULL;

        const char *str = string_descs[index];
        chr_count = (uint8_t)strlen(str);
        if (chr_count > 31)
            chr_count = 31;

        for (uint8_t i = 0; i < chr_count; i++)
            desc_str_buf[1 + i] = str[i];
    }

    desc_str_buf[0] = (uint16_t)((TUSB_DESC_STRING << 8) | (2 * chr_count + 2));
    return desc_str_buf;
}
