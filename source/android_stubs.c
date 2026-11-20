/* android_stubs.c -- Android platform surfaces the GDroid binaries import.
 *
 * The GDroid ARM64 modules link against libandroid, libaaudio, libmediandk,
 * libvulkan and libOpenSLES. None of those exist on Horizon, and two of them
 * cannot be provided honestly:
 *
 *   Vulkan   -- there is no Vulkan driver. Every vk* entry resolves to
 *               gmod_vk_fail, which reports VK_ERROR_INITIALIZATION_FAILED,
 *               so libvrxr's probe fails and the VR path stays closed while
 *               the rest of the engine keeps working.
 *   MediaNDK -- no MediaCodec. Creators return NULL; callers see a codec that
 *               will not open, which the engine tolerates (no in-game video).
 *
 * AAsset is the one API that has to work for real, because the engine reads
 * shipped assets through it. It is backed by plain stdio on the SD card.
 *
 * Distributed under the MIT license. See LICENSE for details.
 */

#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <sys/stat.h>

#include "config.h"
#include "android_stubs.h"
#include "so_util.h"

/* ------------------------------------------------------------------ */
/* generic catch-alls                                                   */
/* ------------------------------------------------------------------ */

/* Vulkan: VK_ERROR_INITIALIZATION_FAILED (-3). */
uintptr_t gmod_vk_fail(void) {
  return (uintptr_t)(intptr_t)-3;
}

/* success / NULL / false -- the shape most Android getters want. */
uintptr_t gmod_stub_zero(void) {
  return 0;
}

/* ------------------------------------------------------------------ */
/* AAudio                                                               */
/* ------------------------------------------------------------------ */

/* AAUDIO_ERROR_UNAVAILABLE */
#define AAUDIO_ERR_UNAVAILABLE (-899)

int32_t stub_AAudio_createStreamBuilder(void **builder) {
  if (builder) *builder = NULL;
  return AAUDIO_ERR_UNAVAILABLE;
}

const char *stub_AAudio_convertResultToText(int32_t result) {
  (void)result;
  return "unavailable";
}

/* ------------------------------------------------------------------ */
/* ANativeWindow                                                        */
/* ------------------------------------------------------------------ */

/* One fake window object, enough for the engine's capability queries.        */
/* switch-sdl2 owns the real EGL surface; nothing here draws.                 */
static uint32_t g_fake_window[16];

void *stub_ANativeWindow_fromSurface(void *env, void *surface) {
  (void)env; (void)surface;
  return g_fake_window;
}

void stub_ANativeWindow_acquire(void *w) { (void)w; }
void stub_ANativeWindow_release(void *w) { (void)w; }

int32_t stub_ANativeWindow_getWidth(void *w)  { (void)w; return screen_width; }
int32_t stub_ANativeWindow_getHeight(void *w) { (void)w; return screen_height; }
int32_t stub_ANativeWindow_getFormat(void *w) { (void)w; return 1; /* WINDOW_FORMAT_RGBA_8888 */ }

int32_t stub_ANativeWindow_setBuffersGeometry(void *w, int32_t width, int32_t height, int32_t format) {
  (void)w; (void)width; (void)height; (void)format;
  return 0;
}

/* ------------------------------------------------------------------ */
/* MediaNDK (AMediaCodec / AMediaExtractor / AMediaFormat)              */
/* ------------------------------------------------------------------ */

void *stub_AMediaExtractor_getTrackFormat(void *ex, size_t track) {
  (void)ex; (void)track;
  return NULL;
}

void *stub_AMediaCodec_getOutputFormat(void *codec) {
  (void)codec;
  return NULL;
}

void *stub_AMediaCodec_getInputBuffer(void *codec, size_t idx, size_t *out_size) {
  (void)codec; (void)idx;
  if (out_size) *out_size = 0;
  return NULL;
}

void *stub_AMediaCodec_getOutputBuffer(void *codec, size_t idx, size_t *out_size) {
  (void)codec; (void)idx;
  if (out_size) *out_size = 0;
  return NULL;
}

/* AMEDIACODEC_INFO_TRY_AGAIN_LATER */
int32_t stub_AMediaCodec_dequeueInputBuffer(void *codec, int64_t timeout) {
  (void)codec; (void)timeout;
  return -1;
}

