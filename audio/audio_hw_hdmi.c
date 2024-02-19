/*
 * Copyright (C) 2016 The Android Open Source Project
 * Copyright (C) 2021-2023 KonstaKANG
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

#define LOG_TAG "audio_hw_rpi_hdmi"
//#define LOG_NDEBUG 0

#include <errno.h>
#include <malloc.h>
#include <pthread.h>
#include <stdint.h>
#include <sys/time.h>
#include <stdlib.h>
#include <unistd.h>

#include <log/log.h>
#include <cutils/str_parms.h>
#include <cutils/properties.h>

#include <hardware/hardware.h>
#include <system/audio.h>
#include <hardware/audio.h>

#include <alsa/asoundlib.h>
#include <audio_utils/resampler.h>
#include <audio_utils/echo_reference.h>
#include <hardware/audio_effect.h>
#include <audio_effects/effect_aec.h>


/* Minimum granularity - Arbitrary but small value */
#define CODEC_BASE_FRAME_COUNT 32

/* number of base blocks in a short period (low latency) */
#define PERIOD_MULTIPLIER 32  /* 21 ms */
/* number of frames per short period (low latency) */
#define PERIOD_SIZE (CODEC_BASE_FRAME_COUNT * PERIOD_MULTIPLIER)
/* number of pseudo periods for low latency playback */
#define PLAYBACK_PERIOD_COUNT 4
#define PLAYBACK_PERIOD_START_THRESHOLD 2
#define CODEC_SAMPLING_RATE 48000
#define CHANNEL_STEREO 2
#define MIN_WRITE_SLEEP_US      5000

struct stub_stream_in {
    struct audio_stream_in stream;
};

struct alsa_audio_device {
    struct audio_hw_device hw_device;

    pthread_mutex_t lock;   /* see note below on mutex acquisition order */
    int devices;
    struct alsa_stream_in *active_input;
    struct alsa_stream_out *active_output;
    bool mic_mute;
};

struct alsa_stream_out {
    struct audio_stream_out stream;
    struct alsa_audio_device *dev;

    pthread_mutex_t lock;   /* see note below on mutex acquisition order */
    snd_pcm_t *pcm;

    snd_pcm_uframes_t period_size;
    unsigned int periods;
    snd_pcm_uframes_t buffer_size;

    bool unavailable;
    int standby;
    snd_pcm_uframes_t written;
};

static void get_alsa_device_name(char *name) {
    char hdmi_device[PROPERTY_VALUE_MAX];
    property_get("persist.audio.hdmi.device", hdmi_device, "vc4hdmi0");

    // use card configured in vc4-hdmi.conf to get IEC958 subframe conversion
    sprintf(name, "default:CARD=%s", hdmi_device);
}

