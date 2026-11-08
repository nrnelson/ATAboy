#ifndef _TUSB_CONFIG_H_
#define _TUSB_CONFIG_H_

#define CFG_TUSB_RHPORT0_MODE    OPT_MODE_DEVICE
#define CFG_TUD_CDC              1
#define CFG_TUD_MSC              1
// MSC buffer = sectors per ATA command on the USB-disk path.  Measured
// imaging a CHS drive over full-speed USB: 4 KiB 464 kB/s, 8 KiB 425 kB/s,
// 16 KiB 373 kB/s.  Bigger buffers serialize larger drive reads and USB
// transfers, so 4 KiB stays.  It also caps API replies (READ = 7 sectors).
#define CFG_TUD_MSC_EP_BUFSIZE   4096
#define CFG_TUD_CDC_RX_BUFSIZE   256
#define CFG_TUD_CDC_TX_BUFSIZE   512

#endif
