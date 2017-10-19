#include <sys/types.h>
#include <stdbool.h>
#include <stdint.h>

bool spice_connect(const char * host, const short port, const char * password);
void spice_disconnect();
bool spice_process();
bool spice_ready();

bool spice_key_down      (uint32_t code);
bool spice_key_up        (uint32_t code);
bool spice_mouse_mode    (bool     server);
bool spice_mouse_position(uint32_t x, uint32_t y);
bool spice_mouse_motion  ( int32_t x,  int32_t y);
bool spice_mouse_press   (uint32_t button);
bool spice_mouse_release (uint32_t button);