/* must be called with hw device and output stream mutexes locked */
static int start_output_stream(struct alsa_stream_out *out)
{
    struct alsa_audio_device *adev = out->dev;

    if (out->unavailable)
        return -ENODEV;

    char device_name[PROPERTY_VALUE_MAX];
    get_alsa_device_name(device_name);
    ALOGI("start_output_stream: %s", device_name);

    int r;
    snd_pcm_t *pcm;

    if ((r = snd_pcm_open(&pcm, device_name, SND_PCM_STREAM_PLAYBACK, 0) < 0)) {
        ALOGE("cannot open pcm_out driver: %s", snd_strerror(r));
        adev->active_output = NULL;
        out->unavailable = true;
        return -ENODEV;
    }
    out->pcm = pcm;

    snd_pcm_hw_params_t *hwp;
    snd_pcm_hw_params_alloca(&hwp);
    snd_pcm_hw_params_any(pcm, hwp);
    snd_pcm_hw_params_set_access(pcm, hwp, SND_PCM_ACCESS_RW_INTERLEAVED);
    snd_pcm_hw_params_set_format(pcm, hwp, SND_PCM_FORMAT_S16_LE);
    snd_pcm_hw_params_set_rate(pcm, hwp, CODEC_SAMPLING_RATE, 0);
    snd_pcm_hw_params_set_channels(pcm, hwp, CHANNEL_STEREO);

    // Configurue period_size, periods and buffer_size
    int dir = 0;
    out->period_size = PERIOD_SIZE;
    if ((r = snd_pcm_hw_params_set_period_size_near(pcm, hwp, &out->period_size, &dir)) < 0) {
        ALOGE("cannot snd_pcm_hw_params_set_period_size_near: %s", snd_strerror(r));
        adev->active_output = NULL;
        out->unavailable = true;
        return -ENODEV;
    }
    dir = 0;
    out->periods = PLAYBACK_PERIOD_COUNT;
    if ((r = snd_pcm_hw_params_set_periods_near(pcm, hwp, &out->periods, &dir)) < 0) {
        ALOGE("cannot snd_pcm_hw_params_set_periods_near: %s", snd_strerror(r));
        adev->active_output = NULL;
        out->unavailable = true;
        return -ENODEV;
    }
    out->buffer_size = out->period_size * out->periods;
    if ((r = snd_pcm_hw_params_set_buffer_size_near(pcm, hwp, &out->buffer_size)) < 0) {
        ALOGE("cannot snd_pcm_hw_params_set_buffer_size_near: %s", snd_strerror(r));
        adev->active_output = NULL;
        out->unavailable = true;
        return -ENODEV;
    }

    //write the hw params
    if ((r = snd_pcm_hw_params(pcm, hwp)) < 0) {
        ALOGE("cannot snd_pcm_hw_params: %s", snd_strerror(r));
        adev->active_output = NULL;
        out->unavailable = true;
        return -ENODEV;
    }

    //Software parameters
    snd_pcm_sw_params_t *swp;
    snd_pcm_sw_params_alloca(&swp);
    snd_pcm_sw_params_current(pcm, swp);

    // set avail_min to period_size
    if ((r = snd_pcm_sw_params_set_avail_min(pcm, swp, out->period_size)) < 0) {
        ALOGE("cannot snd_pcm_sw_params_set_avail_min: %s", snd_strerror(r));
        adev->active_output = NULL;
        out->unavailable = true;
        return -ENODEV;
    }
    // set start_threshold to period_size * PLAYBACK_PERIOD_START_THRESHOLD
    if ((r = snd_pcm_sw_params_set_start_threshold(pcm, swp, out->period_size * PLAYBACK_PERIOD_START_THRESHOLD)) < 0) {
        ALOGE("cannot snd_pcm_sw_params_set_start_threshold: %s", snd_strerror(r));
        adev->active_output = NULL;
        out->unavailable = true;
        return -ENODEV;
    }
    //write the sw params
    if ((r = snd_pcm_sw_params(pcm, swp)) < 0) {
        ALOGE("cannot snd_pcm_sw_params: %s", snd_strerror(r));
        adev->active_output = NULL;
        out->unavailable = true;
        return -ENODEV;
    }

    // prepare
    if ((r = snd_pcm_prepare(pcm)) < 0) {
        ALOGE("cannot snd_pcm_prepare: %s", snd_strerror(r));
        adev->active_output = NULL;
        out->unavailable = true;
        return -ENODEV;
    }

    adev->active_output = out;
    return 0;
}

static uint32_t out_get_sample_rate(const struct audio_stream *stream)
{
    ALOGV("out_get_sample_rate: %d", CODEC_SAMPLING_RATE);
    return CODEC_SAMPLING_RATE;
}

static int out_set_sample_rate(struct audio_stream *stream, uint32_t rate)
{
    ALOGV("out_set_sample_rate: %d", 0);
    return -ENOSYS;
}

static size_t out_get_buffer_size(const struct audio_stream *stream)
{
    struct alsa_stream_out *out = (struct alsa_stream_out *)stream;

    /* return the closest majoring multiple of 16 frames, as
     * audioflinger expects audio buffers to be a multiple of 16 frames */
    size_t size = out->period_size;
    size = ((size + 15) / 16) * 16;
    ALOGV("out_get_buffer_size: %ld", (long int)size);
    return size * audio_stream_out_frame_size((struct audio_stream_out *)stream);
}

static audio_channel_mask_t out_get_channels(const struct audio_stream *stream)
{
    ALOGV("out_get_channels: %d", CHANNEL_STEREO);
    return audio_channel_out_mask_from_count(CHANNEL_STEREO);
}

static audio_format_t out_get_format(const struct audio_stream *stream)
{
    ALOGV("out_get_format: %d", AUDIO_FORMAT_PCM_16_BIT);
    return AUDIO_FORMAT_PCM_16_BIT;
}

