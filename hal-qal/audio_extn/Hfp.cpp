/*
 * Copyright (c) 2012-2020, The Linux Foundation. All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are
 * met:
 *   * Redistributions of source code must retain the above copyright
 *     notice, this list of conditions and the following disclaimer.
 *   * Redistributions in binary form must reproduce the above
 *     copyright notice, this list of conditions and the following
 *     disclaimer in the documentation and/or other materials provided
 *     with the distribution.
 *   * Neither the name of The Linux Foundation nor the names of its
 *     contributors may be used to endorse or promote products derived
 *     from this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED "AS IS" AND ANY EXPRESS OR IMPLIED
 * WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES OF
 * MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NON-INFRINGEMENT
 * ARE DISCLAIMED.  IN NO EVENT SHALL THE COPYRIGHT OWNER OR CONTRIBUTORS
 * BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR
 * BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY,
 * WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE
 * OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN
 * IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#define LOG_TAG "hfp"
#define LOG_NDDEBUG 0

#include <errno.h>
#include <math.h>
#include <log/log.h>
#include <fcntl.h>
#include <dirent.h>
#include <sys/stat.h>
#include <stdlib.h>
#include <dlfcn.h>
#include <math.h>
#include <cutils/properties.h>
#include "QalApi.h"
#include "AudioDevice.h"

#ifdef __cplusplus
extern "C" {
#endif

#define AUDIO_PARAMETER_HFP_ENABLE      "hfp_enable"
#define AUDIO_PARAMETER_HFP_SET_SAMPLING_RATE "hfp_set_sampling_rate"
#define AUDIO_PARAMETER_KEY_HFP_VOLUME "hfp_volume"
#define AUDIO_PARAMETER_HFP_PCM_DEV_ID "hfp_pcm_dev_id"

#define AUDIO_PARAMETER_KEY_HFP_MIC_VOLUME "hfp_mic_volume"

struct hfp_module {
    bool is_hfp_running;
    float hfp_volume;
    int ucid;
    float mic_volume;
    bool mic_mute;
    uint32_t sample_rate;
    qal_stream_handle_t *rx_stream_handle;
    qal_stream_handle_t *tx_stream_handle;
};

#define PLAYBACK_VOLUME_MAX 0x2000
#define CAPTURE_VOLUME_DEFAULT                (15.0)
static struct hfp_module hfpmod = {
    .is_hfp_running = 0,
    .hfp_volume = 0,
    .ucid = USECASE_AUDIO_HFP_SCO,
    .mic_volume = CAPTURE_VOLUME_DEFAULT,
    .mic_mute = 0,
    .sample_rate = 16000,
};

static int32_t hfp_set_volume(float value)
{
    int32_t vol, ret = 0;
    struct qal_volume_data *qal_volume = NULL;

    ALOGV("%s: entry", __func__);
    ALOGD("%s: (%f)\n", __func__, value);

    hfpmod.hfp_volume = value;

    if (value < 0.0) {
        ALOGW("%s: (%f) Under 0.0, assuming 0.0\n", __func__, value);
        value = 0.0;
    } else {
        value = ((value > 15.000000) ? 1.0 : (value / 15));
        ALOGW("%s: Volume brought with in range (%f)\n", __func__, value);
    }
    vol  = lrint((value * 0x2000) + 0.5);

    if (!hfpmod.is_hfp_running) {
        ALOGV("%s: HFP not active, ignoring set_hfp_volume call", __func__);
        return -EIO;
    }

    ALOGD("%s: Setting HFP volume to %d \n", __func__, vol);

    qal_volume = (struct qal_volume_data *)malloc(sizeof(struct qal_volume_data)
            +sizeof(struct qal_channel_vol_kv));
    qal_volume->no_of_volpair = 1;
    qal_volume->volume_pair[0].channel_mask = 0x03;
    qal_volume->volume_pair[0].vol = value;
    ret = qal_stream_set_volume(hfpmod.rx_stream_handle, qal_volume);
    if (ret)
        ALOGE("%s: set volume failed: %d \n", __func__, ret);

    ALOGV("%s: exit", __func__);
    return ret;
}

/*Set mic volume to value.
 * *
 * * This interface is used for mic volume control, set mic volume as value(range 0 ~ 15).
 * */
