/* main.c -- Garry's Mod (GDroid ARM64 binaries) Nintendo Switch loader.
 *
 * Loads the 32 Source ARM64 modules shipped inside the GDroid APK
 * (tinsarfal/GDroid, engine based on nillerusr's Android Source port),
 * mirrors the Android environment and calls LauncherMainAndroid, which runs
 * the whole game.
 *
 * The loader architecture, so_util, libc shim and JNI fake are reused from
 * hl2_nx (MIT, (C) 2021 fgsfds / Andy Nguyen). The module set, data checks,
 * environment variables and launch vector are GMod-specific.
 *
 * Distributed under the MIT license. See LICENSE for details.
 */

#include "saltynx.h"
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <pthread.h>
#include <sys/stat.h>
#include <switch.h>

#include "config.h"
#include "util.h"
#include "error.h"
#include "so_util.h"
#include "imports.h"
#include "libc_shim.h"
#include "jni_fake.h"
#include "game_patches.h"
#include "controller.h"
#include "posix_shim.h"

static void *heap_so_base = NULL;
/* __libnx_initheap runs before newlib's heap exists: touching stdio there
 * (debugPrintf -> fopen -> malloc) crashes the process before anything can be
 * logged. Record the figures and print them from main() instead. */
static size_t g_diag_total, g_diag_avail, g_diag_used, g_diag_heap, g_diag_pool;
static size_t heap_so_limit = 0;

/* Load order matters twice:
 *  - pass 2 resolves every import against the union of all exports, so the
 *    order only has to be self-consistent;
 *  - pass 3 runs .init_array in this exact order, so a module must come
 *    after everything whose globals it touches during init.
 *
 * Deliberately NOT loaded:
 *   libSDL2.so             -- android SDL2; switch-sdl2 provides these symbols
 *   libshaderapivulkan.so  -- 91 MB Vulkan backend, needs a real libvulkan
 *   libzstd-jni-*.so       -- java-side zstd, nothing links against it
 * The renderer therefore goes launcher -> shaderapidx9 -> togl -> EGL/GLES,
 * i.e. switch-mesa, exactly like hl2_nx.
 */
static const char *const module_names[] = {
  "libc++_shared.so",
  "libtier0.so",
  "libvstdlib.so",
  "libsteam_api.so",
  "libtogl.so",
  "libphonon.so",
  "libfilesystem_stdio.so",
  "libinputsystem.so",
  "libdatacache.so",
  "libscenefilecache.so",
  "libsoundemittersystem.so",
  "libvideo_services.so",
  "libvaudio_minimp3.so",
  "libvaudio_opus.so",
  "libvaudio_speex.so",
  "libvphysics.so",
  "libstudiorender.so",
  "libmaterialsystem.so",
  "libshaderapidx9.so",
  "libstdshader_dx9.so",
  "libvtex_dll.so",
  "libvgui2.so",
  "libvguimatsurface.so",
  "libengine.so",
  "libGameUI.so",
  "libServerBrowser.so",
  "liblua_shared.so",
  "libclient.so",
  "libserver.so",
  "libopenxr_loader.so",
  "libvrxr.so",
  "liblauncher.so",
};
#define NUM_MODULES (sizeof(module_names) / sizeof(*module_names))

static so_module modules[NUM_MODULES];

/* split the process memory: the game's malloc resolves to newlib's, so the
 * newlib heap gets everything except the SO_MEMORY_MB pool for the .so modules */
void __libnx_initheap(void) {
  void *addr;
  size_t size = 0, fake_heap_size = 0;
  size_t mem_available = 0, mem_used = 0;

  if (envHasHeapOverride()) {
    addr = envGetHeapOverrideAddr();
    size = envGetHeapOverrideSize();
  } else {
    svcGetInfo(&mem_available, InfoType_TotalMemorySize, CUR_PROCESS_HANDLE, 0);
    svcGetInfo(&mem_used, InfoType_UsedMemorySize, CUR_PROCESS_HANDLE, 0);
    if (mem_available > mem_used + 0x200000)
      size = (mem_available - mem_used - 0x200000) & ~0x1FFFFF;
    if (size == 0)
      size = 0x2000000 * 16;
    Result rc = svcSetHeapSize(&addr, size);
    if (R_FAILED(rc))
      diagAbortWithResult(MAKERESULT(Module_Libnx, LibnxError_HeapAllocFailed));
  }

  const size_t so_size = (size_t)SO_MEMORY_MB * 1024 * 1024;

  extern char *fake_heap_start;
  extern char *fake_heap_end;
  fake_heap_size  = size > so_size ? size - so_size : size / 2;
  fake_heap_start = (char *)addr;
  fake_heap_end   = (char *)addr + fake_heap_size;

  heap_so_base = (char *)addr + fake_heap_size;
  heap_so_base = (void *)ALIGN_MEM((uintptr_t)heap_so_base, 0x1000);
  heap_so_limit = (char *)addr + size - (char *)heap_so_base;

  g_diag_total = size;
  g_diag_avail = mem_available;
  g_diag_used  = mem_used;
  g_diag_heap  = fake_heap_size;
  g_diag_pool  = so_size;
  /* No stdio here: this runs before newlib's heap exists. */
}