static int out_set_format(struct audio_stream *stream, audio_format_t format)
{
    ALOGV("out_set_format: %d",format);
    return -ENOSYS;
}

static int do_output_standby(struct alsa_stream_out *out)
{
    struct alsa_audio_device *adev = out->dev;

    if (!out->standby) {
        snd_pcm_close(out->pcm);
        out->pcm = NULL;
        adev->active_output = NULL;
        out->standby = 1;
    }
    return 0;
}

static int out_standby(struct audio_stream *stream)
{
    ALOGV("out_standby");
    struct alsa_stream_out *out = (struct alsa_stream_out *)stream;
    int status;

    pthread_mutex_lock(&out->dev->lock);
    pthread_mutex_lock(&out->lock);
    status = do_output_standby(out);
    pthread_mutex_unlock(&out->lock);
    pthread_mutex_unlock(&out->dev->lock);
    return status;
}

static int out_dump(const struct audio_stream *stream, int fd)
{
    ALOGV("out_dump");
    return 0;
}

static int out_set_parameters(struct audio_stream *stream, const char *kvpairs)
{
    ALOGV("out_set_parameters");
    struct alsa_stream_out *out = (struct alsa_stream_out *)stream;
    struct alsa_audio_device *adev = out->dev;
    struct str_parms *parms;
    char value[32];
    int val = 0;
    int ret = -EINVAL;

    if (kvpairs == NULL || kvpairs[0] == 0) {
        return 0;
    }

    parms = str_parms_create_str(kvpairs);

    if (str_parms_get_str(parms, AUDIO_PARAMETER_STREAM_ROUTING, value, sizeof(value)) >= 0) {
        val = atoi(value);
        pthread_mutex_lock(&adev->lock);
        pthread_mutex_lock(&out->lock);
        if (((adev->devices & AUDIO_DEVICE_OUT_ALL) != val) && (val != 0)) {
            adev->devices &= ~AUDIO_DEVICE_OUT_ALL;
            adev->devices |= val;
        }
        pthread_mutex_unlock(&out->lock);
        pthread_mutex_unlock(&adev->lock);
        ret = 0;
    }

    str_parms_destroy(parms);
    return ret;
}

static char * out_get_parameters(const struct audio_stream *stream, const char *keys)
{
    ALOGV("out_get_parameters");
    return strdup("");
}

static uint32_t out_get_latency(const struct audio_stream_out *stream)
{
    ALOGV("out_get_latency");
    struct alsa_stream_out *out = (struct alsa_stream_out *)stream;
    // latency = buffer_size / rate
    return (out->buffer_size * 1000) / CODEC_SAMPLING_RATE;
}

static int out_set_volume(struct audio_stream_out *stream, float left,
        float right)
{
    ALOGV("out_set_volume: Left:%f Right:%f", left, right);
    return 0;
}

static ssize_t out_write(struct audio_stream_out *stream, const void* buffer,
        size_t bytes)
{
    int ret;
    struct alsa_stream_out *out = (struct alsa_stream_out *)stream;
    struct alsa_audio_device *adev = out->dev;
    size_t frame_size = audio_stream_out_frame_size(stream);
    snd_pcm_uframes_t out_frames = bytes / frame_size;

    /* acquiring hw device mutex systematically is useful if a low priority thread is waiting
     * on the output stream mutex - e.g. executing select_mode() while holding the hw device
     * mutex
     */
    pthread_mutex_lock(&adev->lock);
    pthread_mutex_lock(&out->lock);
    if (out->standby) {
        ret = start_output_stream(out);
        if (ret != 0) {
            pthread_mutex_unlock(&adev->lock);
            goto exit;
        }
        out->standby = 0;
    }

    pthread_mutex_unlock(&adev->lock);

    ALOGV("out_write: out_frames:%ld", (long int)out_frames);

    ret = snd_pcm_writei(out->pcm, buffer, out_frames);
    if (ret == out_frames) {
        out->written += out_frames;
    }
exit:
    pthread_mutex_unlock(&out->lock);

    if (ret != out_frames) {
        if (ret == -EPIPE) {
            ALOGE("underrun deteced -> redo snd_pcm_prepare");
            snd_pcm_prepare(out->pcm);
        } else {
            ALOGE("out_write err: %s", snd_strerror(ret));
            usleep((int64_t)bytes * 1000000 / audio_stream_out_frame_size(stream) /
                out_get_sample_rate(&stream->common));
        }
    }

    return bytes;
}

