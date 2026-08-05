#ifndef WIFI_BRIDGE_H
#define WIFI_BRIDGE_H
#include <stdbool.h>
int  wifi_bridge_init(void);
int  wifi_bridge_start_sta(const char *ssid, const char *pass);
int  wifi_bridge_start_softap(const char *ssid, const char *pass);
bool wifi_bridge_is_connected(void);
int  wifi_bridge_rssi(void);
void wifi_bridge_get_ip(char *buf, int len);
#endif
