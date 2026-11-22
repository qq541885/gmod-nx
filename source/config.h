/* config.h -- global configuration and config file handling
 *
 * This software may be modified and distributed under the terms
 * of the MIT license.  See the LICENSE file for details.
 */

#ifndef __CONFIG_H__
#define __CONFIG_H__

// memory reserved for loading the Source modules (sum of the LOAD segments of
// the 32 preloaded ARM64 .so files, with headroom for page alignment).
// everything else becomes the newlib heap, which the game allocates from
// since its malloc/free imports resolve to newlib's.
#define SO_MEMORY_MB 192

// where the game data lives on the SD card. must contain garrysmod/,
// sourceengine/ (or hl2/), platform/ and lib/ (the APK's arm64-v8a libraries).
#define DEFAULT_INSTALL_ROOT "/switch/gmodnx"

#define CONFIG_NAME "config.txt"
#define LOG_NAME "debug.log"

// per-line SD card writes cost boot time; set to 1 when investigating a crash
#define DEBUG_LOG 1
extern int screen_width;
extern int screen_height;

typedef struct {
  int screen_width;
  int screen_height;
  char install_root[256];
  char gamedir[64];
  char args[256];
  char lang[32];
  int gamepad;
  int touch_hud;
  int console;
  int gyro_enabled;
  int gyro_sensitivity_x;
  int gyro_sensitivity_y;
  int gyro_invert_x;
  int gyro_invert_y;
  int gyro_space;     /* 0 local, 1 world, 2 player (see gyro.h) */
  int gyro_tighten;   /* anti-drift taper near zero */
  int gyro_accel;     /* speed-based acceleration multiplier */
  char skip_ctors[256]; /* diag: "module.so:N" constructor indices to skip */
} Config;

extern Config config;

int read_config(const char *file);
int write_config(const char *file);

#endif
