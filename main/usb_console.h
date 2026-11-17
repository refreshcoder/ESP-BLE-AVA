#pragma once

/*
 * Command console on the native USB-Serial/JTAG port (the one esptool/monitor already use).
 *
 * Why: on a board that is only reachable through USB, the HTTP API over the SoftAP is out of
 * reach (joining BeaconGW-xxxx kills the host's own network). This gives the same read and
 * control surface over the cable, line-oriented, one JSON object per line.
 *
 * Output protocol: command results are printed as JSON lines, then a line `#END`.
 * A failure prints `#ERR <reason>` before `#END`. Anything else on the port is normal
 * ESP_LOG output and can be ignored.
 */
void usb_console_init(void);
