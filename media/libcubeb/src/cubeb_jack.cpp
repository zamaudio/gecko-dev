/*
 * Copyright © 2012 David Richards
 * Copyright © 2013 Sebastien Alaiwan
 * Copyright © 2016 Damien Zammit
 *
 * This program is made available under an ISC-style license.  See the
 * accompanying file LICENSE for details.
 */
#define _DEFAULT_SOURCE
#define _BSD_SOURCE
#define _POSIX_SOURCE
#include <algorithm>
#include <dlfcn.h>
#include <limits>
#include <stdio.h>
#include <sys/time.h>
#include <assert.h>
#include <string.h>
#include <limits.h>
#include <poll.h>
#include <unistd.h>
#include <stdlib.h>
#include "cubeb/cubeb.h"
#include "cubeb-internal.h"
#include "cubeb_resampler.h"

#include <jack/jack.h>
#include <jack/statistics.h>

#define JACK_API_VISIT(X)                       \
  X(jack_activate)                              \
  X(jack_client_close)                          \
  X(jack_client_open)                           \
  X(jack_connect)                               \
  X(jack_free)                                  \
  X(jack_get_ports)                             \
  X(jack_get_sample_rate)                       \
  X(jack_get_xrun_delayed_usecs)                \
  X(jack_get_buffer_size)                       \
  X(jack_port_get_buffer)                       \
  X(jack_port_name)                             \
  X(jack_port_register)                         \
  X(jack_port_unregister)                       \
  X(jack_port_get_latency_range)                \
  X(jack_set_process_callback)                  \
  X(jack_set_xrun_callback)                     \
  X(jack_set_graph_order_callback)

#define IMPORT_FUNC(x) static decltype(x) * api_##x;
JACK_API_VISIT(IMPORT_FUNC);

static const int MAX_STREAMS = 16;
static const int MAX_CHANNELS  = 8;
static const int FIFO_SIZE = 4096 * sizeof(float);
static const bool AUTO_CONNECT_JACK_PORTS = true;

enum devstream {
  NONE = 0,
  IN_ONLY,
  OUT_ONLY,
  DUPLEX,
};

static void
s16ne_to_float(float *dst, const int16_t *src, size_t n)
{
  for (size_t i = 0; i < n; i++)
    *(dst++) = (float)((float)*(src++) / 32767.0f);
}

extern "C"
{
/*static*/ int jack_init (cubeb ** context, char const * context_name);
}
static char const * cbjack_get_backend_id(cubeb * context);
static int cbjack_get_max_channel_count(cubeb * ctx, uint32_t * max_channels);
static int cbjack_get_min_latency(cubeb * ctx, cubeb_stream_params params, uint32_t * latency_ms);
static int cbjack_get_latency(cubeb_stream * stm, unsigned int * latency_ms);
static int cbjack_get_preferred_sample_rate(cubeb * ctx, uint32_t * rate);
static void cbjack_destroy(cubeb * context);
static void cbjack_deinterleave_audio(cubeb_stream * stream, float **bufs_in, float **bufs_out, jack_nframes_t nframes);
static int cbjack_stream_device_destroy(cubeb_stream * stream,
                                        cubeb_device * device);
static int cbjack_stream_get_current_device(cubeb_stream * stm, cubeb_device ** const device);
static int cbjack_enumerate_devices(cubeb * context, cubeb_device_type type,
                                    cubeb_device_collection ** collection);
static int cbjack_stream_init(cubeb * context, cubeb_stream ** stream, char const * stream_name,
                              cubeb_devid input_device,
                              cubeb_stream_params * input_stream_params,
                              cubeb_devid output_device,
                              cubeb_stream_params * output_stream_params,
                              unsigned int latency,
                              cubeb_data_callback data_callback,
                              cubeb_state_callback state_callback,
                              void * user_ptr);
static void cbjack_stream_destroy(cubeb_stream * stream);
static int cbjack_stream_start(cubeb_stream * stream);
static int cbjack_stream_stop(cubeb_stream * stream);
static int cbjack_stream_get_position(cubeb_stream * stream, uint64_t * position);
static int cbjack_stream_set_volume(cubeb_stream * stm, float volume);

