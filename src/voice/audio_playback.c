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

/* audio_playback.c — Streaming audio playback via media_player buffer mode.
 *
 * Uses media_player_write_data() for synchronous buffer-mode playback.
 * Each write_data() call blocks until the media framework consumes the
 * buffer, so TTS PCM chunks are played as they arrive — no temp file. */

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
    unsigned int sample_rate;
    unsigned int channels;
    unsigned int bits_per_sample;
    volatile int stopped; /* set by audio_playback_stop() from another thread */
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
        /* Let the media framework finish internal cleanup before
         * opening a new player.  Without this delay, the next
         * media_player_open may see stale state and request a
         * bogus allocation (observed: 1.3GB → assert crash). */
        usleep(100000); /* 100ms */
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

    /* Buffer mode: url=NULL, caller provides data via write_data() */
    int ret = media_player_prepare(player, NULL, opts);

    if (ret < 0) {
        syslog(LOG_ERR, "[%s] prepare failed: %d\n", TAG, ret);
        media_player_close(player, 0);
        return NULL;
    }

    ret = media_player_start(player);
    if (ret < 0) {
        syslog(LOG_ERR, "[%s] start failed: %d\n", TAG, ret);
        media_player_close(player, 0);
        return NULL;
    }

    audio_playback_t* pb = calloc(1, sizeof(*pb));

    if (!pb) {
        media_player_close(player, 0);
        return NULL;
    }

    pb->player = player;
    pb->sample_rate = sample_rate;
    pb->channels = channels;
    pb->bits_per_sample = bits_per_sample;
    pb->stopped = 0;
    s_active_player = player;

    syslog(LOG_INFO, "[%s] opened (%uHz %uch %ubit) buffer mode\n",
        TAG, sample_rate, channels, bits_per_sample);
    return pb;
}

int audio_playback_write(audio_playback_t* pb,
    const void* buf, size_t len)
{
    if (!pb || !pb->player || !buf || len == 0) {
        return -EINVAL;
    }

    /* AEC workaround: voice_channel_start() sets stopped to skip
     * remaining TTS chunks and prevent echo into the microphone. */
    if (pb->stopped) {
        return -ECANCELED;
    }

    ssize_t n = media_player_write_data(pb->player, buf, len);

    if (n > 0) {
        pb->total_written += (size_t)n;
    } else {
        syslog(LOG_ERR, "[%s] write_data(%zu) failed: %zd\n",
            TAG, len, n);
    }

    return (int)n;
}

void audio_playback_stop(audio_playback_t* pb)
{
    if (pb) {
        pb->stopped = 1;
        /* Force-stop the media player so write_data() unblocks
         * immediately if it is currently blocking in the audio
         * driver.  media_player_stop is safe to call from any
         * thread on NuttX. */
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
        syslog(LOG_INFO, "[%s] closing (%zu bytes written)\n",
            TAG, pb->total_written);

        /* Always stop before close to unblock the AP-side media
         * player thread.  Without this, media_player_close with
         * pending_stop=1 waits for the AP reader to drain, but
         * the reader is blocked on rpmsg waiting for data that
         * will never come → watchdog fires after ~2 minutes. */
        media_player_stop(pb->player);
        usleep(50 * 1000); /* let AP-side process the stop */
        media_player_close(pb->player, 0);

        s_active_player = NULL;
        syslog(LOG_INFO, "[%s] playback done\n", TAG);
    }

    free(pb);
}