int32_t stub_AMediaCodec_dequeueOutputBuffer(void *codec, void *info, int64_t timeout) {
  (void)codec; (void)timeout;
  if (info) memset(info, 0, 32); /* AMediaCodecBufferInfo */
  return -1;
}

bool stub_AMediaFormat_getInt32(void *fmt, const char *name, int32_t *out) {
  (void)fmt; (void)name;
  if (out) *out = 0;
  return false;
}

bool stub_AMediaFormat_getInt64(void *fmt, const char *name, int64_t *out) {
  (void)fmt; (void)name;
  if (out) *out = 0;
  return false;
}

bool stub_AMediaFormat_getString(void *fmt, const char *name, const char **out) {
  (void)fmt; (void)name;
  if (out) *out = NULL;
  return false;
}

/* These are variables, not functions: the engine compares the pointer value,
 * so each one has to be the address of a distinct object. */
const char stub_AMEDIAFORMAT_KEY_BIT_RATE[]      = "bitrate";
const char stub_AMEDIAFORMAT_KEY_CHANNEL_COUNT[] = "channel-count";
const char stub_AMEDIAFORMAT_KEY_DURATION[]      = "durationUs";
const char stub_AMEDIAFORMAT_KEY_MIME[]          = "mime";
const char stub_AMEDIAFORMAT_KEY_SAMPLE_RATE[]   = "sample-rate";

/* ------------------------------------------------------------------ */
/* libandroid: AAsset -- backed by real stdio                           */
/* ------------------------------------------------------------------ */

/* GDroid installs keep everything on the SD card, so the asset namespace can
 * simply be rooted at the install directory. */
static void asset_join(const char *path, char *out, size_t out_size) {
  if (!path) { out[0] = '\0'; return; }
  if (path[0] == '/' || path[0] == '\0')
    snprintf(out, out_size, "%s", path);
  else
    snprintf(out, out_size, "%s/%s", "" /* cwd already is install root */, path);
}

typedef struct {
  FILE *fp;
  long size;
} AAssetFake;

void *stub_AAssetManager_fromJava(void *env, void *mgr) {
  (void)env;
  /* the caller only round-trips this handle, so hand back the manager itself */
  return mgr ? mgr : (void *)&asset_join;
}

void *stub_AAssetManager_open(void *mgr, const char *filename, int mode) {
  (void)mgr; (void)mode;
  char path[512];
  asset_join(filename, path, sizeof(path));

  FILE *fp = fopen(path, "rb");
  if (!fp)
    return NULL;

  AAssetFake *a = (AAssetFake *)calloc(1, sizeof(*a));
  if (!a) { fclose(fp); return NULL; }
  a->fp = fp;
  fseek(fp, 0, SEEK_END);
  a->size = ftell(fp);
  fseek(fp, 0, SEEK_SET);
  return a;
}

void *stub_AAssetManager_openDir(void *mgr, const char *dir_name) {
  (void)mgr; (void)dir_name;
  return NULL;
}

int stub_AAsset_read(void *asset, void *buf, size_t count) {
  AAssetFake *a = (AAssetFake *)asset;
  if (!a || !a->fp) return -1;
  return (int)fread(buf, 1, count, a->fp);
}

const void *stub_AAsset_getBuffer(void *asset) {
  AAssetFake *a = (AAssetFake *)asset;
  if (!a || !a->fp) return NULL;

  void *buf = malloc((size_t)a->size);
  if (!buf) return NULL;
  fseek(a->fp, 0, SEEK_SET);
  if (fread(buf, 1, (size_t)a->size, a->fp) != (size_t)a->size) {
    free(buf);
    return NULL;
  }
  return buf;
}

int64_t stub_AAsset_getLength(void *asset) {
  AAssetFake *a = (AAssetFake *)asset;
  return a ? (int64_t)a->size : 0;
}

int64_t stub_AAsset_getLength64(void *asset) {
  return stub_AAsset_getLength(asset);
}

int64_t stub_AAsset_seek64(void *asset, int64_t offset, int whence) {
  AAssetFake *a = (AAssetFake *)asset;
  if (!a || !a->fp) return -1;
  if (fseek(a->fp, (long)offset, whence) != 0) return -1;
  return (int64_t)ftell(a->fp);
}

void stub_AAsset_close(void *asset) {
  AAssetFake *a = (AAssetFake *)asset;
  if (!a) return;
  if (a->fp) fclose(a->fp);
  free(a);
}