static struct cubeb_ops const cbjack_ops = {
  .init = jack_init,
  .get_backend_id = cbjack_get_backend_id,
  .get_max_channel_count = cbjack_get_max_channel_count,
  .get_min_latency = cbjack_get_min_latency,
  .get_preferred_sample_rate = cbjack_get_preferred_sample_rate,
  .enumerate_devices = cbjack_enumerate_devices,
  .destroy = cbjack_destroy,
  .stream_init = cbjack_stream_init,
  .stream_destroy = cbjack_stream_destroy,
  .stream_start = cbjack_stream_start,
  .stream_stop = cbjack_stream_stop,
  .stream_get_position = cbjack_stream_get_position,
  .stream_get_latency = cbjack_get_latency,
  .stream_set_volume = cbjack_stream_set_volume,
  .stream_set_panning = NULL,
  .stream_get_current_device = cbjack_stream_get_current_device,
  .stream_device_destroy = cbjack_stream_device_destroy,
  .stream_register_device_changed_callback = NULL,
  .register_device_collection_changed = NULL
};

struct cubeb_stream {
  cubeb *context;

  bool in_use; /**< Set to false iff the stream is free */
  bool ports_ready; /**< Set to true iff the JACK ports are ready */

  cubeb_data_callback data_callback;
  cubeb_state_callback state_callback;
  void *user_ptr;
  cubeb_stream_params in_params;
  cubeb_stream_params out_params;

  cubeb_resampler *resampler;

  uint64_t position;
  bool pause;
  float ratio;
  enum devstream devs;
  char stream_name[256];
  jack_port_t *output_ports[MAX_CHANNELS];
  jack_port_t *input_ports[MAX_CHANNELS];
  float volume;
};

struct cubeb {
  struct cubeb_ops const * ops;
  void *libjack;

  /**< Audio buffers, converted to float */
  float in_float_interleaved_buffer[FIFO_SIZE * MAX_CHANNELS];
  float out_float_interleaved_buffer[FIFO_SIZE * MAX_CHANNELS];

  /**< Audio buffer, at the sampling rate of the output */
  float in_resampled_interleaved_buffer_float[FIFO_SIZE * MAX_CHANNELS * 3];
  int16_t in_resampled_interleaved_buffer_s16ne[FIFO_SIZE * MAX_CHANNELS * 3];
  float out_resampled_interleaved_buffer_float[FIFO_SIZE * MAX_CHANNELS * 3];
  int16_t out_resampled_interleaved_buffer_s16ne[FIFO_SIZE * MAX_CHANNELS * 3];

  cubeb_stream streams[MAX_STREAMS];
  unsigned int active_streams;

  cubeb_device_info *devinfo;
  cubeb_device_collection_changed_callback collection_changed_callback;

  bool active;
  unsigned int jack_sample_rate;
  unsigned int jack_latency;
  unsigned int jack_xruns;
  unsigned int jack_buffer_size;
  unsigned int fragment_size;
  unsigned int output_bytes_per_frame;
  jack_client_t *jack_client;
};

static int
load_jack_lib(cubeb* context)
{
#ifdef __APPLE__
  context->libjack = dlopen("libjack.0.dylib", RTLD_LAZY);
  context->libjack = dlopen("/usr/local/lib/libjack.0.dylib", RTLD_LAZY);
#elif defined(__WIN32__)
# ifdef _WIN64
   context->libjack = LoadLibrary("libjack64.dll");
# else
   context->libjack = LoadLibrary("libjack.dll");
# endif
#else
  context->libjack = dlopen("libjack.so.0", RTLD_LAZY);
#endif
  if (!context->libjack) {
    return CUBEB_ERROR;
  }

#define LOAD(x)                                           \
  {                                                       \
    api_##x = (decltype(x)*)dlsym(context->libjack, #x);  \
    if (!api_##x) {                                       \
      dlclose(context->libjack);                          \
      return CUBEB_ERROR;                                 \
    }                                                     \
  }

  JACK_API_VISIT(LOAD);
#undef LOAD

  return CUBEB_OK;
}

