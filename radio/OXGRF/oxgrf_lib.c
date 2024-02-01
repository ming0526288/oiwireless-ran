/*
 * Licensed to the OpenAirInterface (OAI) Software Alliance under one or more
 * contributor license agreements.  See the NOTICE file distributed with
 * this work for additional information regarding copyright ownership.
 * The OpenAirInterface Software Alliance licenses this file to You under
 * the OAI Public License, Version 1.0  (the "License"); you may not use this file
 * except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.openairinterface.org/?page_id=698
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 *-------------------------------------------------------------------------------
 * For more information about the OpenAirInterface (OAI) Software Alliance:
 *      contact@openairinterface.org
 */

/** oxgrf_lib.c
 *
 * Author: eric
 * base on bladerf_lib.c
 */

#pragma GCC optimize(3, "Ofast", "inline")
#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>
#include <unistd.h>
#include <string.h>
#include <inttypes.h>
#include <math.h>
#include "oxgrf_lib.h"
#include "rf_helper.h"
#include "common/utils/LOG/log.h"

/** @addtogroup _OXGRF_PHY_RF_INTERFACE_
 * @{
 */
#include "openair1/PHY/sse_intrin.h"

//! Number of OXGRF devices
int num_devices = 0;
static bool running = false;

#define BUFFER_SIZE    (122880 * 100 * sizeof(int))
#define NCHAN_PER_DEV  4
static void *cache_buf[NCHAN_PER_DEV];
static void *iq_buf[NCHAN_PER_DEV];
static uint32_t remain = 0;
static uint32_t RX_MTU = 30720;
static uint8_t shift = 2;

static inline int channel_to_mask(int channel_count)
{
    uint8_t ch_mask;
    switch (channel_count) {
    case 4:
        ch_mask = 0xf;break;
    case 3:
        ch_mask = 0x7;break;
    case 2:
        ch_mask = 0x3;break;
    case 1:
        ch_mask = 0x1;break;
    default:
        ch_mask = 0x1;break;
    }

    return ch_mask;
}

/*! \brief get current timestamp
 *\param device the hardware to use
 *\returns timestamp of OXGRF
 */

openair0_timestamp trx_get_timestamp(openair0_device *device) {
    return 0;
}

/*! \brief Start oxgrf
 * \param device the hardware to use
 * \returns 0 on success
 */
int trx_oxgrf_start(openair0_device *device) {

    LOG_I(HW, "[oxgrf] Start oxgrf ...\n");
    running = true;

    return 0;
}

/*! \brief Called to send samples to the oxgrf RF target
  \param device pointer to the device structure specific to the RF hardware target
  \param timestamp The timestamp at whicch the first sample MUST be sent
  \param buff Buffer which holds the samples
  \param nsamps number of samples to be sent
  \param cc index of the component carrier
  \param flags Ignored for the moment
  \returns 0 on success
  */
static int trx_oxgrf_write(openair0_device *device,openair0_timestamp timestamp, void **buff, int nsamps, int cc, int flags) {

    int status;
    oxgrf_state_t *oxgrf = (oxgrf_state_t*)device->priv;

    uint32_t trx_flags = 0;
    radio_tx_burst_flag_t flags_burst = (radio_tx_burst_flag_t) (flags & 0xf);

    if (flags_burst == TX_BURST_START) {
        trx_flags = 0;
    } else if (flags_burst == TX_BURST_END) {
        trx_flags = 1;
    } else if (flags_burst == TX_BURST_START_AND_END) {
        trx_flags = 1;
    } else if (flags_burst == TX_BURST_MIDDLE) {
        trx_flags = 0;
    }

    for(int i = 0; i < cc; i++) {
        int len = nsamps * 2;
        int16_t *iq = buff[i];
#if defined(__x86_64) || defined(__i386__)
        __m256i a, *b;

        while (len >= 16) {
            a = *(__m256i *)&iq[0];
            b = (__m256i *)&iq[0];
            *b = simde_mm256_slli_epi16(a, 4);
            iq += 16;
            len -= 16;
        }
#elif defined(__arm__) || defined(__aarch64__)
        int16x8_t a, *b;

        while (len >= 8) {
            a = *(int16x8_t *)&iq[0];
            b = (int16x8_t *)&iq[0];
            *b = vshlq_n_s16(a, 4);
            iq += 8;
            len -= 8;
        }
#endif
        /* remaining data */
        while (len != 0) {
            iq[0] <<= 4;
            iq++;
            len--;
        }
    }

    status = oxgrf_write_samples_multiport(oxgrf->dev, (const void **)buff, nsamps, channel_to_mask(cc), timestamp, trx_flags);
    if (status < 0) {
        oxgrf->num_tx_errors++;
        LOG_E(HW, "[oxgrf] Failed to TX samples\n");
        exit(-1);
    }

    //LOG_D(HW, "Provided TX timestamp: %u, nsamps: %u\n", ptimestamp, nsamps);

    oxgrf->tx_current_ts = timestamp;
    oxgrf->tx_nsamps += nsamps;
    oxgrf->tx_count++;

    return nsamps;
}

