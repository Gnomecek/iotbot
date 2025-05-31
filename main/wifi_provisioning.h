#ifndef __WIFI_PROVISIONING_H
#define __WIFI_PROVISIONING_H

#include <esp_err.h>

#ifdef __cplusplus
extern "C" {
#endif

//initializes and starts wifi provisioning / connection to wifi
esp_err_t wifi_provision(void);

#ifdef __cplusplus
}
#endif

#endif /* __WIFI_PROVISIONING_H */