static int hfp_set_mic_volume(float value)
{
    int volume, ret = 0;
    struct qal_volume_data *qal_volume = NULL;

    ALOGD("%s: enter, value=%f", __func__, value);

    if (!hfpmod.is_hfp_running) {
        ALOGE("%s: HFP not active, ignoring set_hfp_mic_volume call", __func__);
        return -EIO;
    }

    if (value < 0.0) {
        ALOGW("%s: (%f) Under 0.0, assuming 0.0\n", __func__, value);
        value = 0.0;
    } else if (value > CAPTURE_VOLUME_DEFAULT) {
        value = CAPTURE_VOLUME_DEFAULT;
        ALOGW("%s: Volume brought within range (%f)\n", __func__, value);
    }

    value = value / CAPTURE_VOLUME_DEFAULT;

    volume = (int)(value * PLAYBACK_VOLUME_MAX);

    qal_volume = (struct qal_volume_data *)malloc(sizeof(struct qal_volume_data)
            +sizeof(struct qal_channel_vol_kv));
    qal_volume->no_of_volpair = 1;
    qal_volume->volume_pair[0].channel_mask = 0x03;
    qal_volume->volume_pair[0].vol = value;
    if (qal_stream_set_volume(hfpmod.tx_stream_handle, qal_volume) < 0) {
        ALOGE("%s: Couldn't set HFP Volume: [%d]", __func__, volume);
        return -EINVAL;
    }

    hfpmod.mic_volume = value;

    return ret;
}

static float hfp_get_mic_volume(std::shared_ptr<AudioDevice> adev __unused)
{
    return hfpmod.mic_volume;
}

static int32_t start_hfp(std::shared_ptr<AudioDevice> adev __unused,
        struct str_parms *parms __unused)
{
    int32_t ret = 0;
    uint32_t no_of_devices = 2;
    struct qal_stream_attributes stream_attr = {};
    struct qal_stream_attributes stream_tx_attr = {};
    struct qal_device devices[2] = {};
    struct qal_channel_info ch_info;

    ALOGD("%s: HFP start enter", __func__);
    if (hfpmod.rx_stream_handle || hfpmod.tx_stream_handle)
        return 0; //hfp already running;

    qal_param_device_connection_t param_device_connection;

    param_device_connection.id = QAL_DEVICE_IN_BLUETOOTH_SCO_HEADSET;
    param_device_connection.connection_state = true;
    ret =  qal_set_param(QAL_PARAM_ID_DEVICE_CONNECTION,
                        (void*)&param_device_connection,
                        sizeof(qal_param_device_connection_t));
    if (ret != 0) {
        ALOGE("%s: Set QAL_PARAM_ID_DEVICE_CONNECTION for %d failed", __func__, param_device_connection.id);
        return ret;
    }

    param_device_connection.id = QAL_DEVICE_OUT_BLUETOOTH_SCO;
    param_device_connection.connection_state = true;
    ret =  qal_set_param(QAL_PARAM_ID_DEVICE_CONNECTION,
                        (void*)&param_device_connection,
                        sizeof(qal_param_device_connection_t));
    if (ret != 0) {
        ALOGE("%s: Set QAL_PARAM_ID_DEVICE_CONNECTION for %d failed", __func__, param_device_connection.id);
        return ret;
    }

    qal_param_btsco_t param_btsco;

    param_btsco.bt_sco_on = true;
    ret =  qal_set_param(QAL_PARAM_ID_BT_SCO,
                        (void*)&param_btsco,
                        sizeof(qal_param_btsco_t));
    if (ret != 0) {
        ALOGE("%s: Set QAL_PARAM_ID_BT_SCO failed", __func__);
        return ret;
    }

    if (hfpmod.sample_rate == 16000) {
        param_btsco.bt_wb_speech_enabled = true;
    }
    else
    {
        param_btsco.bt_wb_speech_enabled = false;
    }

    ret =  qal_set_param(QAL_PARAM_ID_BT_SCO_WB,
                        (void*)&param_btsco,
                        sizeof(qal_param_btsco_t));
    if (ret != 0) {
        ALOGE("%s: Set QAL_PARAM_ID_BT_SCO_WB failed", __func__);
        return ret;
    }

    ch_info.channels = 1;
    ch_info.ch_map[0] = QAL_CHMAP_CHANNEL_FL;

    /* BT SCO -> Spkr */
    stream_attr.type = QAL_STREAM_LOOPBACK;
    stream_attr.info.opt_stream_info.loopback_type = QAL_STREAM_LOOPBACK_HFP_RX;
    stream_attr.direction = QAL_AUDIO_INPUT_OUTPUT;
    stream_attr.in_media_config.sample_rate = hfpmod.sample_rate;
    stream_attr.in_media_config.bit_width = 16;
    stream_attr.in_media_config.ch_info = ch_info;
    stream_attr.in_media_config.aud_fmt_id = QAL_AUDIO_FMT_DEFAULT_PCM;

    stream_attr.out_media_config.sample_rate = 48000;
    stream_attr.out_media_config.bit_width = 16;
    stream_attr.out_media_config.ch_info = ch_info;
    stream_attr.out_media_config.aud_fmt_id = QAL_AUDIO_FMT_DEFAULT_PCM;

