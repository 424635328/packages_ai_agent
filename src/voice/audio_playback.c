/*
 * Copyright (C) 2026 Xiaomi Corporation
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

/* audio_playback.c - Streaming audio playback via media_player buffer mode. */

#include "voice/audio_playback.h"
#include "agent_config.h"

#include <errno.h>
#include <media_player.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <syslog.h>
#include <unistd.h>

static const char* TAG = "audio_pb";

#define PB_OPTIONS_LEN 128

static void* s_active_player;

struct audio_playback {
    void* player;
    size_t total_written;
    int bytes_per_frame;
    size_t preroll_bytes;   /* PCM buffered before the player starts */
    volatile int stopped;
    int started;            /* media_player_start() not yet issued */
};

audio_playback_t* audio_playback_open(const char* dev_path,
    unsigned int sample_rate, unsigned int channels,
    unsigned int bits_per_sample)
{
    (void)dev_path;

    if (s_active_player) {
        syslog(LOG_WARNING, "[%s] force closing stale player\n", TAG);
        media_player_stop(s_active_player);
        media_player_close(s_active_player, 0);
        s_active_player = NULL;
        usleep(100000);
    }

    void* player = media_player_open(MEDIA_STREAM_MUSIC);
    if (!player) {
        syslog(LOG_ERR, "[%s] media_player_open failed\n", TAG);
        return NULL;
    }

    char opts[PB_OPTIONS_LEN];
    snprintf(opts, sizeof(opts),
        "format=s%ule:sample_rate=%u:ch_layout=%s",
        bits_per_sample, sample_rate,
        (channels == 1) ? "mono" : "stereo");

    int ret = media_player_prepare(player, NULL, opts);
    if (ret < 0) {
        syslog(LOG_ERR, "[%s] prepare failed: %d\n", TAG, ret);
        media_player_close(player, 0);
        return NULL;
    }

    audio_playback_t* pb = calloc(1, sizeof(*pb));
    if (!pb) {
        media_player_close(player, 0);
        return NULL;
    }

    pb->player = player;
    pb->bytes_per_frame = (bits_per_sample / 8) * channels;
    pb->preroll_bytes = (size_t)AGENT_TTS_PREROLL_MS *
        (size_t)sample_rate * (size_t)pb->bytes_per_frame / 1000u;
    s_active_player = player;

    /* Do not start yet: the first AGENT_TTS_PREROLL_MS of PCM is buffered
     * before the player goes live (see audio_playback_write), so a jittery
     * network cannot drain the queue and cause 'playback -> silence ->
     * playback' underruns at the start of the sentence. */

    syslog(LOG_INFO, "[%s] opened (%uHz %uch %ubit), "
        "preroll=%zu bytes before start\n",
        TAG, sample_rate, channels, bits_per_sample, pb->preroll_bytes);
    return pb;
}

int audio_playback_write(audio_playback_t* pb, const void* buf, size_t len)
{
    if (!pb || !pb->player || !buf || len == 0) {
        return -EINVAL;
    }

    if (pb->stopped) {
        return -ECANCELED;
    }

    ssize_t n = media_player_write_data(pb->player, buf, len);
    if (n <= 0) {
        return (int)n;
    }

    pb->total_written += (size_t)n;

    /* Preroll: hold off media_player_start until the first chunk(s) have
     * filled AGENT_TTS_PREROLL_MS of PCM.  Unless this is done, the very
     * first bytes are written while the server's queue is (nearly) empty;
     * a network stall in the first 200ms of the sentence then drains the
     * queue and the output goes 'silence -> gap -> continue'.  Once the
     * threshold is crossed we start exactly once; the framework's own
     * start_audio threshold (queue full) then aligns with real playback. */

    if (!pb->started && pb->total_written >= pb->preroll_bytes) {
        int ret = media_player_start(pb->player);
        if (ret < 0) {
            syslog(LOG_ERR, "[%s] start failed at preroll: %d\n", TAG, ret);
            return ret;
        }

        pb->started = 1;
        syslog(LOG_INFO, "[%s] player started after %zu bytes "
            "(%.0f ms)\n", TAG, pb->total_written,
            (double)pb->total_written * 1000.0 /
            ((double)pb->bytes_per_frame *
             (double)AGENT_TTS_WS_SAMPLE_RATE));
    }

    return (int)n;
}

void audio_playback_stop(audio_playback_t* pb)
{
    if (pb) {
        pb->stopped = 1;
        if (pb->player) {
            media_player_stop(pb->player);
        }
    }
}

void audio_playback_close(audio_playback_t* pb)
{
    if (!pb) {
        return;
    }

    if (pb->player) {
        unsigned int bpf = pb->bytes_per_frame ? pb->bytes_per_frame : 2;
        double secs = (double)pb->total_written /
                      ((double)bpf * (double)AGENT_TTS_WS_SAMPLE_RATE);
        syslog(LOG_INFO, "[%s] closing (%zu bytes, %.2fs @ %u Hz%s)\n",
            TAG, pb->total_written, secs, AGENT_TTS_WS_SAMPLE_RATE,
            pb->started ? "" : ", never started (data < preroll)");
        media_player_stop(pb->player);
        usleep(50 * 1000);
        media_player_close(pb->player, 0);
        s_active_player = NULL;
    }

    free(pb);
}