const char *stub_AAssetDir_getNextFileName(void *dir) { (void)dir; return NULL; }
void stub_AAssetDir_close(void *dir) { (void)dir; }

/* ------------------------------------------------------------------ */
/* ALooper / ASensor / AConfiguration                                   */
/* ------------------------------------------------------------------ */

void *stub_ALooper_prepare(int opts) { (void)opts; return (void *)&g_fake_window; }
void *stub_ALooper_forThread(void) { return (void *)&g_fake_window; }

/* ALOOPER_POLL_TIMEOUT -- nothing ever arrives on this looper */
int stub_ALooper_pollAll(int timeout_ms, int *out_fd, int *out_events, void **out_data) {
  (void)timeout_ms;
  if (out_fd) *out_fd = -1;
  if (out_events) *out_events = 0;
  if (out_data) *out_data = NULL;
  return -3;
}

void *stub_ASensorManager_getInstance(void) { return (void *)&g_fake_window; }

int stub_ASensorManager_getSensorList(void *mgr, void *list) {
  (void)mgr; (void)list;
  return 0;
}

void *stub_ASensorManager_createEventQueue(void *mgr, void *looper, int ident, void *cb, void *data) {
  (void)mgr; (void)looper; (void)ident; (void)cb; (void)data;
  return NULL;
}

void stub_ASensorManager_destroyEventQueue(void *mgr, void *queue) { (void)mgr; (void)queue; }

int stub_ASensorEventQueue_enableSensor(void *q, void *sensor) { (void)q; (void)sensor; return -1; }
int stub_ASensorEventQueue_disableSensor(void *q, void *sensor) { (void)q; (void)sensor; return -1; }

int stub_ASensorEventQueue_getEvents(void *q, void *events, size_t count) {
  (void)q; (void)events; (void)count;
  return 0;
}

int stub_ASensorEventQueue_setEventRate(void *q, void *sensor, int32_t rate) {
  (void)q; (void)sensor; (void)rate;
  return -1;
}

const char *stub_ASensor_getName(void *sensor) { (void)sensor; return "none"; }
int32_t stub_ASensor_getType(void *sensor) { (void)sensor; return 0; }
int32_t stub_ASensor_getMinDelay(void *sensor) { (void)sensor; return 0; }

/* AConfiguration carries language/country for the engine's locale pick. */
static char g_config_language[4] = "en";
static char g_config_country[4]  = "US";

void *stub_AConfiguration_new(void) { return &g_config_language; }

void *stub_AConfiguration_fromAssetManager(void *mgr) {
  (void)mgr;
  return &g_config_language;
}

void stub_AConfiguration_delete(void *cfg) { (void)cfg; }

void stub_AConfiguration_getLanguage(void *cfg, char *out) {
  (void)cfg;
  if (out) memcpy(out, g_config_language, sizeof(g_config_language));
}

void stub_AConfiguration_getCountry(void *cfg, char *out) {
  (void)cfg;
  if (out) memcpy(out, g_config_country, sizeof(g_config_country));
}

/* ------------------------------------------------------------------ */
/* OpenSL ES                                                            */
/* ------------------------------------------------------------------ */

/* SLInterfaceID values are compared, never dereferenced, so each one just has
 * to be the address of a distinct object. Declaring them as arrays keeps
 * &name pointing at the object rather than at a pointer variable. */
const char stub_SL_IID_ENGINE[16];
const char stub_SL_IID_PLAY[16];
const char stub_SL_IID_RECORD[16];
const char stub_SL_IID_VOLUME[16];
const char stub_SL_IID_ANDROIDSIMPLEBUFFERQUEUE[16];

int32_t stub_slCreateEngine(void **engine, uint32_t num_options, const void *options,
                            uint32_t num_interfaces, const void *interfaces, const void *req) {
  (void)num_options; (void)options; (void)num_interfaces; (void)interfaces; (void)req;
  if (engine) *engine = NULL;
  return -1;
}

/* ------------------------------------------------------------------ */
/* zstd tracing hooks (libzstd-jni registers these; nothing calls them)  */
/* ------------------------------------------------------------------ */

void stub_ZSTD_trace_compress_begin(void) {}
void stub_ZSTD_trace_compress_end(void) {}
void stub_ZSTD_trace_decompress_begin(void) {}
void stub_ZSTD_trace_decompress_end(void) {}