/*! \brief Receive samples from hardware.
 * Read \ref nsamps samples from each channel to buffers. buff[0] is the array for
 * the first channel. *ptimestamp is the time at which the first sample
 * was received.
 * \param device the hardware to use
 * \param[out] ptimestamp the time at which the first sample was received.
 * \param[out] buff An array of pointers to buffers for received samples. The buffers must be large enough to hold the number of samples \ref nsamps.
 * \param nsamps Number of samples. One sample is 2 byte I + 2 byte Q => 4 byte.
 * \param cc  Index of component carrier
 * \returns number of samples read
 */
static int trx_oxgrf_read(openair0_device *device, openair0_timestamp *ptimestamp, void **buff, int nsamps, int cc) {

    int status;
    oxgrf_state_t *oxgrf = (oxgrf_state_t *)device->priv;
    uint64_t timestamp = 0UL;

retry:
    if(remain == 0) {
        int recv = 0;
        timestamp = 0UL;
        if(nsamps % RX_MTU) {
            recv = (nsamps / RX_MTU + 1) * RX_MTU;
            status = oxgrf_read_samples_multiport(oxgrf->dev, iq_buf, recv, channel_to_mask(cc), &timestamp);
            if (status < 0) {
                LOG_E(HW, "[oxgrf] Failed to read samples %d\n", nsamps);
                oxgrf->num_rx_errors++;
                exit(-1);
            }
            for(int i = 0; i < cc; i++)
                memcpy(buff[i], iq_buf[i], nsamps * 4);
            if(recv > nsamps) {
                for(int i = 0; i < cc; i++)
                    memcpy(cache_buf[i], iq_buf[i] + nsamps * 4, (recv - nsamps) * 4);
                remain = recv - nsamps;
            }
        } else {
            recv = nsamps;
            status = oxgrf_read_samples_multiport(oxgrf->dev, buff, recv, channel_to_mask(cc), &timestamp);
            if (status < 0) {
                LOG_E(HW, "[oxgrf] Failed to read samples %d\n", nsamps);
                oxgrf->num_rx_errors++;
                exit(-1);
            }
        }

        *(uint64_t *)ptimestamp = timestamp;
        oxgrf->rx_current_ts = timestamp + nsamps;
        //LOG_D(HW, "case 0: Current RX timestamp  %"PRIu64", hw ts %"PRIu64", nsamps %u, remain %u, recv: %u\n",  *ptimestamp, timestamp, nsamps, remain, recv);
    } else if(remain >= nsamps) {
        for(int i = 0; i < cc; i++)
            memcpy(buff[i], cache_buf[i], nsamps * 4);
        remain -= nsamps;
        if(remain > 0) {
            for(int i = 0; i < cc; i++)
                memmove(cache_buf[i], cache_buf[i] + nsamps * 4, remain * 4);
        }
        *(uint64_t *)ptimestamp = oxgrf->rx_current_ts;
        oxgrf->rx_current_ts += nsamps;
        //LOG_D(HW, "case 1: Current RX timestamp  %"PRIu64", nsamps %u, remain %u\n",  *ptimestamp, nsamps, remain);
    } else {
        int recv;
        if(remain + RX_MTU >= nsamps)
            recv = RX_MTU;
        else
            recv = (nsamps / RX_MTU + 1) * RX_MTU;
        timestamp = 0UL;
        status = oxgrf_read_samples_multiport(oxgrf->dev, iq_buf, recv, channel_to_mask(cc), &timestamp);
        if (status < 0) {
            LOG_E(HW, "[oxgrf] Failed to read samples %d\n", nsamps);
            oxgrf->num_rx_errors++;
            exit(-1);
        }
        if(timestamp != (oxgrf->rx_current_ts + remain)) {
            int overflow = timestamp - (oxgrf->rx_current_ts + remain);
            LOG_W(HW, "Rx overflow %u samples\n", overflow);
            //remain += overflow;
            remain = 0;
            goto retry;
        }
        for(int i = 0; i < cc; i++)
            memcpy(cache_buf[i] + remain * 4, iq_buf[i], recv * 4);
        for(int i = 0; i < cc; i++)
            memcpy(buff[i], cache_buf[i], nsamps * 4);
        remain = recv + remain - nsamps;
        for(int i = 0; i < cc; i++)
            memmove(cache_buf[i], cache_buf[i] + nsamps * 4, remain * 4);

        *(uint64_t *)ptimestamp = oxgrf->rx_current_ts;
        oxgrf->rx_current_ts += nsamps;
        //LOG_D(HW, "case 2: Current RX timestamp  %"PRIu64", hw ts %"PRIu64", nsamps %u, remain %u, recv: %u\n",  *ptimestamp, timestamp, nsamps, remain, recv);
    }

    for(int i = 0; i < cc; i++) {
        int len = nsamps * 2;
        int16_t *iq = buff[i];
#if defined(__x86_64__) || defined(__i386__)
        __m256i a, *b;

        while (len >= 16) {
            a = *(__m256i *)&iq[0];
            b = (__m256i *)&iq[0];
            *b = simde_mm256_srai_epi16(a, shift);
            iq += 16;
            len -= 16;
        }
#elif defined(__arm__) || defined(__aarch64__)
        int16x8_t a, *b;

        while (len >= 8) {
            a = *(int16x8_t *)&iq[0];
            b = (int16x8_t *)&iq[0];
            *b = vshrq_n_s16(a, shift);
            iq += 8;
            len -= 8;
        }
#endif
        /* remaining data */
        while (len != 0) {
            iq[0] >>= shift;
            iq++;
            len--;
        }
    }
    //LOG_D(HW, "Current RX timestamp  %"PRIu64", nsamps %u\n",  *ptimestamp, nsamps);
    oxgrf->rx_nsamps += nsamps;
    oxgrf->rx_count++;

    return nsamps;

}

