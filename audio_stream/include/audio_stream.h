#pragma once

/*
 * Stream a microphone somewhere else: pick channels and sample width, convert, and hand
 * interleaved little-endian PCM to a sink -- USB (audio_stream_usb), an HTTP response
 * (audio_stream_http), or one you write.
 *
 * Three layers, so a project takes only what it needs:
 *
 *   FORMAT  Plain C with no hardware and a host test. What a request means against what
 *           the microphone offers (audio_stream_resolve()), the "0,1,3" channel list,
 *           sample conversion, the WAV header. A stream that is one rate off does not fail
 *           loudly -- it plays at the wrong pitch -- so these are checked off-target.
 *   RUNNER  A task that reads the source, decimates a raw PDM source, converts, and calls
 *           the sink. One stream at a time (see audio_stream_start()).
 *   QUEUE   audio_stream_queue.h: a buffer and a draining task between the runner and a
 *           sink that can block, such as a TCP socket.
 *
 * THE SOURCE IS YOURS. The only seam to hardware is audio_stream_source_t: a description
 * of what the open microphone produces and a read function. This component never opens,
 * closes or reconfigures a microphone, and does not know what an I2S driver is.
 *
 * THE RATE IS THE SOURCE'S. Nothing here resamples. Choosing another rate is reopening
 * the microphone at it, which is why audio_stream_resolve() validates a requested rate
 * rather than applying it. The one exception is a raw PDM source, which has no rate until
 * a decimation ratio is chosen: there the rate is the clock over a ratio.
 *
 * NO PROSE. Every failure is an error code plus the facts in audio_stream_why_t; the
 * application words them.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define AUDIO_STREAM_ERR_BASE         0x43000
/* The source description is not a stream (no rate, no channels, a slot width that is not
 * 16 or 32 bits, a raw PDM source with no clock). */
#define AUDIO_STREAM_ERR_NO_SOURCE    (AUDIO_STREAM_ERR_BASE + 1)
/* The requested rate is not one the source offers. why->src_rate, why->raw_rates. */
#define AUDIO_STREAM_ERR_BAD_RATE     (AUDIO_STREAM_ERR_BASE + 2)
/* A requested channel does not exist. why->channel, why->channels_available. */
#define AUDIO_STREAM_ERR_BAD_CHANNEL  (AUDIO_STREAM_ERR_BASE + 3)
/* Sample width other than 16 or 24, or 24 from a source with fewer data bits.
 * why->src_bits. */
#define AUDIO_STREAM_ERR_BAD_BITS     (AUDIO_STREAM_ERR_BASE + 4)
/* A channel list that is not "n,n,n". */
#define AUDIO_STREAM_ERR_BAD_LIST     (AUDIO_STREAM_ERR_BASE + 5)
/* A channel named twice. why->channel. */
#define AUDIO_STREAM_ERR_DUP_CHANNEL  (AUDIO_STREAM_ERR_BASE + 6)
/* A stream is already running. audio_stream_owner() says whose. */
#define AUDIO_STREAM_ERR_BUSY         (AUDIO_STREAM_ERR_BASE + 7)

#define AUDIO_STREAM_MAX_CHANNELS     16

/* ---- The source ---------------------------------------------------------------------- */

typedef struct {
    /* Frames per second. 0 for a raw PDM source, which has none yet. */
    uint32_t rate_hz;
    /* Slots per frame, and bytes per slot: 2 or 4 for PCM, 2 for raw PDM (one 16-bit word
     * of the bit stream per channel). */
    uint8_t channels;
    uint8_t slot_bytes;
    /* Valid data bits in a slot, left-justified: 16, 24 or 32. */
    uint8_t data_bits;

    /* An undecimated PDM bit stream. The runner decimates it, with pdm_decimator, so the
     * output rate is chosen as pdm_clk_hz over a ratio. channels must then be at most
     * PDM_DECIMATOR_MAX_CHANNELS. */
    bool pdm_raw;
    uint32_t pdm_clk_hz;
    /* Which end of each raw 16-bit word is the oldest bit. False: the most significant. */
    bool pdm_lsb_first;

    /* Fill buf with up to `len` bytes, a whole number of frames. `got` is the number of
     * bytes delivered. Anything but ESP_OK, or zero bytes, counts as a read error. */
    esp_err_t (*read)(void *ctx, void *buf, size_t len, size_t *got, uint32_t timeout_ms);
    /* Optional. False when the microphone has gone (closed, or reopened as another
     * format): the stream ends with AUDIO_STREAM_END_SOURCE_GONE. */
    bool (*alive)(void *ctx);
    void *ctx;
} audio_stream_source_t;

