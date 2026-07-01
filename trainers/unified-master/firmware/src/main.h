#ifndef MAIN_H
#define MAIN_H

#include <Arduino.h>
#include <Adafruit_NeoPixel.h>

extern bool fault_active[55];
extern bool sim_active[16];
extern int sim_step[16];
extern uint32_t sim_timer[16];
extern bool is_ap_mode;
extern bool last_w_state;
extern bool last_y_state;
extern bool last_g_state;
extern Adafruit_NeoPixel status_led;
extern bool ota_in_progress;
extern const char* TRAINER_TYPE;

#endif
