#ifndef API_SERIAL_H
#define API_SERIAL_H

#include <stdbool.h>

// The ATAboy API on the second USB serial port (CDC instance 1, interface
// string "ATAboy API"); framing in apiframe.h, docs/API.md "API serial port".
// Both functions run on core 0 only.

// Poll from the core 0 main loop, after tud_task(): reads requests, runs
// them through api_dispatch() and sends the responses.
void api_serial_task(void);

// True when no response is waiting to be sent.
bool api_serial_idle(void);

#endif