    devices[0].id = QAL_DEVICE_IN_BLUETOOTH_SCO_HEADSET;
    devices[0].config.sample_rate = hfpmod.sample_rate;
    devices[0].config.bit_width = 16;
    devices[0].config.ch_info = ch_info;
    devices[0].config.aud_fmt_id = QAL_AUDIO_FMT_DEFAULT_PCM;

    devices[1].id = QAL_DEVICE_OUT_SPEAKER;

    ret = qal_stream_open(&stream_attr,
            no_of_devices, devices,
            0,
            NULL,
            NULL,
            NULL,
            &hfpmod.rx_stream_handle);
    if (ret != 0) {
        ALOGE("%s: HFP rx stream (BT SCO->Spkr) open failed, rc %d", __func__, ret);
        return ret;
    }
    ret = qal_stream_start(hfpmod.rx_stream_handle);
    if (ret != 0) {
        ALOGE("%s: HFP rx stream (BT SCO->Spkr) start failed, rc %d", __func__, ret);
        qal_stream_close(hfpmod.rx_stream_handle);
        return ret;
    }

    /* Mic -> BT SCO */
    stream_tx_attr.type = QAL_STREAM_LOOPBACK;
    stream_tx_attr.info.opt_stream_info.loopback_type = QAL_STREAM_LOOPBACK_HFP_TX;
    stream_tx_attr.direction = QAL_AUDIO_INPUT_OUTPUT;
    stream_tx_attr.in_media_config.sample_rate = hfpmod.sample_rate;
    stream_tx_attr.in_media_config.bit_width = 16;
    stream_tx_attr.in_media_config.ch_info = ch_info;
    stream_tx_attr.in_media_config.aud_fmt_id = QAL_AUDIO_FMT_DEFAULT_PCM;

    stream_tx_attr.out_media_config.sample_rate = 48000;
    stream_tx_attr.out_media_config.bit_width = 16;
    stream_tx_attr.out_media_config.ch_info = ch_info;
    stream_tx_attr.out_media_config.aud_fmt_id = QAL_AUDIO_FMT_DEFAULT_PCM;

    devices[0].id = QAL_DEVICE_OUT_BLUETOOTH_SCO;
    devices[0].config.sample_rate = hfpmod.sample_rate;
    devices[0].config.bit_width = 16;
    devices[0].config.ch_info = ch_info;
    devices[0].config.aud_fmt_id = QAL_AUDIO_FMT_DEFAULT_PCM;

    devices[1].id = QAL_DEVICE_IN_SPEAKER_MIC;

    ret = qal_stream_open(&stream_tx_attr,
            no_of_devices, devices,
            0,
            NULL,
            NULL,
            NULL,
            &hfpmod.tx_stream_handle);
    if (ret != 0) {
        ALOGE("%s: HFP tx stream (Mic->BT SCO) open failed, rc %d", __func__, ret);
        qal_stream_stop(hfpmod.rx_stream_handle);
        qal_stream_close(hfpmod.rx_stream_handle);
        return ret;
    }
    ret = qal_stream_start(hfpmod.tx_stream_handle);
    if (ret != 0) {
        ALOGE("%s: HFP tx stream (Mic->BT SCO) start failed, rc %d", __func__, ret);
        qal_stream_close(hfpmod.tx_stream_handle);
        qal_stream_stop(hfpmod.rx_stream_handle);
        qal_stream_close(hfpmod.rx_stream_handle);
        return ret;
    }
    hfpmod.is_hfp_running = true;
    hfp_set_volume(hfpmod.hfp_volume);

    ALOGD("%s: HFP start end", __func__);
    return ret;
}