static int out_get_render_position(const struct audio_stream_out *stream,
        uint32_t *dsp_frames)
{
    *dsp_frames = 0;
    ALOGV("out_get_render_position: dsp_frames: %p", dsp_frames);
    return -EINVAL;
}

static int out_get_presentation_position(const struct audio_stream_out *stream,
                                   uint64_t *frames, struct timespec *timestamp)
{
    struct alsa_stream_out *out = (struct alsa_stream_out *)stream;
    int ret = -1;

    if (out->pcm) {
        snd_pcm_uframes_t avail;
        int r;
        if ((r = snd_pcm_htimestamp(out->pcm, &avail, timestamp)) == 0) {
            int64_t signed_frames = (int64_t)(out->written) - out->buffer_size + avail;
            if (signed_frames >= 0) {
                *frames = signed_frames;
                ret = 0;
            }
            ALOGV("out_get_presentation_position: %ld", (long int)(*frames));
        } else {
            ALOGE("out_get_presentation_position: err: %s", snd_strerror(r));
        }
    } else {
        ALOGV("out_get_presentation_position: stream in standby");
    }
    return ret;
}


static int out_add_audio_effect(const struct audio_stream *stream, effect_handle_t effect)
{
    ALOGV("out_add_audio_effect: %p", effect);
    return 0;
}

static int out_remove_audio_effect(const struct audio_stream *stream, effect_handle_t effect)
{
    ALOGV("out_remove_audio_effect: %p", effect);
    return 0;
}

static int out_get_next_write_timestamp(const struct audio_stream_out *stream,
        int64_t *timestamp)
{
    *timestamp = 0;
    ALOGV("out_get_next_write_timestamp: %ld", (long int)(*timestamp));
    return -EINVAL;
}

/** audio_stream_in implementation **/
static uint32_t in_get_sample_rate(const struct audio_stream *stream)
{
    ALOGV("in_get_sample_rate");
    return 8000;
}

static int in_set_sample_rate(struct audio_stream *stream, uint32_t rate)
{
    ALOGV("in_set_sample_rate: %d", rate);
    return -ENOSYS;
}

static size_t in_get_buffer_size(const struct audio_stream *stream)
{
    ALOGV("in_get_buffer_size: %d", 320);
    return 320;
}

static audio_channel_mask_t in_get_channels(const struct audio_stream *stream)
{
    ALOGV("in_get_channels: %d", AUDIO_CHANNEL_IN_MONO);
    return AUDIO_CHANNEL_IN_MONO;
}

static audio_format_t in_get_format(const struct audio_stream *stream)
{
    return AUDIO_FORMAT_PCM_16_BIT;
}

static int in_set_format(struct audio_stream *stream, audio_format_t format)
{
    return -ENOSYS;
}

static int in_standby(struct audio_stream *stream)
{
    return 0;
}

static int in_dump(const struct audio_stream *stream, int fd)
{
    return 0;
}

static int in_set_parameters(struct audio_stream *stream, const char *kvpairs)
{
    return 0;
}

static char * in_get_parameters(const struct audio_stream *stream,
        const char *keys)
{
    return strdup("");
}

static int in_set_gain(struct audio_stream_in *stream, float gain)
{
    return 0;
}

static ssize_t in_read(struct audio_stream_in *stream, void* buffer,
        size_t bytes)
{
    ALOGV("in_read: bytes %zu", bytes);
    /* XXX: fake timing for audio input */
    usleep((int64_t)bytes * 1000000 / audio_stream_in_frame_size(stream) /
            in_get_sample_rate(&stream->common));
    memset(buffer, 0, bytes);
    return bytes;
}

static uint32_t in_get_input_frames_lost(struct audio_stream_in *stream)
{
    return 0;
}

static int in_add_audio_effect(const struct audio_stream *stream, effect_handle_t effect)
{
    return 0;
}

static int in_remove_audio_effect(const struct audio_stream *stream, effect_handle_t effect)
{
    return 0;
}