static void
cbjack_connect_ports (cubeb_stream * stream)
{
  const char **phys_in_ports = api_jack_get_ports (stream->context->jack_client,
                                                   NULL, NULL,
                                                   JackPortIsInput
                                                   | JackPortIsPhysical);
  const char **phys_out_ports = api_jack_get_ports (stream->context->jack_client,
                                                    NULL, NULL,
                                                    JackPortIsOutput
                                                    | JackPortIsPhysical);

 if (*phys_in_ports == NULL) {
    goto skipplayback;
  }

  // Connect outputs to playback
  for (unsigned int c = 0; c < stream->out_params.channels && stream->output_ports[c]; c++) {
    const char *src_port = api_jack_port_name (stream->output_ports[c]);

    api_jack_connect (stream->context->jack_client, src_port, phys_in_ports[c]);
  }

skipplayback:
  if (*phys_out_ports == NULL) {
    goto end;
  }
  // Connect inputs to capture
  for (unsigned int c = 0; c < stream->in_params.channels && stream->input_ports[c]; c++) {
    const char *src_port = api_jack_port_name (stream->input_ports[c]);

    api_jack_connect (stream->context->jack_client, phys_out_ports[c], src_port);
  }
end:
  api_jack_free(phys_out_ports);
  api_jack_free(phys_in_ports);
}

static int
cbjack_xrun_callback(void *arg)
{
  cubeb *ctx = (cubeb *)arg;

  float delay = api_jack_get_xrun_delayed_usecs(ctx->jack_client);
  int fragments = (int)ceilf( ((delay / 1000000.0) * ctx->jack_sample_rate )
                             / (float)(ctx->jack_buffer_size) );
  ctx->jack_xruns += fragments;
  return 0;
}

static int
cbjack_graph_order_callback(void *arg)
{
  cubeb *ctx = (cubeb *)arg;
  int i;

  jack_latency_range_t latency_range;
  jack_nframes_t port_latency, max_latency = 0;

  for (int j = 0; j < MAX_STREAMS; j++) {
    cubeb_stream *stm = &ctx->streams[j];

    if (!stm->in_use)
      continue;
    if (!stm->ports_ready)
      continue;

    for (i = 0; i < (int)stm->out_params.channels; ++i) {
      api_jack_port_get_latency_range(stm->output_ports[i], JackPlaybackLatency, &latency_range);
      port_latency = latency_range.max;
      if (port_latency > max_latency)
          max_latency = port_latency;
    }
    ctx->jack_latency = (max_latency * 1000) / ctx->jack_sample_rate;
  }
  return 0;
}

static int
cbjack_process(jack_nframes_t nframes, void *arg)
{
  cubeb *ctx = (cubeb *)arg;
  int t_jack_xruns = ctx->jack_xruns;
  int i;

  for (int j = 0; j < MAX_STREAMS; j++) {
    cubeb_stream *stm = &ctx->streams[j];
    float *bufs_out[stm->out_params.channels];
    float *bufs_in[stm->in_params.channels];

    if (!stm->in_use)
      continue;

    // handle xruns by skipping audio that should have been played
    for (i = 0; i < t_jack_xruns; i++) {
        stm->position += ctx->fragment_size * stm->ratio;
    }
    ctx->jack_xruns -= t_jack_xruns;


    if (!stm->ports_ready)
      continue;

    // get jack output buffers
    for (i = 0; i < (int)stm->out_params.channels; i++)
      bufs_out[i] = (float*)api_jack_port_get_buffer(stm->output_ports[i], nframes);

    // get jack input buffers
    for (i = 0; i < (int)stm->in_params.channels; i++)
      bufs_in[i] = (float*)api_jack_port_get_buffer(stm->input_ports[i], nframes);

    if (stm->pause) {
      // paused, play silence on output
      for (unsigned int c = 0; c < stm->out_params.channels; c++) {
        float* buffer_out = bufs_out[c];
        for (long f = 0; f < nframes; f++) {
          buffer_out[f] = 0.f;
        }
      }
      // paused, capture silence
      for (unsigned int c = 0; c < stm->in_params.channels; c++) {
        float* buffer_in = bufs_in[c];
        for (long f = 0; f < nframes; f++) {
          buffer_in[f] = 0.f;
        }
      }
    } else {
      // unpaused, play audio
      cbjack_deinterleave_audio(stm, bufs_in, bufs_out, nframes);
    }
  }

  return 0;
}