/*! \brief Terminate operation of the oxgrf transceiver -- free all associated resources
 * \param device the hardware to use
 */
void trx_oxgrf_end(openair0_device *device) {

    oxgrf_state_t *oxgrf = (oxgrf_state_t*)device->priv;

    if(!running)
        return;
    running = false;

    LOG_I(HW, ">>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>\n");
    for(int i = 0; i < oxgrf->tx_num_channels; i++) {
        uint32_t count = 0;
        oxgrf_get_channel_event(oxgrf->dev, TX_CHANNEL_TIMEOUT, i+1, &count);
        LOG_I(HW, "[oxgrf] TX%d Channel timeout: %u\n", i+1, count);
    }
    for(int i = 0; i < oxgrf->rx_num_channels; i++) {
        uint32_t count = 0;
        oxgrf_get_channel_event(oxgrf->dev, RX_CHANNEL_OVERFLOW, i+1, &count);
        LOG_I(HW, "[oxgrf] RX%d Channel overflow: %u\n", i+1, count);
    }
    LOG_I(HW, "<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<\n");

    oxgrf_close_device(oxgrf->dev);

    return;
}

/*! \brief print the oxgrf statistics 
 * \param device the hardware to use
 * \returns  0 on success
 */
int trx_oxgrf_get_stats(openair0_device* device) {
    return(0);
}

/*! \brief Reset the oxgrf statistics 
 * \param device the hardware to use
 * \returns  0 on success
 */
int trx_oxgrf_reset_stats(openair0_device* device) {
    return(0);

}

/*! \brief Stop oxgrf
 * \param card the hardware to use
 * \returns 0 in success 
 */
int trx_oxgrf_stop(openair0_device* device) {
    return(0);
}

/*! \brief Set frequencies (TX/RX)
 * \param device the hardware to use
 * \param openair0_cfg RF frontend parameters set by application
 * \param dummy dummy variable not used
 * \returns 0 in success
 */
int trx_oxgrf_set_freq(openair0_device* device, openair0_config_t *openair0_cfg) {

    int status;
    oxgrf_state_t *oxgrf = (oxgrf_state_t *)device->priv;

    if(oxgrf->tx_lo_freq != openair0_cfg->tx_freq[0]) {
        if ((status = oxgrf_set_tx_lo_freq(oxgrf->dev, 0, (uint64_t)(openair0_cfg->tx_freq[0]))) < 0) {
            LOG_E(HW, "[oxgrf] Failed to set TX frequency\n");
        } else {
            LOG_I(HW, "[oxgrf] set TX frequency to %lu\n",(uint64_t)(openair0_cfg->tx_freq[0]));
            oxgrf->tx_lo_freq = openair0_cfg->tx_freq[0];
        }
    }

    if(oxgrf->rx_lo_freq != openair0_cfg->rx_freq[0]) {
        if ((status = oxgrf_set_rx_lo_freq(oxgrf->dev, 0, (uint64_t)(openair0_cfg->rx_freq[0]))) < 0) {
            LOG_E(HW, "[oxgrf] Failed to set RX frequency\n");
        } else {
            LOG_I(HW, "[oxgrf] set RX frequency to %lu\n",(uint64_t)(openair0_cfg->rx_freq[0]));
            oxgrf->rx_lo_freq = openair0_cfg->rx_freq[0];
        }
    }

    return(0);

}