/* render at the real panel size so the engine's GL viewport fills switch-sdl2's
 * framebuffer; config screen_width/height override, -1 means auto by dock state */
static void set_screen_size(void) {
  if (config.screen_width > 0 && config.screen_height > 0) {
    screen_width = config.screen_width;
    screen_height = config.screen_height;
  } else if (appletGetOperationMode() == AppletOperationMode_Console) {
    screen_width = 1920;
    screen_height = 1080;
  } else {
    screen_width = 1280;
    screen_height = 720;
  }
}

static int is_regular_file(const char *path) {
  struct stat st;
  return stat(path, &st) == 0 && S_ISREG(st.st_mode);
}

static int is_dir(const char *path) {
  struct stat st;
  return stat(path, &st) == 0 && S_ISDIR(st.st_mode);
}

/* GDroid uses a plain garrysmod/ + sourceengine/ + platform/ layout, the same
 * three folders the desktop install has. PC bin/*.so files are never used. */
static void check_data(void) {
  const char *files[] = {
    "lib/liblauncher.so",
    "lib/libengine.so",
    "lib/libclient.so",
    "lib/libserver.so",
    "lib/liblua_shared.so",
    "lib/libtogl.so",
    "lib/libshaderapidx9.so",
    "garrysmod/gameinfo.txt",
  };
  struct stat st;
  const unsigned int numfiles = sizeof(files) / sizeof(*files);
  for (unsigned int i = 0; i < numfiles; ++i) {
    if (stat(files[i], &st) < 0)
      fatal_error("Could not find\n%s/%s\nCheck your data files.", config.install_root, files[i]);
  }

  if (!is_dir("garrysmod") || !is_dir("platform")) {
    fatal_error("Missing garrysmod/ or platform/ in\n%s", config.install_root);
  }

  /* sourceengine/ carries the shared HL2 VPKs on a GDroid install; accept the
   * classic hl2/ name as well so a hand-made layout still boots */
  if (!is_dir("sourceengine") && !is_dir("hl2")) {
    fatal_error("Missing sourceengine/ (or hl2/) in\n%s", config.install_root);
  }

  if (!is_regular_file("garrysmod/garrysmod_dir.vpk") &&
      !is_dir("garrysmod/lua")) {
    fatal_error("garrysmod/ has neither garrysmod_dir.vpk nor lua/.\n"
                "Copy the complete folder, not just the _dir.vpk.");
  }
}

static void load_modules(void) {
  char path[512];
  void *base = heap_so_base;
  size_t remaining = heap_so_limit;

  /* pass 1: load everything so cross-module resolution sees all exports */
  for (unsigned int i = 0; i < NUM_MODULES; i++) {
    snprintf(path, sizeof(path), "lib/%s", module_names[i]);
    debugPrintf("loading %s\n", path);
    const int res = so_load(&modules[i], path, base, remaining);
    if (res < 0)
      fatal_error("Could not load\n%s\n(so_load: %d)", path, res);
    base = (void *)ALIGN_MEM((uintptr_t)base + modules[i].load_size, 0x1000);
    remaining = heap_so_limit - ((uintptr_t)base - (uintptr_t)heap_so_base);
    debugPrintf("loaded %s (%zu bytes)\n", path, modules[i].load_size);
  }

  /* pass 2: relocate + resolve now that every export is visible */
  for (unsigned int i = 0; i < NUM_MODULES; i++) {
    debugPrintf("relocate %s\n", module_names[i]);
    so_relocate(&modules[i]);
    so_resolve(&modules[i], dynlib_functions, dynlib_numfunctions, 1);
  }
  debugPrintf("pass 2 done\n");

  apply_game_patches();
  debugPrintf("patches done\n");
  controller_prepare(so_find_module("libengine.so"), so_find_module("libinputsystem.so"));
  debugPrintf("controller prepared\n");

  /* pass 3: map as code, then run initializers in dependency order */
  for (unsigned int i = 0; i < NUM_MODULES; i++) {
    debugPrintf("finalize %s\n", module_names[i]);
    so_finalize(&modules[i]);
    so_flush_caches(&modules[i]);
  }
  debugPrintf("pass 3 done: all modules mapped RX\n");

  for (unsigned int i = 0; i < NUM_MODULES; i++) {
    debugPrintf("init_array %s (%d ctors)\n", module_names[i], modules[i].num_init);
    so_execute_init_array(&modules[i]);
    so_free_temp(&modules[i]);
  }
  debugPrintf("pass 4 done: all ctors ran\n");
  controller_install();
  debugPrintf("controller installed, load_modules returning\n");
}