static void
cbjack_deinterleave_audio(cubeb_stream * stream, float **bufs_in, float **bufs_out, jack_nframes_t nframes)
{
  float *in_interleaved_buffer = nullptr;
  float *out_interleaved_buffer = nullptr;

  long resampler_needed_frames = nframes;
  long resampler_done_frames = 0;
  long input_frames_count = nframes;

 if (stream->devs == DUPLEX) {
  if (stream->out_params.format == CUBEB_SAMPLE_S16NE) {
    resampler_done_frames = cubeb_resampler_fill(stream->resampler,
                                                   stream->context->in_resampled_interleaved_buffer_s16ne,
                                                   &input_frames_count,
                                                   stream->context->out_resampled_interleaved_buffer_s16ne,
                                                   resampler_needed_frames);

    s16ne_to_float(stream->context->out_resampled_interleaved_buffer_float, stream->context->out_resampled_interleaved_buffer_s16ne, resampler_done_frames * stream->out_params.channels);
    s16ne_to_float(stream->context->in_resampled_interleaved_buffer_float, stream->context->in_resampled_interleaved_buffer_s16ne, input_frames_count * stream->in_params.channels);

  } else if (stream->out_params.format == CUBEB_SAMPLE_FLOAT32NE) {
    resampler_done_frames = cubeb_resampler_fill(stream->resampler,
                                                   stream->context->in_resampled_interleaved_buffer_float,
                                                   &input_frames_count,
                                                   stream->context->out_resampled_interleaved_buffer_float,
                                                   resampler_needed_frames);
  }
  out_interleaved_buffer = stream->context->out_resampled_interleaved_buffer_float;
  in_interleaved_buffer = stream->context->in_resampled_interleaved_buffer_float;
 } else if (stream->devs == OUT_ONLY) {
  if (stream->out_params.format == CUBEB_SAMPLE_S16NE) {
    resampler_done_frames = cubeb_resampler_fill(stream->resampler,
                                                   NULL,
                                                   0,
                                                   stream->context->out_resampled_interleaved_buffer_s16ne,
                                                   resampler_needed_frames);

    s16ne_to_float(stream->context->out_resampled_interleaved_buffer_float, stream->context->out_resampled_interleaved_buffer_s16ne, resampler_done_frames * stream->out_params.channels);
  } else if (stream->out_params.format == CUBEB_SAMPLE_FLOAT32NE) {
    resampler_done_frames = cubeb_resampler_fill(stream->resampler,
                                                   NULL,
                                                   0,
                                                   stream->context->out_resampled_interleaved_buffer_float,
                                                   resampler_needed_frames);
  }
  out_interleaved_buffer = stream->context->out_resampled_interleaved_buffer_float;
 } else {
  resampler_needed_frames = 0;
  if (stream->in_params.format == CUBEB_SAMPLE_S16NE) {
    resampler_done_frames = cubeb_resampler_fill(stream->resampler,
                                                   stream->context->in_resampled_interleaved_buffer_s16ne,
                                                   &input_frames_count,
                                                   NULL,
                                                   resampler_needed_frames);

    s16ne_to_float(stream->context->in_resampled_interleaved_buffer_float, stream->context->in_resampled_interleaved_buffer_s16ne, input_frames_count * stream->in_params.channels);

  } else if (stream->in_params.format == CUBEB_SAMPLE_FLOAT32NE) {
    resampler_done_frames = cubeb_resampler_fill(stream->resampler,
                                                   stream->context->in_resampled_interleaved_buffer_float,
                                                   &input_frames_count,
                                                   NULL,
                                                   resampler_needed_frames);
  }
  in_interleaved_buffer = stream->context->in_resampled_interleaved_buffer_float;
 }

 if (stream->devs == OUT_ONLY || stream->devs == DUPLEX) {
  // convert interleaved output buffers to contiguous buffers
  for (unsigned int c = 0; c < stream->out_params.channels; c++) {
    float* buffer = bufs_out[c];
    for (long f = 0; f < resampler_done_frames; f++) {
      buffer[f] = out_interleaved_buffer[(f * stream->out_params.channels) + c] * stream->volume;
    }
    if (resampler_done_frames < resampler_needed_frames) {
      // draining
      for (long f = resampler_done_frames; f < resampler_needed_frames; f++) {
        buffer[f] = 0.f;
      }
    }
    if (resampler_done_frames == 0) {
      // stop, but first zero out the existing buffer
      for (long f = 0; f < resampler_needed_frames; f++) {
        buffer[f] = 0.f;
      }
    }
  }
 }
 if (stream->devs == IN_ONLY || stream->devs == DUPLEX) {
  // convert interleaved input buffers to contiguous buffers
  for (unsigned int c = 0; c < stream->in_params.channels; c++) {
    float* buffer = bufs_in[c];
    for (long f = 0; f < resampler_done_frames; f++) {
      buffer[f] = in_interleaved_buffer[(f * stream->in_params.channels) + c] * stream->volume;
    }
    if (resampler_done_frames < input_frames_count) {
      // draining
      for (long f = resampler_done_frames; f < input_frames_count; f++) {
        buffer[f] = 0.f;
      }
    }
    if (resampler_done_frames == 0) {
      // stop, but first zero out the existing buffer
      for (long f = 0; f < input_frames_count; f++) {
        buffer[f] = 0.f;
      }
    }
  }
 }
  if (resampler_done_frames == 0) {
    // stop stream
    cbjack_stream_stop(stream);
    // set drained state
    stream->state_callback(stream, stream->user_ptr, CUBEB_STATE_DRAINED);
  } else {
    // advance stream position
    stream->position += resampler_done_frames * stream->ratio;
  }
}

