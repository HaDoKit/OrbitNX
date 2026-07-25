#ifndef _LOGGER_H
#define _LOGGER_H

// File-based logger for the Switch client - mirrors the host's Logger.cs so the two logs can be
// compared side by side after a session. Written to the SD card rather than relying on nxlink's
// stdout redirect (nxlinkStdio, see network_init) because that only reaches anything if nxlink's
// listener happens to still be connected at the moment a line is printed - a real file survives
// the app being run standalone (off a forwarder/hbmenu, not through nxlink) and can be pulled off
// the SD card afterward regardless of how the session actually ended, crash included.
void orbitnx_log_init(void);

// printf-style; timestamps are seconds elapsed since orbitnx_log_init() was called, not wall clock -
// this device doesn't reliably have accurate RTC/network time without extra setup, and elapsed
// time is enough to correlate events within one session's log.
void orbitnx_log(const char *category, const char *fmt, ...);

void orbitnx_log_close(void);

#endif
