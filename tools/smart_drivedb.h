/*
 * smartmontools drive database entry for the ATAboy USB-to-IDE bridge.
 *
 * Lets smartctl/smartd find the drive behind an ATAboy without "-d sat":
 *     sudo cp tools/smart_drivedb.h /etc/smart_drivedb.h      (or append to it)
 *     sudo smartctl -a /dev/sdX
 * smartctl reads /etc/smart_drivedb.h in addition to its built-in database.
 * Needs the automation-API firmware (stock firmware has no SAT pass-through).
 *
 * 0xcafe:0x4005 is the ATAboy's USB ID (TinyUSB's example vendor ID, so this
 * entry is for local use rather than the upstream smartmontools database).
 */
{ "USB: ATAboy USB-IDE bridge; ",
  "0xcafe:0x4005",
  "", // any bcdDevice
  "",
  "-d sat"
},