static void append_arg(char *dst, size_t dst_size, const char *arg) {
  size_t len;
  size_t arg_len;

  if (!dst_size || !arg || !arg[0])
    return;

  len = strlen(dst);
  if (len >= dst_size - 1)
    return;

  if (len > 0) {
    dst[len++] = ' ';
    dst[len] = '\0';
  }

  arg_len = strlen(arg);
  if (arg_len >= dst_size - len)
    arg_len = dst_size - len - 1;
  memcpy(dst + len, arg, arg_len);
  dst[len + arg_len] = '\0';
}

static const char *language_env_name(const char *lang) {
  static const struct {
    const char *name;
    const char *env;
  } langs[] = {
    { "english", "en_US" },
    { "german", "de_DE" },
    { "french", "fr_FR" },
    { "italian", "it_IT" },
    { "spanish", "es_ES" },
    { "koreana", "ko_KR" },
    { "schinese", "zh_CN" },
    { "tchinese", "zh_TW" },
    { "russian", "ru_RU" },
    { "thai", "th_TH" },
    { "japanese", "ja_JP" },
    { "portuguese", "pt_PT" },
    { "polish", "pl_PL" },
    { "brazilian", "pt_BR" },
  };

  for (unsigned i = 0; i < sizeof(langs) / sizeof(*langs); i++) {
    if (!strcmp(lang, langs[i].name))
      return langs[i].env;
  }
  return "en_US";
}

/* GDroid's launcher exports no setArgs JNI entry (hl2's does), so the command
 * line has to arrive through argv. LauncherMainAndroid builds the engine
 * command line from it. */
static void setup_environment(void) {
  static char lib_path[384];
  snprintf(lib_path, sizeof(lib_path), "%s/lib", config.install_root);

  setenv("APP_DATA_PATH", config.install_root, 1);
  setenv("APP_LIB_PATH", lib_path, 1);
  setenv("VALVE_GAME_PATH", config.install_root, 1);
  setenv("LANG", language_env_name(config.lang), 1);
  setenv("HOME", config.install_root, 1);
  setenv("LIBGL_USEVBO", "0", 1);
  setenv("SDL_TOUCH_MOUSE_EVENTS", "0", 1);

  /* the engine picks togl unless told otherwise; make it explicit so a stray
   * default cannot drag in the Vulkan backend */
  setenv("MESA_GL_VERSION_OVERRIDE", "3.3", 1);
  setenv("MESA_GLSL_VERSION_OVERRIDE", "330", 1);
}

