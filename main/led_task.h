#ifndef __LED_TASK_H
#define __LED_TASK_H

#ifdef __cpluscplus
extern "C" {
#endif

typedef enum _t_led_action
{
  LED_OFF = 0,
  LED_BLINKING_SLOWLY,
  LED_BLINKING_ANGRY,
  LED_BLINK_ONCE,
  LED_ON
} t_led_action;

//initialize LED task on gpio with active state on_state 
void* led_init(const unsigned int gpio, const int on_state);
//push action into LED task and let it repeat repeats times (use -1 for infinitely repeating action)
void led_push_action(void *handle, t_led_action led_action, int repeats);
//deinitializes LED task thet leads to its termination in next cycle
void led_deinit(void *handle);

#ifdef __cpluscplus
}
#endif

#endif
