#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <sys/time.h>

#include "esp_system.h"
#include "esp_log.h"
#include "esp_task.h"

#include "driver/gpio.h"

#include "led_task.h"

static const char *TAG = "led_task";

//max number of actions in buffer
#ifndef LED_ACTIONS_MAX
#define LED_ACTIONS_MAX 8
#endif

#define LED_ON_TIME 100
#define LED_OFF_TIME_ANGRY (LED_ON_TIME)
#define LED_OFF_TIME_SLOW 1900
#define LED_OFF_TIME_ONCE 400 

//increment list index, wrap around LED_ACTIONS_MAX
#define INC_LED_LIST_IDX(idx) \
  if (++(idx)>=LED_ACTIONS_MAX) idx=0

//check pointer validity
#define IS_LED_INVALID(led) \
  ((led) == NULL || \
  (led)->size!=sizeof(t_led_state) || \
  (unsigned int)((led)->list_head)>=LED_ACTIONS_MAX || \
  (unsigned int)((led)->list_tail)>=LED_ACTIONS_MAX || \
  (unsigned int)((led)->list_len)>LED_ACTIONS_MAX)

#define IS_LED_VALID(led) (!IS_LED_INVALID(led))

// typedef enum _t_led_action
// {
//   LED_OFF = 0,
//   LED_BLINKING_SLOWLY,
//   LED_BLINKING_ANGRY,
//   LED_BLINK_ONCE,
//   LED_ON
// } t_led_action;

typedef struct _t_led_action_cell
{
  t_led_action action; //current action
  int repeats; //number of repeats, -1 means forever or until new action is pushed
} t_led_action_cell;

typedef struct _t_led_running
{
  int idx; //active index, <0 when inactive
  int phase; //phase of active 
  uint64_t next_change; //when there will be next change needed
} t_led_running;

typedef struct _t_led_state
{
  size_t size; //size of structure, it is (weak) handle integrity check
  unsigned int gpio; //led gpio
  int on_state; //led light on level

  t_led_action_cell list[LED_ACTIONS_MAX]; //list of actions
  int list_head; //first empty
  int list_tail; //first to run
  int list_len; //number of items in list
  int list_changed; //whether list has changed

  t_led_running running; //running state
  
} t_led_state;

static portMUX_TYPE spinlock = portMUX_INITIALIZER_UNLOCKED;

static void led_task(void *handle);

//initialize LED task on gpio with active state on_state 
void* led_init(const unsigned int gpio, const int on_state)
{
  esp_err_t r;

  gpio_config_t io_conf = {
      .intr_type = GPIO_INTR_DISABLE,
      .mode = GPIO_MODE_OUTPUT,
      .pin_bit_mask = BIT64(gpio),
      .pull_down_en = 0,
      .pull_up_en = 0
    };


  ESP_LOGI(TAG, "Initializing..");

  r = gpio_config(&io_conf);
  if(r!=ESP_OK)
  {
    ESP_LOGE(TAG, "Error 0x%x initializing GPIO %d",r,gpio);
    return NULL;
  } 

  r=gpio_set_level(gpio, !on_state);
  if(r!=ESP_OK)
  {
    ESP_LOGE(TAG, "Error 0x%x setting GPIO %d level",r, gpio);
  }

  t_led_state *state=(t_led_state*)calloc(1,sizeof(t_led_state));
  if(state == NULL)
  {
    ESP_LOGE(TAG, "Error getting memory for led state");
    return NULL;
  } 

  state->gpio=gpio;
  state->on_state=on_state;
  state->running.idx=-1;
  state->size=sizeof(t_led_state);

  TaskHandle_t xHandle = NULL;
  xTaskCreate( led_task, "led_task", 4096, state, 3, &xHandle );

  ESP_LOGI(TAG, "Task handle is %p", xHandle);

  return state;
}


/*
+---+---+
| 0 | 1 |
+---+---+
  ^   ^tail
  head
*/

//push action into LED task and let it repeat repeats times (use -1 for infinitely repeating action)
void led_push_action(void *handle, t_led_action led_action, int repeats)
{
  t_led_state *led=(t_led_state *)handle; //make it easier to write references..
  taskENTER_CRITICAL(&spinlock);

  //integrity check
  if(IS_LED_VALID(led)) {
    if(led->list_len<LED_ACTIONS_MAX)
    {
      //list has space for new action, just add it
      led->list[led->list_tail].action=led_action;
      led->list[led->list_tail].repeats=repeats;
      led->list_len+=1;
      INC_LED_LIST_IDX(led->list_tail);
    }
    else
    {
      //no more space, so move whole list, set new at the tail and finish active action
      INC_LED_LIST_IDX(led->list_tail);
      INC_LED_LIST_IDX(led->list_head);
      led->list[led->list_tail].action=led_action;
      led->list[led->list_tail].repeats=repeats;
      led->running.idx=-1;
    }
    led->list_changed=1;
  }

  taskEXIT_CRITICAL(&spinlock);
}

