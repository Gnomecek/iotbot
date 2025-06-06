/* Wi-Fi Provisioning Manager Example

   This example code is in the Public Domain (or CC0 licensed, at your option.)

   Unless required by applicable law or agreed to in writing, this
   software is distributed on an "AS IS" BASIS, WITHOUT WARRANTIES OR
   CONDITIONS OF ANY KIND, either express or implied.
*/

#include <stdio.h>
#include <string.h>

#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <freertos/event_groups.h>

#include <esp_log.h>
#include <esp_wifi.h>
#include <esp_event.h>
#include <nvs_flash.h>
//#include <esp_http_server.h>

#include <wifi_provisioning/manager.h>

#include <wifi_provisioning/scheme_softap.h>

#include "wifi_provisioning.h"

static const char *TAG = "wifi_provisioning";

/* Signal Wi-Fi events on this event-group */
const int WIFI_CONNECTED_EVENT = BIT0;
static EventGroupHandle_t wifi_event_group;

/* Event handler for catching system events */
static void event_handler(void *arg, esp_event_base_t event_base,
                          int32_t event_id, void *event_data)
{
  if (event_base == WIFI_PROV_EVENT)
  {
    switch (event_id)
    {
    case WIFI_PROV_START:
      ESP_LOGI(TAG, "Provisioning started");
      break;
    case WIFI_PROV_CRED_RECV:
    {
      wifi_sta_config_t *wifi_sta_cfg = (wifi_sta_config_t *)event_data;
      ESP_LOGI(TAG, "Received Wi-Fi credentials"
                    "\n\tSSID     : %s\n\tPassword : %s",
               (const char *)wifi_sta_cfg->ssid,
               (const char *)wifi_sta_cfg->password);
      break;
    }
    case WIFI_PROV_CRED_FAIL:
    {
      wifi_prov_sta_fail_reason_t *reason = (wifi_prov_sta_fail_reason_t *)event_data;
      ESP_LOGE(TAG, "Provisioning failed!\n\tReason : %s"
                    "\n\tPlease reset to factory and retry provisioning",
               (*reason == WIFI_PROV_STA_AUTH_ERROR) ? "Wi-Fi station authentication failed" : "Wi-Fi access-point not found");

      /* Reset the state machine on provisioning failure.
       * This is enabled by the CONFIG_EXAMPLE_RESET_PROV_MGR_ON_FAILURE configuration.
       * It allows the provisioning manager to retry the provisioning process
       * based on the number of attempts specified in wifi_conn_attempts. After attempting
       * the maximum number of retries, the provisioning manager will reset the state machine
       * and the provisioning process will be terminated.
       */
      wifi_prov_mgr_reset_sm_state_on_failure();
      break;
    }
    case WIFI_PROV_CRED_SUCCESS:
      ESP_LOGI(TAG, "Provisioning successful");
      break;
    case WIFI_PROV_END:
      /* De-initialize manager once provisioning is finished */
      wifi_prov_mgr_deinit();
      break;
    default:
      break;
    }
  }
  else if (event_base == WIFI_EVENT)
  {
    switch (event_id)
    {
    case WIFI_EVENT_STA_START:
      esp_wifi_connect();
      break;
    case WIFI_EVENT_STA_DISCONNECTED:
      ESP_LOGI(TAG, "Disconnected. Connecting to the AP again...");
      esp_wifi_connect();
      break;
    case WIFI_EVENT_AP_STACONNECTED:
      ESP_LOGI(TAG, "SoftAP transport: Connected!");
      break;
    case WIFI_EVENT_AP_STADISCONNECTED:
      ESP_LOGI(TAG, "SoftAP transport: Disconnected!");
      break;
    default:
      break;
    }
  }
  else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP)
  {
    ip_event_got_ip_t *event = (ip_event_got_ip_t *)event_data;
    ESP_LOGI(TAG, "Connected with IP Address:" IPSTR, IP2STR(&event->ip_info.ip));
    /* Signal main application to continue execution */
    xEventGroupSetBits(wifi_event_group, WIFI_CONNECTED_EVENT);
  }
  else if (event_base == PROTOCOMM_SECURITY_SESSION_EVENT)
  {
    switch (event_id)
    {
    case PROTOCOMM_SECURITY_SESSION_SETUP_OK:
      ESP_LOGI(TAG, "Secured session established!");
      break;
    case PROTOCOMM_SECURITY_SESSION_INVALID_SECURITY_PARAMS:
      ESP_LOGE(TAG, "Received invalid security parameters for establishing secure session!");
      break;
    case PROTOCOMM_SECURITY_SESSION_CREDENTIALS_MISMATCH:
      ESP_LOGE(TAG, "Received incorrect username and/or PoP for establishing secure session!");
      break;
    default:
      break;
    }
  }
}

static void wifi_init_sta(void)
{
  /* Start Wi-Fi in station mode */
  ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
  ESP_ERROR_CHECK(esp_wifi_start());
}

static void get_device_service_name(char *service_name, size_t max)
{
  uint8_t eth_mac[6];
  const char *ssid_prefix = "DBOT_";
  esp_wifi_get_mac(WIFI_IF_STA, eth_mac);
  snprintf(service_name, max, "%s%02X%02X%02X",
           ssid_prefix, eth_mac[3], eth_mac[4], eth_mac[5]);
}

