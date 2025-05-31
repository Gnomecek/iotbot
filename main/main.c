#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <esp_system.h>
#include <esp_log.h>
#include <esp_event.h>
#include <nvs_flash.h>
#include <esp_netif.h>

#include <driver/gpio.h>

#include "discordbot.h"

#include "led_task.h"

#include "wifi_provisioning.h"

static const char *TAG = "discord_bot_main";

/***************************************************** */
/** MAIN */
void app_main(void)
{
  esp_err_t ret;
  void *lh;

  ESP_LOGI(TAG, "App main initializing..");

  ESP_ERROR_CHECK(nvs_flash_init());
  ESP_ERROR_CHECK((esp_netif_init()));
  ESP_ERROR_CHECK(esp_event_loop_create_default());

  lh=led_init(GPIO_NUM_10,0);
  ESP_LOGI(TAG, "led_init returns %p", lh);

  led_push_action(lh,LED_BLINKING_ANGRY,-1);

  ret=wifi_provision();

  if(ret == ESP_OK )
  {
    dib_start();
    led_push_action(lh,LED_BLINKING_SLOWLY,-1);
  }
  else{
    led_push_action(lh,LED_BLINKING_ANGRY,-1);
  }

}