static int build_argv(char **argv, int max_args) {
  static char cmdline[768];
  char num[16];
  int argc = 0;

  cmdline[0] = '\0';
  append_arg(cmdline, sizeof(cmdline), "-game");
  append_arg(cmdline, sizeof(cmdline), config.gamedir);
  append_arg(cmdline, sizeof(cmdline), "-w");
  snprintf(num, sizeof(num), "%d", screen_width);
  append_arg(cmdline, sizeof(cmdline), num);
  append_arg(cmdline, sizeof(cmdline), "-h");
  snprintf(num, sizeof(num), "%d", screen_height);
  append_arg(cmdline, sizeof(cmdline), num);
  append_arg(cmdline, sizeof(cmdline), "-language");
  append_arg(cmdline, sizeof(cmdline), config.lang);

  /* shaderapidx9 -> togl -> GLES is the only backend on the table */
  append_arg(cmdline, sizeof(cmdline), "-shaderapi");
  append_arg(cmdline, sizeof(cmdline), "shaderapidx9");
  append_arg(cmdline, sizeof(cmdline), "-dxlevel");
  append_arg(cmdline, sizeof(cmdline), "90");

  if (config.console)
    append_arg(cmdline, sizeof(cmdline), "-console");
  append_arg(cmdline, sizeof(cmdline), "+mat_queue_mode");
  append_arg(cmdline, sizeof(cmdline), "2");
  append_arg(cmdline, sizeof(cmdline), "+joystick");
  append_arg(cmdline, sizeof(cmdline), config.gamepad ? "1" : "0");
  if (config.args[0])
    append_arg(cmdline, sizeof(cmdline), config.args);

#if DEBUG_LOG
  append_arg(cmdline, sizeof(cmdline), "-dev");
  append_arg(cmdline, sizeof(cmdline), "-condebug");
#endif

  debugPrintf("command line: %s\n", cmdline);

  /* tokenize in place; argv must stay alive for the whole game run */
  static char storage[768];
  strncpy(storage, cmdline, sizeof(storage) - 1);
  storage[sizeof(storage) - 1] = '\0';

  char *tok = strtok(storage, " \t\r\n");
  while (tok && argc < max_args - 1) {
    argv[argc++] = tok;
    tok = strtok(NULL, " \t\r\n");
  }
  argv[argc] = NULL;
  if (argc == 0) { argv[0] = "gmod_linux"; argc = 1; argv[1] = NULL; }
  return argc;
}

/* FastLoad helps loading but floors GPU clocks, so switch it off once frames flow. */
extern volatile unsigned sdl_swap_count;
extern volatile int g_video_playing;

#define CLK_WIN_TICKS 12
#define CLK_BOOST_AT   5
#define CLK_DROP_AT   12

static void *clock_thread(void *arg) {
  (void)arg;
  unsigned ring[CLK_WIN_TICKS] = { 0 };
  unsigned win_sum = 0;
  unsigned last_swap = sdl_swap_count;
  int ri = 0;
  int boosted = 1;

  for (;;) {
    svcSleepThread(250000000ll);
    if (g_video_playing) {
      last_swap = sdl_swap_count;
      continue;
    }

    const unsigned cur = sdl_swap_count;
    win_sum -= ring[ri];
    ring[ri] = cur - last_swap;
    win_sum += ring[ri];
    ri = (ri + 1) % CLK_WIN_TICKS;
    last_swap = cur;

    if (boosted) {
      if (win_sum > CLK_DROP_AT) { cpu_boost(0); boosted = 0; }
    } else {
      if (win_sum <= CLK_BOOST_AT) { cpu_boost(1); boosted = 1; }
    }
  }
  return NULL;
}

static void start_clock_thread(void) {
  pthread_t t;
  pthread_attr_t at;
  pthread_attr_init(&at);
  pthread_attr_setstacksize(&at, 128 * 1024);
  pthread_create(&t, &at, clock_thread, NULL);
  pthread_attr_destroy(&at);
}


/* ------------------------------------------------------------------ */
/* Crash dump: a SIGSEGV here means some module dereferenced a bad      */
/* pointer; without PC/LR we can only guess which. Dump the fault      */
/* address, PC, LR, SP and the general registers so one run is enough. */
/* ------------------------------------------------------------------ */
#include <signal.h>
#include <unistd.h>

static void gmod_crash_handler(int sig, siginfo_t *info, void *ctx) {
  ucontext_t *uc = (ucontext_t *)ctx;
  char buf[512];
  int n;

  n = snprintf(buf, sizeof(buf),
               "\n=== CRASH sig=%d fault=%p err=0x%x ===\n"
               "PC =%p  LR =%p  SP =%p\n",
               sig, info ? info->si_addr : NULL,
               info ? info->si_code : 0,
               (void *)uc->uc_mcontext.pc,
               (void *)uc->uc_mcontext.regs[30],
               (void *)uc->uc_mcontext.sp);
  if (n > 0) { ssize_t w = write(1, buf, (size_t)n); (void)w; }
  debugPrintf("%s", buf);

  for (int i = 0; i < 29; i += 4) {
    n = snprintf(buf, sizeof(buf),
                 "x%-2d=%016llx x%-2d=%016llx x%-2d=%016llx x%-2d=%016llx\n",
                 i,   (unsigned long long)uc->uc_mcontext.regs[i],
                 i+1, (unsigned long long)uc->uc_mcontext.regs[i+1],
                 i+2, (unsigned long long)uc->uc_mcontext.regs[i+2],
                 i+3, (unsigned long long)uc->uc_mcontext.regs[i+3]);
    if (n > 0) { ssize_t w = write(1, buf, (size_t)n); (void)w; }
    debugPrintf("%s", buf);
  }

  /* 打印栈上的返回地址，便于人工回溯 */
  uintptr_t *sp = (uintptr_t *)uc->uc_mcontext.sp;
  n = snprintf(buf, sizeof(buf), "--- stack ---\n");
  if (n > 0) { ssize_t w = write(1, buf, (size_t)n); (void)w; }
  for (int i = 0; i < 32; i++) {
    uintptr_t v = 0;
    /* 只读用户态地址，避免二次崩溃 */
    if ((uintptr_t)(sp + i) > 0x1000 && (uintptr_t)(sp + i) < 0x00007fffffffe000ULL)
      v = sp[i];
    n = snprintf(buf, sizeof(buf), "  sp+%03x: %016llx\n", i * 8,
                 (unsigned long long)v);
    if (n > 0) { ssize_t w = write(1, buf, (size_t)n); (void)w; }
    debugPrintf("%s", buf);
  }

  n = snprintf(buf, sizeof(buf), "=== END CRASH ===\n");
  if (n > 0) { ssize_t w = write(1, buf, (size_t)n); (void)w; }
  debugPrintf("%s", buf);

  _exit(128 + sig);
}