/** 
main wifi provisioning function
initializes and starts wifi provisioning / connection to wifi
*/
esp_err_t wifi_provision(void)
{
  const char *step;
  /* Initialize NVS partition */
  step="Initialize NVS partition";
  esp_err_t ret = nvs_flash_init();
  if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND)
  {
    /* NVS partition was truncated
     * and needs to be erased */
    ESP_ERROR_CHECK(nvs_flash_erase());

    /* Retry nvs_flash_init */
    ESP_ERROR_CHECK(nvs_flash_init());
  }

  /* Initialize TCP/IP */
  step="Initialize TCP/IP";
  ret=esp_netif_init();
  if (ret != ESP_OK) goto FNRET;

  /* Initialize the event loop */
  step="Initialize the event loop";
  ret=esp_event_loop_create_default();
  if (ret != ESP_OK && ret != ESP_ERR_INVALID_STATE) goto FNRET;

  wifi_event_group = xEventGroupCreate();

  /* Register our event handler for Wi-Fi, IP and Provisioning events */
  step="Register WIFI_PROV_EVENT handler";
  ret=esp_event_handler_register(WIFI_PROV_EVENT, ESP_EVENT_ANY_ID, &event_handler, NULL);
  if (ret != ESP_OK) goto FNRET;

  step="Register PROTOCOMM_SECURITY_SESSION_EVENT handler";
  ret=esp_event_handler_register(PROTOCOMM_SECURITY_SESSION_EVENT, ESP_EVENT_ANY_ID, &event_handler, NULL);
  if (ret != ESP_OK) goto FNRET;

  step="Register IP_EVENT handler";
  ret=esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &event_handler, NULL);
  if (ret != ESP_OK) goto FNRET;

  /* Initialize Wi-Fi including netif with default config */
  esp_netif_create_default_wifi_sta();
  esp_netif_create_default_wifi_ap();

  step="esp_wifi_init";
  wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
  ret=esp_wifi_init(&cfg);
  if (ret != ESP_OK) goto FNRET;

  /* Configuration for the provisioning manager */
  wifi_prov_mgr_config_t config = {
#ifdef CONFIG_EXAMPLE_RESET_PROV_MGR_ON_FAILURE
      .wifi_prov_conn_cfg = {
          .wifi_conn_attempts = CONFIG_EXAMPLE_PROV_MGR_CONNECTION_CNT,
      },
#endif
      /* What is the Provisioning Scheme that we want ?
       * wifi_prov_scheme_softap or wifi_prov_scheme_ble -> wifi_prov_scheme_softap*/
      .scheme = wifi_prov_scheme_softap,

      /* WIFI_PROV_EVENT_HANDLER_NONE when using wifi_prov_scheme_softap*/
      .scheme_event_handler = WIFI_PROV_EVENT_HANDLER_NONE
  };

  /* Initialize provisioning manager with the
   * configuration parameters set above */
  step="Initialize provisioning manager";
  ret=wifi_prov_mgr_init(config);
  if (ret != ESP_OK) goto FNRET;

  bool provisioned = false;

  /* Let's find out if the device is provisioned */
  step="Checking initialization status";
  ret=wifi_prov_mgr_is_provisioned(&provisioned);
  if (ret != ESP_OK) goto FNRET;

  /* If device is not yet provisioned start provisioning service */
  if (!provisioned)
  {
    ESP_LOGI(TAG, "Starting provisioning");

    /* What is the Device Service Name that we want
     * This translates to :
     *     - Wi-Fi SSID when scheme is wifi_prov_scheme_softap
     *     - device name when scheme is wifi_prov_scheme_ble
     */
    char service_name[12];
    get_device_service_name(service_name, sizeof(service_name));

    /* What is the security level that we want (0, 1, 2):
     *      - WIFI_PROV_SECURITY_0 is simply plain text communication.
     */
    wifi_prov_security_t security = WIFI_PROV_SECURITY_0;

    /* What is the service key (could be NULL)
     * This translates to :
     *     - Wi-Fi password when scheme is wifi_prov_scheme_softap
     *          (Minimum expected length: 8, maximum 64 for WPA2-PSK)
     */
    const char *service_key = "password";

    /* Start provisioning service */
    ESP_LOGI(TAG, "starting provisioning...");
    step="Start provisioning service";
    ret=wifi_prov_mgr_start_provisioning(security, NULL, service_name, service_key);
    if (ret != ESP_OK) goto FNRET;
  }
  else
  {
    ESP_LOGI(TAG, "Already provisioned, starting Wi-Fi STA");

    /* We don't need the manager as device is already provisioned,
     * so let's release it's resources */
    wifi_prov_mgr_deinit();
    
    step="Register WIFI_EVENT handler";
    ret=esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &event_handler, NULL);
    if (ret != ESP_OK) goto FNRET;
    /* Start Wi-Fi station */
    wifi_init_sta();
  }

  ESP_LOGI(TAG, "Waiting for connection...");

  /* Wait for Wi-Fi connection */
  xEventGroupWaitBits(wifi_event_group, WIFI_CONNECTED_EVENT, true, true, portMAX_DELAY);

FNRET:
  if (ret != ESP_OK)
  {
    ESP_LOGE(TAG, "Error in wifi_provisioning, step %s, err=0x%x", step, ret);
  }
  return ret;
}
