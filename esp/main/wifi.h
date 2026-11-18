#ifndef WIFI_H
#define WIFI_H

#include <stddef.h>

void wifi_configure(const char *ssid, const char *pass,
		    const char *web_pass, int web_enabled);

/* Bring WiFi up for the boot menu and tear it down before the PC starts. */
void wifi_menu_start(void);
void wifi_menu_stop(void);

/* One-line state for the boot menu: AP/IP/password hint. */
void wifi_status_line(char *buf, size_t len);

#endif /* WIFI_H */