/*static*/ int
jack_init (cubeb ** context, char const * context_name)
{
  int r;

  *context = NULL;

  cubeb *ctx = (cubeb*)calloc(1, sizeof(*ctx));
  if (ctx == NULL) {
    return CUBEB_ERROR;
  }

  r = load_jack_lib(ctx);
  if (r != 0) {
    cbjack_destroy(ctx);
    return CUBEB_ERROR;
  }

  ctx->ops = &cbjack_ops;

  const char* jack_client_name = "cubeb";
  if (context_name)
    jack_client_name = context_name;

  ctx->jack_client = api_jack_client_open(jack_client_name,
                                          JackNoStartServer,
                                          NULL);

  if (ctx->jack_client == NULL) {
    cbjack_destroy(ctx);
    return CUBEB_ERROR;
  }

  ctx->jack_xruns = 0;

  api_jack_set_process_callback (ctx->jack_client, cbjack_process, ctx);
  api_jack_set_xrun_callback (ctx->jack_client, cbjack_xrun_callback, ctx);
  api_jack_set_graph_order_callback (ctx->jack_client, cbjack_graph_order_callback, ctx);

  if (api_jack_activate (ctx->jack_client)) {
    cbjack_destroy(ctx);
    return CUBEB_ERROR;
  }

  ctx->jack_sample_rate = api_jack_get_sample_rate(ctx->jack_client);

  ctx->active = true;
  *context = ctx;

  return CUBEB_OK;
}

static char const *
cbjack_get_backend_id(cubeb * context)
{
  return "jack";
}

static int
cbjack_get_max_channel_count(cubeb * ctx, uint32_t * max_channels)
{
  *max_channels = MAX_CHANNELS;
  return CUBEB_OK;
}

static int
cbjack_get_latency(cubeb_stream * stm, unsigned int * latency_ms)
{
  *latency_ms = stm->context->jack_latency;
  return CUBEB_OK;
}

static int
cbjack_get_min_latency(cubeb * ctx, cubeb_stream_params params, uint32_t * latency_ms)
{
  *latency_ms = ctx->jack_latency;
  return CUBEB_OK;
}

