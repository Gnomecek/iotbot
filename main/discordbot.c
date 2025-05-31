#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/projdefs.h"
#include "esp_system.h"
#include "esp_log.h"
#include "esp_task.h"
#include "esp_event.h"
#include "nvs_flash.h"

#include "driver/gpio.h"

#include "discord.h"
#include "discord/session.h"
#include "discord/message.h"
#include "estr.h"

#include "discordbot.h"

static const char *TAG = "discord_bot";

static discord_handle_t bot;

#define RELAY_GPIO GPIO_NUM_20

#ifdef CONFIG_DISCORD_CHANNEL_ID
static char cached_channel_id[256] = CONFIG_DISCORD_CHANNEL_ID;
#else
static char cached_channel_id[256] = "";
#endif

static int connected = 0;

//tries to send realy state do discord channel
static void send_relay_state(char *channel_id)
{
  if(!connected) return; //cannot send messages

  if (channel_id)
  {
    // copy channel id for future use
    ESP_LOGI(TAG, "Going to store channel_id=%s",channel_id);
    cached_channel_id[0] = 0;
    strncat(cached_channel_id, channel_id, sizeof(cached_channel_id) / sizeof(cached_channel_id[0]) - 1);
  }
  if (cached_channel_id[0])
  {
    // we know channel_id
    ESP_LOGI(TAG, "Going to send message to channel_id=%s",cached_channel_id);

    char *content = estr_cat("Door is ", gpio_get_level(RELAY_GPIO) ? "OPEN " DISCORD_EMOJI_X : "closed " DISCORD_EMOJI_WHITE_CHECK_MARK);

    discord_message_t msg = {.content = content, .channel_id = cached_channel_id};

    discord_message_t *sent_msg = NULL;
    esp_err_t err = discord_message_send(bot, &msg, &sent_msg);
    free(content);

    if (err == ESP_OK)
    {
      ESP_LOGI(TAG, "Relay status message successfully sent");

      if (sent_msg)
      { // null check because message can be sent but not returned
        ESP_LOGI(TAG, "Relay status message got ID #%s", sent_msg->id?sent_msg->id:"UNKNOWN");
        discord_message_free(sent_msg);
      }
    }
    else
    {
      ESP_LOGE(TAG, "Fail to send Relay status message");
    }
  }
}

//handles discord bot events
static void bot_event_handler(void *handler_arg, esp_event_base_t base, int32_t event_id, void *event_data)
{
  discord_event_data_t *data = (discord_event_data_t *)event_data;

  switch (event_id)
  {
  case DISCORD_EVENT_CONNECTED:
  {
    discord_session_t *session = (discord_session_t *)data->ptr;
    connected=1;

    ESP_LOGI(TAG, "Bot %s#%s connected", session->user->username, session->user->discriminator);

    send_relay_state(NULL);

  }
  break;

  case DISCORD_EVENT_MESSAGE_RECEIVED:
  {
    discord_message_t *msg = (discord_message_t *)data->ptr;

    ESP_LOGI(TAG,
             "New message (dm=%s, author=%s#%s, bot=%s, channel=%s, guild=%s, content=%s)",
             !msg->guild_id ? "true" : "false",
             msg->author->username,
             msg->author->discriminator,
             msg->author->bot ? "true" : "false",
             msg->channel_id,
             msg->guild_id ? msg->guild_id : "NULL",
             msg->content);

    if (msg->content && msg->content[0])
    {

      char *echo_content = estr_cat("Hey ", msg->author->username, " you wrote `", msg->content, "`");

      discord_message_t echo = {.content = echo_content, .channel_id = msg->channel_id};

      discord_message_t *sent_msg = NULL;
      esp_err_t err = discord_message_send(bot, &echo, &sent_msg);
      free(echo_content);

      if (err == ESP_OK)
      {
        ESP_LOGI(TAG, "Echo message successfully sent");

        if (sent_msg)
        { // null check because message can be sent but not returned
          ESP_LOGI(TAG, "Echo message got ID #%s", sent_msg->id);
          discord_message_free(sent_msg);
        }
      }
      else
      {
        ESP_LOGE(TAG, "Fail to send echo message");
      }

      send_relay_state(msg->channel_id);
    }
  }
  break;

  case DISCORD_EVENT_MESSAGE_UPDATED:
  {
    discord_message_t *msg = (discord_message_t *)data->ptr;
    ESP_LOGI(TAG,
             "%s has updated his message (#%s). New content: %s",
             msg->author->username,
             msg->id,
             msg->content);
  }
  break;

  case DISCORD_EVENT_MESSAGE_DELETED:
  {
    discord_message_t *msg = (discord_message_t *)data->ptr;
    ESP_LOGI(TAG, "Message #%s deleted", msg->id);
  }
  break;

  case DISCORD_EVENT_DISCONNECTED:
    connected=0;
    ESP_LOGW(TAG, "Bot logged out");
    break;
  }
}