/*! \brief calibration table for OXGRF */
rx_gain_calib_table_t calib_table_oxgrf[] = {
    {3500000000.0, 72.0},
    {2660000000.0, 72.0},
    {2300000000.0, 72.0},
    {1880000000.0, 72.0},
    {816000000.0,  72.0},
    {-1,0}
};

/*! \brief set RX gain offset from calibration table
 * \param openair0_cfg RF frontend parameters set by application
 * \param chain_index RF chain ID
 */
void set_rx_gain_offset(openair0_config_t *openair0_cfg, int chain_index) {

    int i = 0;
    // loop through calibration table to find best adjustment factor for RX frequency
    double min_diff = 6e9, diff;

    while (openair0_cfg->rx_gain_calib_table[i].freq > 0) {
        diff = fabs(openair0_cfg->rx_freq[chain_index] - openair0_cfg->rx_gain_calib_table[i].freq);
        printf("cal %d: freq %f, offset %f, diff %f\n",
                i,
                openair0_cfg->rx_gain_calib_table[i].freq,
                openair0_cfg->rx_gain_calib_table[i].offset, diff);
        if (min_diff > diff) {
            min_diff = diff;
            openair0_cfg->rx_gain_offset[chain_index] = openair0_cfg->rx_gain_calib_table[i].offset;
        }
        i++;
    }
}

/*! \brief Set Gains (TX/RX)
 * \param device the hardware to use
 * \param openair0_cfg openair0 Config structure
 * \returns 0 in success
 */
int trx_oxgrf_set_gains(openair0_device* device, openair0_config_t *openair0_cfg) {

    int ret = 0;
    oxgrf_state_t *oxgrf = (oxgrf_state_t *)device->priv;

    if (openair0_cfg->rx_gain[0] > 65+openair0_cfg->rx_gain_offset[0]) {
        LOG_E(HW, "[oxgrf] Reduce RX Gain 0 by %f dB\n", openair0_cfg->rx_gain[0] - openair0_cfg->rx_gain_offset[0] - 65);
        return -1;
    }

    if ((ret = oxgrf_set_rx1_rf_gain(oxgrf->dev, 0, (uint32_t)(openair0_cfg->rx_gain[0] - openair0_cfg->rx_gain_offset[0]))) < 0) {
        LOG_I(HW, "[oxgrf] Failed to set RX1 gain\n");
    } else
        LOG_I(HW, "[oxgrf] set RX1 gain to %u\n",(uint32_t)(openair0_cfg->rx_gain[0]));

    if(oxgrf->rx_num_channels > 1) {
        if ((ret = oxgrf_set_rx2_rf_gain(oxgrf->dev, 0, (uint32_t)(openair0_cfg->rx_gain[1] - openair0_cfg->rx_gain_offset[0]))) < 0) {
            LOG_E(HW, "[oxgrf] Failed to set RX2 gain\n");
        } else
            LOG_I(HW, "[oxgrf] set RX gain to %u\n",(uint32_t)(openair0_cfg->rx_gain[1]));
    }
#if 0
    if ((ret = oxgrf_set_tx1_attenuation(oxgrf->dev, 0, openair0_cfg->tx_gain[0] * 1000)) < 0) {
        LOG_E(HW, "[oxgrf] Failed to set TX1 gain\n");
    } else
        LOG_I(HW, "[oxgrf] set the TX1 gain to %d\n", 90 - (uint32_t)openair0_cfg->tx_gain[0]);

    if(oxgrf->tx_num_channels > 1) {
        if ((ret = oxgrf_set_tx2_attenuation(oxgrf->dev, 0, openair0_cfg->tx_gain[1] * 1000)) < 0) {
            LOG_E(HW, "[oxgrf] Failed to set TX2 gain\n");
        } else
            LOG_I(HW, "[oxgrf] set the TX2 gain to %d\n", 90 - (uint32_t)openair0_cfg->tx_gain[1]);
    }
#endif
    return(ret);
}

/*! \brief Initialize Openair oxgrf target. It returns 0 if OK
 * \param device the hardware to use
 * \param openair0_cfg RF frontend parameters set by application
 * \returns 0 on success
 */