static int
cbjack_get_preferred_sample_rate(cubeb * ctx, uint32_t * rate)
{
  if (!ctx->jack_client) {
    jack_client_t *testclient = api_jack_client_open("test-samplerate",
                                                 JackNoStartServer,
                                                 NULL);
    if (!testclient) {
      return CUBEB_ERROR;
    }

    *rate = api_jack_get_sample_rate(testclient);
    api_jack_client_close(testclient);

  } else {
    *rate = api_jack_get_sample_rate(ctx->jack_client);
  }
  return CUBEB_OK;
}

static void
cbjack_destroy(cubeb * context)
{
  context->active = false;

  if (context->jack_client != NULL)
    api_jack_client_close (context->jack_client);

  if (context->libjack)
    dlclose(context->libjack);

  free(context);
}

static cubeb_stream*
context_alloc_stream(cubeb * context, char const * stream_name)
{
  for (int i = 0; i < MAX_STREAMS; i++) {
    if (!context->streams[i].in_use) {
      cubeb_stream * stm = &context->streams[i];
      stm->in_use = true;
      snprintf(stm->stream_name, 255, "%s_%u", stream_name, i);
      return stm;
    }
  }
  return NULL;
}

static int
cbjack_stream_init(cubeb * context, cubeb_stream ** stream, char const * stream_name,
                   cubeb_devid input_device,
                   cubeb_stream_params * input_stream_params,
                   cubeb_devid output_device,
                   cubeb_stream_params * output_stream_params,
                   unsigned int latency,
                   cubeb_data_callback data_callback,
                   cubeb_state_callback state_callback,
                   void * user_ptr)
{
  int stream_actual_rate = 0;
  int jack_rate = api_jack_get_sample_rate(context->jack_client);

  if (output_stream_params
     && (output_stream_params->format != CUBEB_SAMPLE_FLOAT32NE &&
         output_stream_params->format != CUBEB_SAMPLE_S16NE)
     ) {
    return CUBEB_ERROR_INVALID_FORMAT;
  }

  if (input_stream_params
     && (input_stream_params->format != CUBEB_SAMPLE_FLOAT32NE &&
         input_stream_params->format != CUBEB_SAMPLE_S16NE)
     ) {
    return CUBEB_ERROR_INVALID_FORMAT;
  }

  *stream = NULL;

  // Find a free stream.
  cubeb_stream * stm = context_alloc_stream(context, stream_name);

  // No free stream?
  if (stm == NULL) {
    return CUBEB_ERROR;
  }

  stm->ports_ready = false;
  stm->user_ptr = user_ptr;
  stm->context = context;
  stm->devs = NONE;
  if (output_stream_params) {
    stm->out_params = *output_stream_params;
    stream_actual_rate = stm->out_params.rate;
    stm->out_params.rate = jack_rate;
    stm->devs = OUT_ONLY;
    if (stm->out_params.format == CUBEB_SAMPLE_FLOAT32NE) {
      context->output_bytes_per_frame = sizeof(float);
    } else {
      context->output_bytes_per_frame = sizeof(short);
    }
  }
  if (input_stream_params && output_stream_params) {
    stm->in_params = *input_stream_params;
    stm->in_params.rate = stm->out_params.rate;
    stm->devs = DUPLEX;
    if (stm->out_params.format == CUBEB_SAMPLE_FLOAT32NE) {
      context->output_bytes_per_frame = sizeof(float);
      stm->in_params.format = CUBEB_SAMPLE_FLOAT32NE;
    } else {
      context->output_bytes_per_frame = sizeof(short);
      stm->in_params.format = CUBEB_SAMPLE_S16NE;
    }
  } else if (input_stream_params && !output_stream_params) {
    stm->in_params = *input_stream_params;
    stream_actual_rate = stm->in_params.rate;
    stm->in_params.rate = jack_rate;
    stm->devs = IN_ONLY;
    if (stm->in_params.format == CUBEB_SAMPLE_FLOAT32NE) {
      context->output_bytes_per_frame = sizeof(float);
    } else {
      context->output_bytes_per_frame = sizeof(short);
    }
  }

  stm->ratio = (float)stream_actual_rate / (float)jack_rate;

  stm->data_callback = data_callback;
  stm->state_callback = state_callback;
  stm->position = 0;
  stm->volume = 1.0f;
  context->jack_buffer_size = api_jack_get_buffer_size(context->jack_client);
  context->fragment_size = context->jack_buffer_size;

  if (stm->devs == NONE) {
    return CUBEB_ERROR;
  }

  stm->resampler = NULL;

  if (stm->devs == DUPLEX) {
    stm->resampler = cubeb_resampler_create(stm,
                                          &stm->in_params,
                                          &stm->out_params,
                                          stream_actual_rate,
                                          stm->data_callback,
                                          stm->user_ptr,
                                          CUBEB_RESAMPLER_QUALITY_DESKTOP);
  } else if (stm->devs == IN_ONLY) {
    stm->resampler = cubeb_resampler_create(stm,
                                          &stm->in_params,
                                          nullptr,
                                          stream_actual_rate,
                                          stm->data_callback,
                                          stm->user_ptr,
                                          CUBEB_RESAMPLER_QUALITY_DESKTOP);
  } else if (stm->devs == OUT_ONLY) {
    stm->resampler = cubeb_resampler_create(stm,
                                          nullptr,
                                          &stm->out_params,
                                          stream_actual_rate,
                                          stm->data_callback,
                                          stm->user_ptr,
                                          CUBEB_RESAMPLER_QUALITY_DESKTOP);
  }

  if (!stm->resampler) {
    stm->in_use = false;
    return CUBEB_ERROR;
  }

  for (unsigned int c = 0; c < stm->out_params.channels; c++) {
    char portname[256];
    snprintf(portname, 255, "%s_out_%d", stm->stream_name, c);
    stm->output_ports[c] = api_jack_port_register(stm->context->jack_client,
                                                  portname,
                                                  JACK_DEFAULT_AUDIO_TYPE,
                                                  JackPortIsOutput,
                                                  0);
  }

  for (unsigned int c = 0; c < stm->in_params.channels; c++) {
    char portname[256];
    snprintf(portname, 255, "%s_in_%d", stm->stream_name, c);
    stm->input_ports[c] = api_jack_port_register(stm->context->jack_client,
                                                  portname,
                                                  JACK_DEFAULT_AUDIO_TYPE,
                                                  JackPortIsInput,
                                                  0);
  }

  cbjack_connect_ports(stm);

  stm->ports_ready = true;
  stm->pause = true;

  *stream = stm;

  return CUBEB_OK;
}