static int crash_handler_installed = 0;

static void gmod_install_crash_handler(void) {
  if (crash_handler_installed) return;
  struct sigaction sa;
  memset(&sa, 0, sizeof(sa));
  sa.sa_sigaction = gmod_crash_handler;
  sa.sa_flags = SA_SIGINFO | SA_ONSTACK;
  sigemptyset(&sa.sa_mask);
  sigaction(SIGSEGV, &sa, NULL);
  sigaction(SIGBUS,  &sa, NULL);
  sigaction(SIGILL,  &sa, NULL);
  sigaction(SIGFPE,  &sa, NULL);
  sigaction(SIGABRT, &sa, NULL);
  crash_handler_installed = 1;
  debugPrintf("crash handler installed (SEGV/BUS/ILL/FPE/ABRT)\n");
}
int main(void) {
  gmod_install_crash_handler();
  saltynx_start();
  debugPrintf("heap: total=%zuMB avail=%zuMB used=%zuMB newlib=%zuMB pool=%zuMB\n",
              g_diag_total >> 20, g_diag_avail >> 20, g_diag_used >> 20,
              g_diag_heap >> 20, g_diag_pool >> 20);
  atexit(saltynx_stop);
  cpu_boost(1);

  if (!envIsSyscallHinted(0x77) || !envIsSyscallHinted(0x78) ||
      !envIsSyscallHinted(0x73)) {
    fatal_error("svcMapProcessCodeMemory is unavailable.\n"
                "Run with a full title override or a forwarder, not applet mode.");
  }
  if (envGetOwnProcessHandle() == INVALID_HANDLE)
    fatal_error("Own process handle is unavailable.");

  if (read_config(CONFIG_NAME) < 0) {
    chdir(DEFAULT_INSTALL_ROOT);
    if (read_config(CONFIG_NAME) < 0)
      write_config(CONFIG_NAME);
  }
  chdir(config.install_root);

  check_data();
  set_screen_size();

  fake_tls_install();
  jni_init();
  proc_files_init(config.install_root);

  gmod_stdio_init();
  so_set_skip_ctors(config.skip_ctors);
  if (config.skip_ctors[0])
    debugPrintf("skip_ctors: %s\n", config.skip_ctors);
  load_modules();

  so_module *launcher = so_find_module("liblauncher.so");
  if (!launcher)
    fatal_error("liblauncher.so did not load");

  setup_environment();

  int (*LauncherMainAndroid)(int argc, char **argv) =
      (void *)so_find_addr_rx(launcher, "LauncherMainAndroid");
  if (!LauncherMainAndroid) {
    /* fall back to the plain entry some GDroid builds only export */
    LauncherMainAndroid = (void *)so_find_addr_rx(launcher, "LauncherMain");
  }
  if (!LauncherMainAndroid)
    fatal_error("liblauncher.so exports neither\nLauncherMainAndroid nor LauncherMain");

  static char *argv[64];
  const int argc = build_argv(argv, 64);

  start_clock_thread();

  int ret = LauncherMainAndroid(argc, argv);
  debugPrintf("LauncherMainAndroid returned %d\n", ret);

  extern void NX_NORETURN __libnx_exit(int rc);
  __libnx_exit(0);

  return 0;
}
