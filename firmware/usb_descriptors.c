#include "tusb.h"
#include "pico/unique_id.h"
#include <string.h>

#define USB_VID   0xCafe
#define USB_PID   0x4005
#define USB_BCD   0x0200

// --- Device Descriptor ---
tusb_desc_device_t const desc_device = {
    .bLength            = sizeof(tusb_desc_device_t),
    .bDescriptorType    = TUSB_DESC_DEVICE,
    .bcdUSB             = USB_BCD,
    .bDeviceClass       = TUSB_CLASS_MISC,
    .bDeviceSubClass    = MISC_SUBCLASS_COMMON,
    .bDeviceProtocol    = MISC_PROTOCOL_IAD,
    .bMaxPacketSize0    = CFG_TUD_ENDPOINT0_SIZE,
    .idVendor           = USB_VID,
    .idProduct          = USB_PID,
    .bcdDevice          = 0x0101,   // 0x0101: API serial port added; hosts must not reuse a cached layout
    .iManufacturer      = 0x01,
    .iProduct           = 0x02,
    .iSerialNumber      = 0x03,
    .bNumConfigurations = 0x01
};

uint8_t const *tud_descriptor_device_cb(void) {
    return (uint8_t const *)&desc_device;
}

// --- Configuration Descriptor ---
// Interfaces 0-2 are as they always were (setup-UI serial port, then the
// disk), so hosts and tools that bind to interface 0 or 2 keep working.  The
// API serial port comes after them as interfaces 3-4 with its own IAD.
// TinyUSB numbers CDC instances in descriptor order: 0 = setup UI, 1 = API.
enum {
    ITF_NUM_CDC = 0,
    ITF_NUM_CDC_DATA,
    ITF_NUM_MSC,
    ITF_NUM_API,
    ITF_NUM_API_DATA,
    ITF_NUM_TOTAL
};

#define CONFIG_TOTAL_LEN (TUD_CONFIG_DESC_LEN + TUD_CDC_DESC_LEN + TUD_MSC_DESC_LEN + TUD_CDC_DESC_LEN)

#define EPNUM_CDC_NOTIF   0x81
#define EPNUM_CDC_OUT     0x02
#define EPNUM_CDC_IN      0x82
#define EPNUM_MSC_OUT     0x03
#define EPNUM_MSC_IN      0x83
#define EPNUM_API_NOTIF   0x84
#define EPNUM_API_OUT     0x05
#define EPNUM_API_IN      0x85

uint8_t const desc_configuration[] = {
    TUD_CONFIG_DESCRIPTOR(1, ITF_NUM_TOTAL, 0, CONFIG_TOTAL_LEN, 0x00, 100),
    TUD_CDC_DESCRIPTOR(ITF_NUM_CDC, 4, EPNUM_CDC_NOTIF, 8, EPNUM_CDC_OUT, EPNUM_CDC_IN, 64),
    TUD_MSC_DESCRIPTOR(ITF_NUM_MSC, 5, EPNUM_MSC_OUT, EPNUM_MSC_IN, 64),
    TUD_CDC_DESCRIPTOR(ITF_NUM_API, 6, EPNUM_API_NOTIF, 8, EPNUM_API_OUT, EPNUM_API_IN, 64),
};

// A wrong total length or interface count breaks enumeration until BOOTSEL.
_Static_assert(sizeof(desc_configuration) == CONFIG_TOTAL_LEN, "configuration descriptor length");
_Static_assert(CONFIG_TOTAL_LEN == 9 + 66 + 23 + 66, "configuration descriptor layout");
_Static_assert(ITF_NUM_TOTAL == 5, "interface count");
_Static_assert(CFG_TUD_CDC == 2, "two CDC instances: setup UI and API");

uint8_t const *tud_descriptor_configuration_cb(uint8_t index) {
    (void)index;
    return desc_configuration;
}

// --- String Descriptors ---
char const *string_desc_arr[] = {
    (const char[]){0x09, 0x04},   // 0: English
    "Obsolete Tech",               // 1: Manufacturer
    "ATAboy",                      // 2: Product
    NULL,                          // 3: Serial: the chip's unique board id (below)
    "ATAboy CDC",                  // 4: CDC Interface
    "ATAboy Storage",              // 5: MSC Interface
    "ATAboy API",                  // 6: API serial port (host tools find it by this name)
};

static uint16_t _desc_str[32];

uint16_t const *tud_descriptor_string_cb(uint8_t index, uint16_t langid) {
    (void)langid;
    uint8_t chr_count;

    if (index == 0) {
        memcpy(&_desc_str[1], string_desc_arr[0], 2);
        chr_count = 1;
    } else {
        if (!(index < sizeof(string_desc_arr) / sizeof(string_desc_arr[0]))) return NULL;
        // Every unit used to report "654321", so a host could not tell two
        // ATAboys apart.  The RP2350's unique board id (16 hex digits) is the
        // same on every power-up and different on every unit.
        char serial[2 * PICO_UNIQUE_BOARD_ID_SIZE_BYTES + 1];
        const char *str = string_desc_arr[index];
        if (index == 3) {
            pico_get_unique_board_id_string(serial, sizeof(serial));
            str = serial;
        }
        chr_count = strlen(str);
        if (chr_count > 31) chr_count = 31;
        for (uint8_t i = 0; i < chr_count; i++) _desc_str[1 + i] = str[i];
    }

    _desc_str[0] = (TUSB_DESC_STRING << 8) | (2 * chr_count + 2);
    return _desc_str;
}