static void led_update_action(t_led_state *led, uint64_t t);

//helper, inits action starting at led->list_head
//always called form critical section protected code
static void led_init_action(t_led_state *led, uint64_t t)
{
  if(led == NULL || led->list_len<=0) return; //nothing to do

  //skip all -1 repeats until last or finite
  while(led->list_len>1 && led->list[led->list_head].repeats<0)
  {
    led->list_len--;
    INC_LED_LIST_IDX(led->list_head);
  }

  led->running.idx=led->list_head;
  led->running.phase=0;
  led_update_action(led,t);

}

//helper, inits action starting at led->list_head
//always called form critical section protected code
static void led_update_action(t_led_state *led, uint64_t t)
{
  uint64_t next_change;
  int led_on,list_changed, manage_repeats;
  int phase;
  int update_led=0, led_state;

  if(led == NULL || led->running.idx<0) return; //nothing to do
  
  next_change=(uint64_t)-1;
  led_on=0;
  manage_repeats=0;
  list_changed=0;
  phase=0;

  switch (led->list[led->running.idx].action)
  {
  case LED_OFF:
    break;
  case LED_ON:
    led_on=1;
    break;
  case LED_BLINKING_SLOWLY:
    if(led->running.phase)
    {
      next_change=t+LED_OFF_TIME_SLOW;
      manage_repeats=1;
    }else
    {
      led_on=1;
      next_change=t+LED_ON_TIME;
      phase=1;  
    }
    break;
  case LED_BLINKING_ANGRY:
    if(led->running.phase)
    {
      next_change=t+LED_OFF_TIME_ANGRY;
      manage_repeats=1;
    }else
    {
      led_on=1;
      next_change=t+LED_ON_TIME;
      phase=1;  
    }
    break;
  case LED_BLINK_ONCE:
    if(led->running.phase)
    {
      next_change=t+LED_OFF_TIME_ONCE;
      manage_repeats=1;
    }else
    {
      led_on=1;
      next_change=t+LED_ON_TIME;
      phase=1;  
    }
    break;

  default:
    //seems we have an unsupported action in the list, just skip it and let next action to process on next run
    if(led->list_len>0)
    {
      list_changed=1;
      INC_LED_LIST_IDX(led->list_head);
      led->list_len-=1;
    }
    break;
  }

  if(manage_repeats)
  {
    if(led->list[led->running.idx].repeats>=0 && (--led->list[led->running.idx].repeats)<0)
    {
      list_changed=1;
    }
  }

  led->list_changed=list_changed;
  if(list_changed)
  {
    led->running.idx=-1;
  }
  else
  {
    led->running.next_change=next_change;
    led->running.phase=phase;
    update_led=1;
    led_state=!led->on_state ^ led_on;
  }

  if(update_led)
  {
    gpio_set_level(led->gpio, led_state);
  }
}

//main task, maintains action queue and performs desired actions
//ends when handle becomes invalid
static void led_task(void *handle)
{
  t_led_state *led=(t_led_state *)handle; //make it easier to write references..

  //integrity check
  int valid=IS_LED_VALID(led);
   
  const TickType_t ten_ms = pdMS_TO_TICKS(10);
  uint64_t t=0;

  ESP_LOGI(TAG, "Ticks to 10ms: 0x%lx",ten_ms);

  while(valid)
  {
    taskENTER_CRITICAL(&spinlock);

    valid=IS_LED_VALID(led);

    if(valid) {
      if(led->list_changed)
      {
        led_init_action(led,t);
      }
      else if(led->running.idx<0)
      {
        ; //we have nothing to do
      }
      else if(led->running.next_change<=t) //is it right time?
      {
        led_update_action(led,t);
      }
    }
    taskEXIT_CRITICAL(&spinlock);
    vTaskDelay(ten_ms);
    t+=10;
  }
  ESP_LOGE(TAG, "Task deleted!");
  vTaskDelete(NULL);
}

//deinitializes LED task thet leads to its termination in next cycle
void led_deinit(void *handle)
{
  taskENTER_CRITICAL(&spinlock);
  if(IS_LED_VALID((t_led_state *)handle))
  {
    memset(handle, 0, sizeof(t_led_state));
    free(handle);
  }
  taskEXIT_CRITICAL(&spinlock);
}