int device_init(openair0_device *device, openair0_config_t *openair0_cfg) {

    int status;

    oxgrf_state_t *oxgrf = (oxgrf_state_t*)malloc(sizeof(oxgrf_state_t));
    memset(oxgrf, 0, sizeof(oxgrf_state_t));

    LOG_I(HW, "[oxgrf] openair0_cfg[0].sdr_addrs == '%s'\n", openair0_cfg[0].sdr_addrs);
    LOG_I(HW, "[oxgrf] openair0_cfg[0].rx_num_channels == '%d'\n", openair0_cfg[0].rx_num_channels);
    LOG_I(HW, "[oxgrf] openair0_cfg[0].tx_num_channels == '%d'\n", openair0_cfg[0].tx_num_channels);

    openair0_cfg[0].rx_gain_calib_table = calib_table_oxgrf;
    set_rx_gain_offset(openair0_cfg, 0);
    if(oxgrf->rx_num_channels > 1)
        set_rx_gain_offset(openair0_cfg, 1);
    openair0_cfg->iq_txshift = 0;
    openair0_cfg->iq_rxrescale = 15; /*not sure*/ //FIXME: adjust to oxgrf
    oxgrf->sample_rate = (unsigned int)openair0_cfg->sample_rate;
    LOG_I(HW, "[oxgrf] sampling_rate %d\n", oxgrf->sample_rate);
    oxgrf->rx_num_channels = openair0_cfg[0].rx_num_channels;
    oxgrf->tx_num_channels = openair0_cfg[0].tx_num_channels;

    RX_MTU = openair0_cfg->sample_rate / 1000 / 2;
    if(RX_MTU > 30720)
        RX_MTU = 30720;
    else if(!(RX_MTU % 5760))
        RX_MTU = 5760;

    bool rx_ant = true;
    bool pa_status = false;
    int auxdac1 = 0;
    char args[64];
    if (openair0_cfg[0].sdr_addrs == NULL) {
        strcpy(args, "dev=pciex:0");
    } else {
        strcpy(args, openair0_cfg[0].sdr_addrs);
    }

    char dev_str[64];
    const char dev_arg[] = "dev=";
    char *dev_ptr = strstr(args, dev_arg);
    if(dev_ptr) {
        copy_subdev_string(dev_str, dev_ptr + strlen(dev_arg));
        remove_substring(args, dev_arg);
        remove_substring(args, dev_str);
        LOG_I(HW, "[oxgrf] Using %s\n", dev_str);
    }

    const char auxdac1_arg[] = "auxdac1=";
    char auxdac1_str[64] = {0};
    char *auxdac1_ptr = strstr(args, auxdac1_arg);
    if(auxdac1_ptr) {
        copy_subdev_string(auxdac1_str, auxdac1_ptr + strlen(auxdac1_arg));
        remove_substring(args, auxdac1_arg);
        remove_substring(args, auxdac1_str);
        auxdac1 = atoi(auxdac1_str);
        LOG_I(HW, "[oxgrf] Setting auxdac1:%u\n", auxdac1);
    }

    const char pa_arg[] = "pa=";
    char pa_str[64] = {0};
    char *pa_ptr = strstr(args, pa_arg);
    if(pa_ptr) {
        copy_subdev_string(pa_str, pa_ptr + strlen(pa_arg));
        remove_substring(args, pa_arg);
        remove_substring(args, pa_str);
        pa_status = !strcmp(pa_str, "enabled");
        LOG_I(HW, "[oxgrf] PA Status:%s\n", pa_status?"Enabled":"Disabled");
    }

    const char ant_arg[] = "rx_ant=";
    char ant_str[64] = {0};
    char *ant_ptr = strstr(args, ant_arg);
    if(ant_ptr) {
        copy_subdev_string(ant_str, ant_ptr + strlen(ant_arg));
        remove_substring(args, ant_arg);
        remove_substring(args, ant_str);
        rx_ant = strcmp(ant_str, "trx");
    }

    char dstring[128];
    if(RX_MTU < 30720) {
        sprintf(dstring, ",nsamples_recv_frame:%u", RX_MTU);
        strcat(dev_str, dstring);
    }
    if ((oxgrf->dev = oxgrf_open_device(dev_str)) == NULL ) {
        LOG_E(HW, "[oxgrf] Failed to open oxgrf\n");
        free(oxgrf);
        return -1;
    }

    uint32_t model = 0;
    oxgrf_get_model_version(oxgrf->dev, &model);
    model &= 0xffff;
    if(model == 550) {
        oxgrf->BoardType = Y550;
        shift = 4;
        LOG_I(HW, "[oxgrf] device type: Y%d\n", model);
    } else if(model == 230) {
        oxgrf->BoardType = Y230;
        shift = 4;
        LOG_I(HW, "[oxgrf] device type: Y%d\n", model);
    } else if(model == 380) {
        oxgrf->BoardType = Y380;
        shift = 4;
        LOG_I(HW, "[oxgrf] device type: Y%d\n", model);
    } else if(model == 590) {
        oxgrf->BoardType = Y590;
        shift = 2;
        LOG_I(HW, "[oxgrf] device type: Y%d\n", model);
    } else if(model == 7400) {
        oxgrf->BoardType = IQX7400;
        shift = 2;
        LOG_I(HW, "[oxgrf] device type: IQX%d\n", model);
    } else if(model == 7402) {
        oxgrf->BoardType = IQX7402;
        shift = 2;
        LOG_I(HW, "[oxgrf] device type: IQX%d(Split Mode)\n", model - 2);
    } else if(model == 6000 || model == 7000 || model == 7100) {
        oxgrf->BoardType = IQX7000;
        shift = 4;
        LOG_I(HW, "[oxgrf] device type: IQX%d\n", model);
    } else {
        oxgrf->BoardType = UNKNOWN;
        LOG_I(HW, "[oxgrf] device type: unknown\n");
    }

    if(oxgrf->BoardType == Y230) {
        switch ((int)openair0_cfg->sample_rate) {
#if 0
        case 61440000:
            openair0_cfg->samples_per_packet    = 30720;
            openair0_cfg->tx_sample_advance     = 80;
            openair0_cfg[0].tx_bw               = 40e6;
            openair0_cfg[0].rx_bw               = 40e6;
            break;
        case 46080000:
            openair0_cfg->samples_per_packet    = 23040;
            openair0_cfg->tx_sample_advance     = 80;
            openair0_cfg[0].tx_bw               = 40e6;
            openair0_cfg[0].rx_bw               = 40e6;
            break;
#endif
        case 30720000:
            openair0_cfg->samples_per_packet    = 15360;
            openair0_cfg->tx_sample_advance     = 80;
            openair0_cfg[0].tx_bw               = 20e6;
            openair0_cfg[0].rx_bw               = 20e6;
            break;
        case 23040000:
            openair0_cfg->samples_per_packet    = 11520;
            openair0_cfg->tx_sample_advance     = 80;
            openair0_cfg[0].tx_bw               = 20e6;
            openair0_cfg[0].rx_bw               = 20e6;
            break;
        case 15360000:
            openair0_cfg->samples_per_packet    = 7680;
            openair0_cfg->tx_sample_advance     = 52;
            openair0_cfg[0].tx_bw               = 10e6;
            openair0_cfg[0].rx_bw               = 10e6;
            break;
        case 7680000:
            openair0_cfg->samples_per_packet    = 7680;
            openair0_cfg->tx_sample_advance     = 34;
            openair0_cfg[0].tx_bw               = 5e6;
            openair0_cfg[0].rx_bw               = 5e6;
            break;
        case 1920000:
            openair0_cfg->samples_per_packet    = 1920;
            openair0_cfg->tx_sample_advance     = 9;
            openair0_cfg[0].tx_bw               = 1.25e6;
            openair0_cfg[0].rx_bw               = 1.25e6;
            break;
        default:
            LOG_I(HW, "[oxgrf] Error: unknown sampling rate %f\n", openair0_cfg->sample_rate);
            free(oxgrf);
            exit(-1);
            break;
        }
    }
    if(oxgrf->BoardType == Y380
            || oxgrf->BoardType == Y550
            || oxgrf->BoardType == Y590
            || oxgrf->BoardType == IQX7000
            || oxgrf->BoardType == IQX7402
            || oxgrf->BoardType == IQX7400) {
        switch ((int)openair0_cfg->sample_rate) {
        case 122880000:
            openair0_cfg->samples_per_packet    = 30720;
            openair0_cfg->tx_sample_advance     = 80;
            openair0_cfg[0].tx_bw               = 100e6;
            openair0_cfg[0].rx_bw               = 100e6;
            break;
        case 61440000:
            openair0_cfg->samples_per_packet    = 30720;
            openair0_cfg->tx_sample_advance     = 80;
            openair0_cfg[0].tx_bw               = 40e6;
            openair0_cfg[0].rx_bw               = 40e6;
            break;
        case 30720000:
            openair0_cfg->samples_per_packet    = 15360;
            openair0_cfg->tx_sample_advance     = 80;
            openair0_cfg[0].tx_bw               = 20e6;
            openair0_cfg[0].rx_bw               = 20e6;
            break;
        case 15360000:
            openair0_cfg->samples_per_packet    = 15360;
            openair0_cfg->tx_sample_advance     = 52;
            openair0_cfg[0].tx_bw               = 10e6;
            openair0_cfg[0].rx_bw               = 10e6;
            break;
        case 7680000:
            openair0_cfg->samples_per_packet    = 7680;
            openair0_cfg->tx_sample_advance     = 34;
            openair0_cfg[0].tx_bw               = 5e6;
            openair0_cfg[0].rx_bw               = 5e6;
            break;
        case 1920000:
            openair0_cfg->samples_per_packet    = 1920;
            openair0_cfg->tx_sample_advance     = 9;
            openair0_cfg[0].tx_bw               = 1.25e6;
            openair0_cfg[0].rx_bw               = 1.25e6;
            break;
        default:
            LOG_I(HW, "[oxgrf] Error: unknown sampling rate %f\n", openair0_cfg->sample_rate);
            free(oxgrf);
            exit(-1);
            break;
        }

    }

    LOG_I(HW, "[oxgrf] Initializing openair0_device\n");
    switch (openair0_cfg[0].clock_source) {
    case external:
        LOG_I(HW, "[oxgrf] clock_source: external\n");
        oxgrf_set_ref_clock (oxgrf->dev, 0, EXTERNAL_REFERENCE);
        oxgrf_set_pps_select (oxgrf->dev, 0, PPS_EXTERNAL_EN);
        break;
    case gpsdo:
        LOG_I(HW, "[oxgrf] clock_source: gpsdo\n");
        break;
    case internal:
    default:
        oxgrf_set_ref_clock (oxgrf->dev, 0, INTERNAL_REFERENCE);
        oxgrf_set_pps_select (oxgrf->dev, 0, PPS_INTERNAL_EN);
        //oxgrf_set_vco_select (oxgrf->dev, 0, AUXDAC1);
        LOG_I(HW, "[oxgrf] clock_source: internal\n");
        break;
    }
    oxgrf_set_auxdac1 (oxgrf->dev, 0, auxdac1);
    if (pa_status) {
        oxgrf_set_duplex_select (oxgrf->dev, 0, FDD);
        oxgrf_set_trxsw_fpga_enable(oxgrf->dev, 0, 1);
        oxgrf_set_rx_ant_enable (oxgrf->dev, 0, 0);
    } else if (openair0_cfg->duplex_mode == duplex_mode_TDD && !rx_ant) {
        oxgrf_set_duplex_select (oxgrf->dev, 0, TDD);
        oxgrf_set_trxsw_fpga_enable(oxgrf->dev, 0, 1);
        oxgrf_set_rx_ant_enable (oxgrf->dev, 0, 0);
    } else {
        oxgrf_set_duplex_select (oxgrf->dev, 0, FDD);
        oxgrf_set_trxsw_fpga_enable(oxgrf->dev, 0, 0);
        oxgrf_set_rx_ant_enable (oxgrf->dev, 0, 1);
    }
    LOG_I(HW, "[oxgrf] RX Ant:%s\n", rx_ant?"Enabled":"Disabled");
    oxgrf_set_tx_fir_en_dis (oxgrf->dev, 0, 0);
    oxgrf_set_rx_fir_en_dis (oxgrf->dev, 0, 0);

    int max_idx = ((oxgrf->rx_num_channels > 2 || oxgrf->tx_num_channels > 2)? 1 : 0);

    for(int chip_idx = 0; chip_idx <= max_idx; chip_idx++) {
        // RX port Initialize
        if ((status = oxgrf_set_rx_lo_freq(oxgrf->dev, chip_idx, (uint64_t)(openair0_cfg->rx_freq[0]))) < 0) {
            LOG_E(HW, "[oxgrf] Failed to set RX frequency\n");
        } else
            LOG_I(HW, "[oxgrf] set RX frequency to %lu\n",(uint64_t)(openair0_cfg->rx_freq[0]));
        if ((status = oxgrf_set_rx_sampling_freq(oxgrf->dev, chip_idx, (uint32_t)(openair0_cfg->sample_rate))) < 0) {
            LOG_E(HW, "[oxgrf] Failed to set RX sample rate\n");
        } else
            LOG_I(HW, "[oxgrf] set RX sample rate to %u\n", (uint32_t)(openair0_cfg->sample_rate));
        if ((status = oxgrf_set_rx_rf_bandwidth(oxgrf->dev, chip_idx, (uint32_t)(openair0_cfg->rx_bw))) < 0) {
            LOG_E(HW, "[oxgrf] Failed to set RX bandwidth\n");
        } else
            LOG_I(HW, "[oxgrf] set RX bandwidth to %u\n",(uint32_t)(openair0_cfg->rx_bw));

        if ((status = oxgrf_set_rx1_gain_control_mode(oxgrf->dev, chip_idx, 0)) < 0){
            LOG_E(HW, "[oxgrf] Failed to set RX1 Gain Control Mode\n");
        } else
            LOG_I(HW, "[oxgrf] set RX1 Gain Control Mode MGC\n");

        uint32_t rxgain = openair0_cfg->rx_gain[0] - openair0_cfg->rx_gain_offset[0];
        if (rxgain > 30)
            rxgain = 60;
        else
            rxgain *= 2;
        if ((status = oxgrf_set_rx1_rf_gain(oxgrf->dev, chip_idx, rxgain)) < 0) {
            LOG_I(HW, "[oxgrf] Failed to set RX1 gain\n");
        } else
            LOG_I(HW, "[oxgrf] set RX1 gain to %u\n", rxgain);

        if ((status = oxgrf_set_rx2_gain_control_mode(oxgrf->dev, chip_idx, 0)) < 0){
            LOG_E(HW, "[oxgrf] Failed to set RX2 Gain Control Mode\n");
        } else
            LOG_I(HW, "[oxgrf] set RX2 Gain Control Mode MGC\n");

        if ((status = oxgrf_set_rx2_rf_gain(oxgrf->dev, chip_idx, rxgain)) < 0) {
            LOG_E(HW, "[oxgrf] Failed to set RX2 gain\n");
        } else
            LOG_I(HW, "[oxgrf] set RX2 gain to %u\n", rxgain);

        // TX port Initialize
        if ((status = oxgrf_set_tx_lo_freq(oxgrf->dev, chip_idx, (uint64_t)openair0_cfg->tx_freq[0])) < 0) {
            LOG_E(HW, "[oxgrf] Failed to set TX frequency\n");
        } else
            LOG_I(HW, "[oxgrf] set TX Frequency to %lu\n", (uint64_t)openair0_cfg->tx_freq[0]);

        if ((status = oxgrf_set_tx_sampling_freq(oxgrf->dev, chip_idx, (uint32_t)openair0_cfg->sample_rate)) < 0) {
            LOG_E(HW, "[oxgrf] Failed to set TX sample rate\n");
        } else
            LOG_I(HW, "[oxgrf] set TX sampling rate to %u\n", (uint32_t)openair0_cfg->sample_rate);

        if ((status = oxgrf_set_tx_rf_bandwidth(oxgrf->dev, chip_idx, (uint32_t)openair0_cfg->tx_bw)) <0) {
            LOG_E(HW, "[oxgrf] Failed to set TX bandwidth\n");
        } else
            LOG_I(HW, "[oxgrf] set TX bandwidth to %u\n", (uint32_t)openair0_cfg->tx_bw);

        if ((status = oxgrf_set_tx1_attenuation(oxgrf->dev, chip_idx, openair0_cfg->tx_gain[0] * 1000)) < 0) {
            LOG_E(HW, "[oxgrf] Failed to set TX1 gain\n");
        } else
            LOG_I(HW, "[oxgrf] set the TX1 gain to %d\n", 90 - (uint32_t)openair0_cfg->tx_gain[0]);

        if ((status = oxgrf_set_tx2_attenuation(oxgrf->dev, chip_idx, openair0_cfg->tx_gain[1] * 1000)) < 0) {
            LOG_E(HW, "[oxgrf] Failed to set TX2 gain\n");
        } else
            LOG_I(HW, "[oxgrf] set the TX2 gain to %d\n", 90 - (uint32_t)openair0_cfg->tx_gain[1]);
    }

    uint32_t depth = oxgrf->sample_rate / 10 * sizeof(int) * oxgrf->tx_num_channels;
    oxgrf_set_hwbuf_depth(oxgrf->dev, 0, depth);

    oxgrf_enable_timestamp(oxgrf->dev, 0, 0);
    sleep(2);
    oxgrf_enable_timestamp(oxgrf->dev, 0, 1);
    sleep(2);

    for(int i = 0; i < NCHAN_PER_DEV; i++) {
        int ret = posix_memalign((void **)&cache_buf[i], 4096, BUFFER_SIZE);
        if(ret) {
            LOG_I(HW, "Failed to alloc memory\n");
            return -1;
        }
        ret = posix_memalign((void **)&iq_buf[i], 4096, BUFFER_SIZE);
        if(ret) {
            LOG_I(HW, "Failed to alloc memory\n");
            return -1;
        }
    }

    device->Mod_id               = num_devices++;
    device->type                 = OXGRF_DEV;
    device->trx_start_func       = trx_oxgrf_start;
    device->trx_end_func         = trx_oxgrf_end;
    device->trx_read_func        = trx_oxgrf_read;
    device->trx_write_func       = trx_oxgrf_write;
    device->trx_get_stats_func   = trx_oxgrf_get_stats;
    device->trx_reset_stats_func = trx_oxgrf_reset_stats;
    device->trx_stop_func        = trx_oxgrf_stop;
    device->trx_set_freq_func    = trx_oxgrf_set_freq;
    device->trx_set_gains_func   = trx_oxgrf_set_gains;
    device->openair0_cfg         = openair0_cfg;
    device->priv                 = (void *)oxgrf;

    return 0;
}

/*@}*/