/***************************************************** */
/** RELAY CODE */

// relay state change function
static void relay_state_changed(int)
{
  send_relay_state(NULL);
}

// ISR that handles relay state change
static void IRAM_ATTR relay_isr_handler(void *arg)
{
  TaskHandle_t xTaskToNotify = (TaskHandle_t)arg;
  BaseType_t xHigherPriorityTaskWoken = pdFALSE;
  if (xTaskToNotify)
  {
    vTaskNotifyGiveFromISR(xTaskToNotify, &xHigherPriorityTaskWoken);
    portYIELD_FROM_ISR(xHigherPriorityTaskWoken);
  }
}

// MUST be called from task that monitors relay!
esp_err_t configure_relay(gpio_num_t gpio_num)
{
  esp_err_t r;
  TaskHandle_t xTaskToNotify = xTaskGetCurrentTaskHandle();

  gpio_config_t io_conf = {
      .intr_type = GPIO_INTR_ANYEDGE,
      .mode = GPIO_MODE_INPUT,
      .pin_bit_mask = BIT64(gpio_num),
      .pull_down_en = 0,
      .pull_up_en = 1};

  //configure pin
  r = gpio_config(&io_conf);
  if (r) goto FNRET;

  // hook isr handler for specific gpio pin
  r = gpio_isr_handler_add(gpio_num, relay_isr_handler, (void *)xTaskToNotify);
  if (r) goto FNRET;

FNRET:
  ESP_LOGI("configure_relay", "Initialization return code=%d", r);
  return r;
}

// accepts gpio_num_t* which holds pin GPIO with relay being connected
// it expects, that gpio_install_isr_service has already been called
static void relay_monitoring_task(void *arg)
{
  esp_err_t r = ESP_OK - 1;
  gpio_num_t gpio_num = 0;
  int relay_state;

  if (arg)
  {
    gpio_num = *((gpio_num_t *)arg);
    r = configure_relay(gpio_num);
  }
  if (r != ESP_OK)
  {
    // we have nothing to do :/
    vTaskSuspend(NULL);
  }

  relay_state = !gpio_get_level(gpio_num);

  while (2 + 3 * 4 == 14)
  {
    if (relay_state != gpio_get_level(gpio_num))
    {
      relay_state = gpio_get_level(gpio_num);
      ESP_LOGI("relay_monitoring_task", "Relay state changed to %d!", relay_state);
      //send new state to Discord
      relay_state_changed(relay_state);
      //ignore short state changes
      vTaskDelay(pdMS_TO_TICKS(500));
    }
    //wait for relay state change
    ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
    ESP_LOGI("relay_monitoring_task", "Notification received!");
  }
}


/***************************************************** */


esp_err_t dib_start()
{
  gpio_num_t gpio_relay_num = RELAY_GPIO;
  BaseType_t t;
  esp_err_t r = ESP_OK - 1;

  // install gpio isr service
  gpio_install_isr_service(0);

  // start gpio task
  t = xTaskCreate(relay_monitoring_task, "relay_monitoring_task", 4096, &gpio_relay_num, 5, NULL);
  ESP_LOGI(TAG, "Monitoring task creation return code=%d", t);
  if (t!=pdPASS) goto FNRET;
  
  // discord_config_t cfg = { .intents = DISCORD_INTENT_GUILD_MESSAGES | DISCORD_INTENT_MESSAGE_CONTENT };
  discord_config_t cfg = {.intents = DISCORD_INTENT_GUILD_MESSAGES};

  bot = discord_create(&cfg);
  if (bot==NULL) goto FNRET;

  r=discord_register_events(bot, DISCORD_EVENT_ANY, bot_event_handler, NULL);
  if (r) goto FNRET;

  r=discord_login(bot);
  if (r) goto FNRET;

  FNRET:
  ESP_LOGI("dib_start", "Initialization return code=0X%x", r);
  return r;
}