static int adev_open_output_stream(struct audio_hw_device *dev,
        audio_io_handle_t handle,
        audio_devices_t devices,
        audio_output_flags_t flags,
        struct audio_config *config,
        struct audio_stream_out **stream_out,
        const char *address __unused)
{
    ALOGV("adev_open_output_stream...");

    struct alsa_audio_device *ladev = (struct alsa_audio_device *)dev;
    struct alsa_stream_out *out;
    int ret = 0;

    out = (struct alsa_stream_out *)calloc(1, sizeof(struct alsa_stream_out));
    if (!out)
        return -ENOMEM;

    out->stream.common.get_sample_rate = out_get_sample_rate;
    out->stream.common.set_sample_rate = out_set_sample_rate;
    out->stream.common.get_buffer_size = out_get_buffer_size;
    out->stream.common.get_channels = out_get_channels;
    out->stream.common.get_format = out_get_format;
    out->stream.common.set_format = out_set_format;
    out->stream.common.standby = out_standby;
    out->stream.common.dump = out_dump;
    out->stream.common.set_parameters = out_set_parameters;
    out->stream.common.get_parameters = out_get_parameters;
    out->stream.common.add_audio_effect = out_add_audio_effect;
    out->stream.common.remove_audio_effect = out_remove_audio_effect;
    out->stream.get_latency = out_get_latency;
    out->stream.set_volume = out_set_volume;
    out->stream.write = out_write;
    out->stream.get_render_position = out_get_render_position;
    out->stream.get_next_write_timestamp = out_get_next_write_timestamp;
    out->stream.get_presentation_position = out_get_presentation_position;

    out->period_size = PERIOD_SIZE;
    out->periods = PLAYBACK_PERIOD_COUNT;
    out->buffer_size = out->period_size * out->periods;

    out->dev = ladev;
    out->standby = 1;
    out->unavailable = false;

    config->format = out_get_format(&out->stream.common);
    config->channel_mask = out_get_channels(&out->stream.common);
    config->sample_rate = out_get_sample_rate(&out->stream.common);

    *stream_out = &out->stream;

    /* TODO The retry mechanism isn't implemented in AudioPolicyManager/AudioFlinger. */
    ret = 0;

    return ret;
}

static void adev_close_output_stream(struct audio_hw_device *dev,
        struct audio_stream_out *stream)
{
    ALOGV("adev_close_output_stream...");
    free(stream);
}

static int adev_set_parameters(struct audio_hw_device *dev, const char *kvpairs)
{
    ALOGV("adev_set_parameters");
    return -ENOSYS;
}

static char * adev_get_parameters(const struct audio_hw_device *dev,
        const char *keys)
{
    ALOGV("adev_get_parameters");
    return strdup("");
}

static int adev_init_check(const struct audio_hw_device *dev)
{
    ALOGV("adev_init_check");
    return 0;
}

static int adev_set_voice_volume(struct audio_hw_device *dev, float volume)
{
    ALOGV("adev_set_voice_volume: %f", volume);
    return -ENOSYS;
}

static int adev_set_master_volume(struct audio_hw_device *dev, float volume)
{
    ALOGV("adev_set_master_volume: %f", volume);
    return -ENOSYS;
}

static int adev_get_master_volume(struct audio_hw_device *dev, float *volume)
{
    ALOGV("adev_get_master_volume: %f", *volume);
    return -ENOSYS;
}

static int adev_set_master_mute(struct audio_hw_device *dev, bool muted)
{
    ALOGV("adev_set_master_mute: %d", muted);
    return -ENOSYS;
}

static int adev_get_master_mute(struct audio_hw_device *dev, bool *muted)
{
    ALOGV("adev_get_master_mute: %d", *muted);
    return -ENOSYS;
}

static int adev_set_mode(struct audio_hw_device *dev, audio_mode_t mode)
{
    ALOGV("adev_set_mode: %d", mode);
    return 0;
}

static int adev_set_mic_mute(struct audio_hw_device *dev, bool state)
{
    ALOGV("adev_set_mic_mute: %d",state);
    return -ENOSYS;
}

static int adev_get_mic_mute(const struct audio_hw_device *dev, bool *state)
{
    ALOGV("adev_get_mic_mute");
    return -ENOSYS;
}

static size_t adev_get_input_buffer_size(const struct audio_hw_device *dev,
        const struct audio_config *config)
{
    ALOGV("adev_get_input_buffer_size: %d", 320);
    return 320;
}