static void
cbjack_stream_destroy(cubeb_stream * stream)
{
  stream->ports_ready = false;

  for (unsigned int c = 0; c < stream->out_params.channels; c++) {
    if (stream->output_ports[c]) {
      api_jack_port_unregister (stream->context->jack_client, stream->output_ports[c]);
      stream->output_ports[c] = NULL;
    }
  }

  for (unsigned int c = 0; c < stream->in_params.channels; c++) {
    if (stream->input_ports[c]) {
      api_jack_port_unregister (stream->context->jack_client, stream->input_ports[c]);
      stream->input_ports[c] = NULL;
    }
  }

  if (stream->resampler) {
    cubeb_resampler_destroy(stream->resampler);
    stream->resampler = NULL;
  }
  stream->in_use = false;
}

static int
cbjack_stream_start(cubeb_stream * stream)
{
  stream->pause = false;
  stream->state_callback(stream, stream->user_ptr, CUBEB_STATE_STARTED);
  return CUBEB_OK;
}

static int
cbjack_stream_stop(cubeb_stream * stream)
{
  stream->pause = true;
  stream->state_callback(stream, stream->user_ptr, CUBEB_STATE_STOPPED);
  return CUBEB_OK;
}

static int
cbjack_stream_get_position(cubeb_stream * stream, uint64_t * position)
{
  *position = stream->position;
  return CUBEB_OK;
}