static int32_t stop_hfp()
{
    int32_t ret = 0;
    if (hfpmod.rx_stream_handle) {
        qal_stream_stop(hfpmod.rx_stream_handle);
        qal_stream_close(hfpmod.rx_stream_handle);
    }
    if (hfpmod.tx_stream_handle) {
        qal_stream_stop(hfpmod.tx_stream_handle);
        qal_stream_close(hfpmod.tx_stream_handle);
    }

    qal_param_btsco_t param_btsco;

    param_btsco.bt_sco_on = true;
    ret =  qal_set_param(QAL_PARAM_ID_BT_SCO,
                        (void*)&param_btsco,
                        sizeof(qal_param_btsco_t));
    if (ret != 0) {
        ALOGE("%s: Set QAL_PARAM_ID_BT_SCO failed", __func__);
    }

    qal_param_device_connection_t param_device_connection;

    param_device_connection.id = QAL_DEVICE_IN_BLUETOOTH_SCO_HEADSET;
    param_device_connection.connection_state = true;
    ret =  qal_set_param(QAL_PARAM_ID_DEVICE_CONNECTION,
                        (void*)&param_device_connection,
                        sizeof(qal_param_device_connection_t));
    if (ret != 0) {
        ALOGE("%s: Set QAL_PARAM_ID_DEVICE_DISCONNECTION for %d failed", __func__, param_device_connection.id);
    }

    param_device_connection.id = QAL_DEVICE_OUT_BLUETOOTH_SCO;
    param_device_connection.connection_state = true;
    ret =  qal_set_param(QAL_PARAM_ID_DEVICE_CONNECTION,
                        (void*)&param_device_connection,
                        sizeof(qal_param_device_connection_t));
    if (ret != 0) {
        ALOGE("%s: Set QAL_PARAM_ID_DEVICE_DISCONNECTION for %d failed", __func__, param_device_connection.id);
    }

    return ret;
}

void hfp_init()
{
    return;
}

bool hfp_is_active(std::shared_ptr<AudioDevice> adev __unused)
{
    return hfpmod.is_hfp_running;
}

audio_usecase_t hfp_get_usecase()
{
    return hfpmod.ucid;
}

/*Set mic mute state.
 * *
 * * This interface is used for mic mute state control
 * */
int hfp_set_mic_mute(std::shared_ptr<AudioDevice> adev,
        bool state)
{
    int rc = 0;

    if (state == hfpmod.mic_mute)
        return rc;

    if (state == true) {
        hfpmod.mic_volume = hfp_get_mic_volume(adev);
    }
    rc = hfp_set_mic_volume((state == true) ? 0.0 : hfpmod.mic_volume);
    hfpmod.mic_mute = state;
    ALOGD("%s: Setting mute state %d, rc %d\n", __func__, state, rc);
    return rc;
}

int hfp_set_mic_mute2(std::shared_ptr<AudioDevice> adev __unused, bool state __unused)
{
    ALOGD("%s: Unsupported\n", __func__);
    return 0;
}

void hfp_set_parameters(std::shared_ptr<AudioDevice> adev, struct str_parms *parms)
{
    int status = 0;
    char value[32]={0};
    float vol;
    int val;
    int rate;

    ALOGD("%s: enter", __func__);

    status = str_parms_get_str(parms, AUDIO_PARAMETER_HFP_ENABLE, value,
                                    sizeof(value));
    if (status >= 0) {
        if (!strncmp(value, "true", sizeof(value)) && !hfpmod.is_hfp_running)
            status = start_hfp(adev, parms);
        else if (!strncmp(value, "false", sizeof(value)) && hfpmod.is_hfp_running)
            stop_hfp();
        else
            ALOGE("hfp_enable=%s is unsupported", value);
    }

    memset(value, 0, sizeof(value));
    status = str_parms_get_str(parms,AUDIO_PARAMETER_HFP_SET_SAMPLING_RATE, value,
                                    sizeof(value));
    if (status >= 0) {
        rate = atoi(value);
        if (rate == 8000){
            hfpmod.ucid = USECASE_AUDIO_HFP_SCO;
            hfpmod.sample_rate = (uint32_t) rate;
        } else if (rate == 16000){
            hfpmod.ucid = USECASE_AUDIO_HFP_SCO_WB;
            hfpmod.sample_rate = (uint32_t) rate;
        } else
            ALOGE("Unsupported rate.. %d", rate);
    }

    memset(value, 0, sizeof(value));
    status = str_parms_get_str(parms, AUDIO_PARAMETER_KEY_HFP_VOLUME,
                                    value, sizeof(value));
    if (status >= 0) {
        if (sscanf(value, "%f", &vol) != 1){
            ALOGE("%s: error in retrieving hfp volume", __func__);
            status = -EIO;
            goto exit;
        }
        ALOGD("%s: set_hfp_volume usecase, Vol: [%f]", __func__, vol);
        hfp_set_volume(vol);
    }

    memset(value, 0, sizeof(value));
    status = str_parms_get_str(parms, AUDIO_PARAMETER_KEY_HFP_MIC_VOLUME,
                                    value, sizeof(value));
    if (status >= 0) {
        if (sscanf(value, "%f", &vol) != 1){
            ALOGE("%s: error in retrieving hfp mic volume", __func__);
            status = -EIO;
            goto exit;
        }
        ALOGD("%s: set_hfp_mic_volume usecase, Vol: [%f]", __func__, vol);
        hfp_set_mic_volume(vol);
    }

exit:
    ALOGV("%s Exit",__func__);
}

#ifdef __cplusplus
}
#endif