static int adev_open_input_stream(struct audio_hw_device __unused *dev,
        audio_io_handle_t handle,
        audio_devices_t devices,
        struct audio_config *config,
        struct audio_stream_in **stream_in,
        audio_input_flags_t flags __unused,
        const char *address __unused,
        audio_source_t source __unused)
{
    struct stub_stream_in *in;

    ALOGV("adev_open_input_stream...");

    in = (struct stub_stream_in *)calloc(1, sizeof(struct stub_stream_in));
    if (!in)
        return -ENOMEM;

    in->stream.common.get_sample_rate = in_get_sample_rate;
    in->stream.common.set_sample_rate = in_set_sample_rate;
    in->stream.common.get_buffer_size = in_get_buffer_size;
    in->stream.common.get_channels = in_get_channels;
    in->stream.common.get_format = in_get_format;
    in->stream.common.set_format = in_set_format;
    in->stream.common.standby = in_standby;
    in->stream.common.dump = in_dump;
    in->stream.common.set_parameters = in_set_parameters;
    in->stream.common.get_parameters = in_get_parameters;
    in->stream.common.add_audio_effect = in_add_audio_effect;
    in->stream.common.remove_audio_effect = in_remove_audio_effect;
    in->stream.set_gain = in_set_gain;
    in->stream.read = in_read;
    in->stream.get_input_frames_lost = in_get_input_frames_lost;

    *stream_in = &in->stream;
    return 0;
}

static void adev_close_input_stream(struct audio_hw_device *dev,
        struct audio_stream_in *in)
{
    ALOGV("adev_close_input_stream...");
    return;
}

static int adev_dump(const audio_hw_device_t *device, int fd)
{
    ALOGV("adev_dump");
    return 0;
}

static int adev_close(hw_device_t *device)
{
    ALOGV("adev_close");
    free(device);
    return 0;
}

static int adev_open(const hw_module_t* module, const char* name,
        hw_device_t** device)
{
    struct alsa_audio_device *adev;

    ALOGV("adev_open: %s", name);

    if (strcmp(name, AUDIO_HARDWARE_INTERFACE) != 0)
        return -EINVAL;

    adev = calloc(1, sizeof(struct alsa_audio_device));
    if (!adev)
        return -ENOMEM;

    adev->hw_device.common.tag = HARDWARE_DEVICE_TAG;
    adev->hw_device.common.version = AUDIO_DEVICE_API_VERSION_2_0;
    adev->hw_device.common.module = (struct hw_module_t *) module;
    adev->hw_device.common.close = adev_close;
    adev->hw_device.init_check = adev_init_check;
    adev->hw_device.set_voice_volume = adev_set_voice_volume;
    adev->hw_device.set_master_volume = adev_set_master_volume;
    adev->hw_device.get_master_volume = adev_get_master_volume;
    adev->hw_device.set_master_mute = adev_set_master_mute;
    adev->hw_device.get_master_mute = adev_get_master_mute;
    adev->hw_device.set_mode = adev_set_mode;
    adev->hw_device.set_mic_mute = adev_set_mic_mute;
    adev->hw_device.get_mic_mute = adev_get_mic_mute;
    adev->hw_device.set_parameters = adev_set_parameters;
    adev->hw_device.get_parameters = adev_get_parameters;
    adev->hw_device.get_input_buffer_size = adev_get_input_buffer_size;
    adev->hw_device.open_output_stream = adev_open_output_stream;
    adev->hw_device.close_output_stream = adev_close_output_stream;
    adev->hw_device.open_input_stream = adev_open_input_stream;
    adev->hw_device.close_input_stream = adev_close_input_stream;
    adev->hw_device.dump = adev_dump;

    adev->devices = AUDIO_DEVICE_NONE;

    *device = &adev->hw_device.common;

    return 0;
}

static struct hw_module_methods_t hal_module_methods = {
    .open = adev_open,
};

struct audio_module HAL_MODULE_INFO_SYM = {
    .common = {
        .tag = HARDWARE_MODULE_TAG,
        .module_api_version = AUDIO_MODULE_API_VERSION_0_1,
        .hal_api_version = HARDWARE_HAL_API_VERSION,
        .id = AUDIO_HARDWARE_MODULE_ID,
        .name = "Raspberry Pi audio hdmi HW HAL",
        .author = "The Android Open Source Project",
        .methods = &hal_module_methods,
    },
};