static int
cbjack_stream_set_volume(cubeb_stream * stm, float volume)
{
  stm->volume = volume;
  return CUBEB_OK;
}


static int
cbjack_stream_get_current_device(cubeb_stream * stm, cubeb_device ** const device)
{
  *device = (cubeb_device *)calloc(1, sizeof(cubeb_device));
  if (*device == NULL)
    return CUBEB_ERROR;

  const char *j_in = "JACK capture";
  const char *j_out = "JACK playback";
  const char *empty = "";

  if (stm->devs == DUPLEX) {
    (*device)->input_name = strdup(j_in);
    (*device)->output_name = strdup(j_out);
  } else if (stm->devs == IN_ONLY) {
    (*device)->input_name = strdup(j_in);
    (*device)->output_name = strdup(empty);
  } else if (stm->devs == OUT_ONLY) {
    (*device)->input_name = strdup(empty);
    (*device)->output_name = strdup(j_out);
  }

  return CUBEB_OK;
}

static int
cbjack_stream_device_destroy(cubeb_stream * stream,
                             cubeb_device * device)
{
  if (device->input_name)
    free(device->input_name);
  if (device->output_name)
    free(device->output_name);
  free(device);
  return CUBEB_OK;
}

static int
cbjack_enumerate_devices(cubeb * context, cubeb_device_type type,
                         cubeb_device_collection ** collection)
{
  if (!context)
    return CUBEB_ERROR;

  uint32_t rate;
  cbjack_get_preferred_sample_rate(context, &rate);
  const char *j_in = "JACK capture";
  const char *j_out = "JACK playback";

  context->devinfo = (cubeb_device_info *)malloc(sizeof(cubeb_device_info));
  if (type & CUBEB_DEVICE_TYPE_OUTPUT) {
    context->devinfo->device_id = strdup(j_out);
    context->devinfo->devid = context->devinfo->device_id;
    context->devinfo->friendly_name = strdup(j_out);
    context->devinfo->group_id = strdup(j_out);
    context->devinfo->vendor_name = strdup(j_out);
    context->devinfo->type = CUBEB_DEVICE_TYPE_OUTPUT;
    context->devinfo->state = CUBEB_DEVICE_STATE_ENABLED;
    context->devinfo->preferred = CUBEB_DEVICE_PREF_ALL;
    context->devinfo->format = CUBEB_DEVICE_FMT_F32NE;
    context->devinfo->default_format = CUBEB_DEVICE_FMT_F32NE;
    context->devinfo->max_channels = MAX_CHANNELS;
    context->devinfo->min_rate = rate;
    context->devinfo->max_rate = rate;
    context->devinfo->default_rate = rate;
    context->devinfo->latency_lo_ms = 0;
    context->devinfo->latency_hi_ms = 10;
  } else if (type & CUBEB_DEVICE_TYPE_INPUT) {
    context->devinfo->device_id = strdup(j_in);
    context->devinfo->devid = context->devinfo->device_id;
    context->devinfo->friendly_name = strdup(j_in);
    context->devinfo->group_id = strdup(j_in);
    context->devinfo->vendor_name = strdup(j_in);
    context->devinfo->type = CUBEB_DEVICE_TYPE_INPUT;
    context->devinfo->state = CUBEB_DEVICE_STATE_ENABLED;
    context->devinfo->preferred = CUBEB_DEVICE_PREF_ALL;
    context->devinfo->format = CUBEB_DEVICE_FMT_F32NE;
    context->devinfo->default_format = CUBEB_DEVICE_FMT_F32NE;
    context->devinfo->max_channels = MAX_CHANNELS;
    context->devinfo->min_rate = rate;
    context->devinfo->max_rate = rate;
    context->devinfo->default_rate = rate;
    context->devinfo->latency_lo_ms = 0;
    context->devinfo->latency_hi_ms = 10;
  }
  *collection = (cubeb_device_collection *)
                 malloc(sizeof(cubeb_device_collection) +
                        sizeof(cubeb_device_info *));

  (*collection)->count = 1;

  (*collection)->device[0] = context->devinfo;

  return CUBEB_OK;
}
