/* android_stubs.h -- declarations for the Android platform surfaces the
 * GDroid binaries import, so that imports.c can take their addresses.
 *
 * Only the address is ever taken (&symbol cast to uintptr_t), so the declared
 * signatures here are deliberately minimal: exact types do not affect linkage.
 * The variables (AMEDIAFORMAT_KEY_*, SL_IID_*) must be declared with their
 * real type because their *address* is the value the engine compares.
 *
 * Distributed under the MIT license. See LICENSE for details.
 */

#ifndef __ANDROID_STUBS_H__
#define __ANDROID_STUBS_H__

#include <stdint.h>
#include <stdbool.h>

/* generic catch-alls */
uintptr_t gmod_vk_fail(void);
uintptr_t gmod_stub_zero(void);

/* AAudio */
int32_t      stub_AAudio_createStreamBuilder(void **builder);
const char  *stub_AAudio_convertResultToText(int32_t result);

/* ANativeWindow */
void  *stub_ANativeWindow_fromSurface(void *env, void *surface);
void   stub_ANativeWindow_acquire(void *w);
void   stub_ANativeWindow_release(void *w);
int32_t stub_ANativeWindow_getWidth(void *w);
int32_t stub_ANativeWindow_getHeight(void *w);
int32_t stub_ANativeWindow_getFormat(void *w);
int32_t stub_ANativeWindow_setBuffersGeometry(void *w, int32_t width, int32_t height, int32_t format);

/* MediaNDK */
void    *stub_AMediaExtractor_getTrackFormat(void *ex, size_t track);
void    *stub_AMediaCodec_getOutputFormat(void *codec);
void    *stub_AMediaCodec_getInputBuffer(void *codec, size_t idx, size_t *out_size);
void    *stub_AMediaCodec_getOutputBuffer(void *codec, size_t idx, size_t *out_size);
int32_t  stub_AMediaCodec_dequeueInputBuffer(void *codec, int64_t timeout);
int32_t  stub_AMediaCodec_dequeueOutputBuffer(void *codec, void *info, int64_t timeout);
bool     stub_AMediaFormat_getInt32(void *fmt, const char *name, int32_t *out);
bool     stub_AMediaFormat_getInt64(void *fmt, const char *name, int64_t *out);
bool     stub_AMediaFormat_getString(void *fmt, const char *name, const char **out);

extern const char stub_AMEDIAFORMAT_KEY_BIT_RATE[];
extern const char stub_AMEDIAFORMAT_KEY_CHANNEL_COUNT[];
extern const char stub_AMEDIAFORMAT_KEY_DURATION[];
extern const char stub_AMEDIAFORMAT_KEY_MIME[];
extern const char stub_AMEDIAFORMAT_KEY_SAMPLE_RATE[];

/* AAsset (real implementation, stdio-backed) */
void    *stub_AAssetManager_fromJava(void *env, void *mgr);
void    *stub_AAssetManager_open(void *mgr, const char *filename, int mode);
void    *stub_AAssetManager_openDir(void *mgr, const char *dir_name);
int      stub_AAsset_read(void *asset, void *buf, size_t count);
const void *stub_AAsset_getBuffer(void *asset);
int64_t  stub_AAsset_getLength(void *asset);
int64_t  stub_AAsset_getLength64(void *asset);
int64_t  stub_AAsset_seek64(void *asset, int64_t offset, int whence);
void     stub_AAsset_close(void *asset);
const char *stub_AAssetDir_getNextFileName(void *dir);
void     stub_AAssetDir_close(void *dir);

/* ALooper */
void  *stub_ALooper_prepare(int opts);
void  *stub_ALooper_forThread(void);
int    stub_ALooper_pollAll(int timeout_ms, int *out_fd, int *out_events, void **out_data);

/* ASensor */
void  *stub_ASensorManager_getInstance(void);
int    stub_ASensorManager_getSensorList(void *mgr, void *list);
void  *stub_ASensorManager_createEventQueue(void *mgr, void *looper, int ident, void *cb, void *data);
void   stub_ASensorManager_destroyEventQueue(void *mgr, void *queue);
int    stub_ASensorEventQueue_enableSensor(void *q, void *sensor);
int    stub_ASensorEventQueue_disableSensor(void *q, void *sensor);
int    stub_ASensorEventQueue_getEvents(void *q, void *events, size_t count);
int    stub_ASensorEventQueue_setEventRate(void *q, void *sensor, int32_t rate);
const char *stub_ASensor_getName(void *sensor);
int32_t stub_ASensor_getType(void *sensor);
int32_t stub_ASensor_getMinDelay(void *sensor);

/* AConfiguration */
void  *stub_AConfiguration_new(void);
void  *stub_AConfiguration_fromAssetManager(void *mgr);
void   stub_AConfiguration_delete(void *cfg);
void   stub_AConfiguration_getLanguage(void *cfg, char *out);
void   stub_AConfiguration_getCountry(void *cfg, char *out);

/* OpenSL ES */
int32_t stub_slCreateEngine(void **engine, uint32_t num_options, const void *options,
                            uint32_t num_interfaces, const void *interfaces, const void *req);

extern const char stub_SL_IID_ENGINE[16];
extern const char stub_SL_IID_PLAY[16];
extern const char stub_SL_IID_RECORD[16];
extern const char stub_SL_IID_VOLUME[16];
extern const char stub_SL_IID_ANDROIDSIMPLEBUFFERQUEUE[16];

/* zstd tracing hooks */
void stub_ZSTD_trace_compress_begin(void);
void stub_ZSTD_trace_compress_end(void);
void stub_ZSTD_trace_decompress_begin(void);
void stub_ZSTD_trace_decompress_end(void);

/* Android-only SDL helpers (see android_stubs.c) */
void *gmod_android_get_jnienv(void);
void *gmod_android_get_activity(void);

#endif