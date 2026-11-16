// SETTINGS popup (gear icon in the header): OSC output on/off, target IP and
// port, gyro-over-OSC on/off. Saved to sdmc:/3ds/osc3ds/eucseq.ini.
#pragma once
#include <3ds.h>
#include <stdbool.h>

void settings_load(void);           // after net_config_load(); applies gyro sensor state
bool settings_osc_on(void);         // OSC output enabled (off by default)
bool settings_gyro_on(void);        // send /gyro + /accel (needs OSC on)

bool settings_is_open(void);
void settings_open(void);
void settings_close(void);
void settings_touch_down(int x, int y);     // may open the system keyboard: call outside a frame
void settings_draw(void);                   // last, over everything on the bottom screen
void settings_tick(void);                   // once per frame: sends gyro when enabled