/* True when the description is a stream. */
bool audio_stream_source_valid(const audio_stream_source_t *src);

/* ---- A request, and what it resolves to ---------------------------------------------- */

/* What the user asked for. A zero field means "follow the source". */
typedef struct {
    uint32_t rate;                              /* 0, or the rate the source must have */
    uint8_t bits;                               /* 0 (16), 16 or 24 */
    int nch;                                    /* 0 (all channels), or the length of chan[] */
    uint8_t chan[AUDIO_STREAM_MAX_CHANNELS];    /* source channels, in output order */
} audio_stream_want_t;

typedef struct {
    uint32_t rate_hz;
    uint8_t channels;                           /* output channels: 1..AUDIO_STREAM_MAX_CHANNELS */
    uint8_t bytes;                              /* per output sample: 2 or 3 */
    uint8_t chan[AUDIO_STREAM_MAX_CHANNELS];    /* which source channel feeds each output one */
    int ratio;                                  /* raw PDM: bit clocks per output sample, else 0 */
} audio_stream_fmt_t;

/* Facts about a refusal. Which fields are meaningful is listed beside each error. */
typedef struct {
    esp_err_t code;
    uint32_t src_rate;
    uint32_t want_rate;
    int channel;
    int channels_available;
    uint8_t src_bits;
    /* The rates a raw PDM source offers, in the order of audio_stream_raw_ratios(). */
    uint32_t raw_rates[4];
} audio_stream_why_t;

/* The decimation ratios a raw PDM source can be streamed at, bit clocks per output
 * sample: 64, 48, 32, 16. 64 is the standard PDM oversampling and the default, so a
 * 3.072 MHz clock gives 48 kHz. */
const int *audio_stream_raw_ratios(size_t *count);

/* Parses "0,1,3" into out[] (room for AUDIO_STREAM_MAX_CHANNELS), checking each against
 * `nch_available`. ESP_OK with *count set, or AUDIO_STREAM_ERR_BAD_LIST,
 * AUDIO_STREAM_ERR_BAD_CHANNEL or AUDIO_STREAM_ERR_DUP_CHANNEL; `why` may be NULL. */
esp_err_t audio_stream_parse_channels(const char *text, int nch_available, uint8_t *out,
                                      int *count, audio_stream_why_t *why);

/* Turns a request and a source into a format, or says why not. `why` may be NULL and is
 * filled on every call: code is ESP_OK on success. Does not touch hardware. */
esp_err_t audio_stream_resolve(const audio_stream_source_t *src, const audio_stream_want_t *want,
                               audio_stream_fmt_t *fmt, audio_stream_why_t *why);

/* ---- Conversion and framing ---------------------------------------------------------- */

/*
 * Selects fmt->chan[] from `frames` source frames of `src_channels` slots, each
 * `slot_bytes` wide (2 or 4, data left-justified), and writes interleaved little-endian
 * 16- or 24-bit samples. 32-bit slots keep their top 16 or 24 bits; a 16-bit slot given
 * 24-bit output is left-justified with a zero low byte. Returns the bytes written:
 * frames * fmt->channels * fmt->bytes. `out` must have room for that.
 */
size_t audio_stream_convert(const audio_stream_fmt_t *fmt, size_t slot_bytes, int src_channels,
                            const void *in, size_t frames, uint8_t *out);

/* Large enough for audio_stream_wav_header(). */
#define AUDIO_STREAM_WAV_HEADER_MAX   68

/*
 * The header of a WAV file of unknown length, for a stream: the RIFF and data sizes are
 * 0xFFFFFFFF, which ffmpeg, sox, VLC and browsers read as "until the end". Plain PCM for
 * mono or stereo 16-bit; WAVE_FORMAT_EXTENSIBLE otherwise, which is what players need
 * to accept more than two channels or 24-bit samples. Returns the header's length.
 */
size_t audio_stream_wav_header(const audio_stream_fmt_t *fmt, uint8_t *buf);

/* ---- The runner ---------------------------------------------------------------------- */

typedef enum {
    AUDIO_STREAM_END_STOPPED,        /* audio_stream_stop() */
    AUDIO_STREAM_END_SOURCE_GONE,    /* source.alive() said no */
    AUDIO_STREAM_END_READ_ERRORS,    /* the source stopped delivering (20 failures in a row) */
    AUDIO_STREAM_END_SINK_GONE,      /* sink.write() said no */
} audio_stream_end_t;

typedef struct {
    /* Optional. Called once from audio_stream_start(), after the buffers exist and before
     * the task does: bring the sink up here, and fail if it cannot come up. Success means
     * end() will be called, exactly once, however the stream finishes. */
    esp_err_t (*begin)(void *ctx, const audio_stream_fmt_t *fmt);
    /* Called from the stream task with interleaved PCM, a whole number of frames. Must not
     * block for long: the source is a receiver that overruns. Return false when the sink
     * is gone, which ends the stream. */
    bool (*write)(void *ctx, const uint8_t *pcm, size_t len);
    /* Optional. Called once, from the stream task, when it has finished. Do not call
     * audio_stream_stop() from here or from write(): it waits for this task. */
    void (*end)(void *ctx, audio_stream_end_t why);
    /*
     * What happens to the stream's place when it ends by itself. True: it is free at
     * once, so the next audio_stream_start() succeeds (a network client that went away).
     * False: the stream stays reserved until the owner calls audio_stream_stop() (a
     * device that is still up, and would otherwise be left with nobody feeding it).
     */
    bool release_on_end;
} audio_stream_sink_t;

typedef struct {
    const char *owner;     /* a string that lives forever; audio_stream_owner() returns it */
    int core;              /* -1 for any; clamped to -1 on a single-core chip */
    unsigned priority;     /* 0 -> 6 */
    unsigned block_ms;     /* audio per read and per sink write; 0 -> 5 */
} audio_stream_options_t;

/*
 * Start streaming `src`, in the format audio_stream_resolve() produced, to `sink`.
 *
 * ONE STREAM AT A TIME. Two readers of one receiver would each get alternate blocks, so
 * a second start returns AUDIO_STREAM_ERR_BUSY and audio_stream_owner() names the first.
 * It is callable from any task.
 *
 * The source description is copied; `src->ctx` and `sink_ctx` must outlive the stream.
 * Keep the microphone open and unchanged until the stream has ended.
 *
 * Errors: ESP_ERR_INVALID_ARG, AUDIO_STREAM_ERR_BUSY, ESP_ERR_NO_MEM, the decimator's
 * error for a raw source, or whatever sink.begin() returned.
 */
esp_err_t audio_stream_start(const audio_stream_source_t *src, const audio_stream_fmt_t *fmt,
                             const audio_stream_sink_t *sink, void *sink_ctx,
                             const audio_stream_options_t *opt);

/* End the stream and wait for the task, then free its place. ESP_ERR_INVALID_STATE when
 * there is nothing to stop. Succeeds on a stream that already ended by itself. */
esp_err_t audio_stream_stop(void);

/* Who started the stream occupying the place, or NULL when it is free. */
const char *audio_stream_owner(void);

typedef struct {
    bool running;                  /* the task is alive */
    bool has_ended;                /* `ended` is valid: a stream has finished since the last start */
    audio_stream_end_t ended;
    const char *owner;             /* of the current or most recent stream */
    uint32_t rate_hz;
    uint8_t channels, bytes;       /* the output format */
    uint64_t frames;               /* output frames handed to the sink */
    uint32_t read_errors;
    /* Raw PDM only: CPU cycles spent decimating, and the output frames that made. Divide
     * for cycles per frame, then by the CPU clock for load. */
    uint64_t dec_cycles, dec_frames;
} audio_stream_stats_t;

void audio_stream_get_stats(audio_stream_stats_t *out);

#ifdef __cplusplus
}
#endif
