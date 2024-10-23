
#include <rte_eal.h>
#include <rte_ethdev.h>
#include <rte_mbuf.h>
#include <rte_malloc.h>
#include <rte_timer.h>
#include<sys/types.h>
#include<sys/stat.h>
#include<fcntl.h>
#include <stdlib.h>
#include<unistd.h>
#include <sched.h>
#include <pthread.h>
#include <string.h> /**> memset */
#include <signal.h>
#include <termios.h>
#include <rte_eal.h> /**> rte_eal_init */
#include <rte_debug.h> /**> for rte_panic */
#include <rte_errno.h> /**> rte_errno global var */
#include <rte_memzone.h> /**> rte_memzone_dump */
#include <rte_memcpy.h>
#include <rte_string_fns.h>
#include <rte_spinlock.h>
#include <time.h>  /** For SLEEP **/
#include <getopt.h>
#include <stdio.h>
#include <arpa/inet.h>
#include <stdbool.h>
#include <inttypes.h>
#include <sys/time.h>
#include <math.h>
#include "dpdkrf_oai.h"
#include "dpdkrf_if.h"
#include "common/utils/LOG/log.h"
#include "common/utils/LOG/vcd_signal_dumper.h"
#define SAMPLE_PER_PACKAGE 960


int num_devices = 0;

#define DEFAULT_NUM 61440
static uint8_t cache_rx[4][SAMPLE_PER_PACKAGE*512];
static uint8_t cache_tx[4][SAMPLE_PER_PACKAGE*512];
#define CTRL_BUF_NUM 64
int32_t gTxCtrlBuf[SAMPLE_PER_PACKAGE * 2 * CTRL_BUF_NUM] __attribute__((aligned(8)));
int32_t gRxCtrlBuf[SAMPLE_PER_PACKAGE * 2 * CTRL_BUF_NUM] __attribute__((aligned(8)));

int gRxCtrlIdx = 0;
int gRxCtrlIdxRead = 0;

int g_syncing = -1;

int32_t test_dpdkrf_read (openair0_device *device, openair0_timestamp *ptimestamp, void **buff1, int nsamps, int cc){

	void *buff[4];
	for (int i = 0; i < cc; i++)
	{
		buff[i] = buff1[i];
	}

	for (int i = cc; i < 4; i++)
	{
		buff[i] = cache_rx[1];
	}
VCD_SIGNAL_DUMPER_DUMP_FUNCTION_BY_NAME( VCD_SIGNAL_DUMPER_FUNCTIONS_TRX_READ_RF, 1 );
	int32_t rr = trx_dpdkrf_read((uint8_t **)buff, nsamps, (uint64_t *)ptimestamp,cc);
VCD_SIGNAL_DUMPER_DUMP_FUNCTION_BY_NAME( VCD_SIGNAL_DUMPER_FUNCTIONS_TRX_READ_RF, 0 );
    return nsamps;

}

int32_t test_dpdkrf_write (openair0_device *device,openair0_timestamp timestamp, void **buff1, int nsamps, int cc, int flags){

  	void *buff[4];
	for (int i = 0; i < cc; i++)
	{
		buff[i] = buff1[i];
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
	for (int i = cc; i < 4; i++)
	{
		buff[i] = cache_tx[1];;
	}

	int32_t rr = trx_dpdkrf_write((uint8_t **)buff,nsamps, timestamp, cc);
	return nsamps;
}


int trx_dpdkrf_start(openair0_device *device) {
    openair0_config_t *openair0_cfg;
	//uint8_t **txpCtrlgain = rte_malloc(NULL, 4 * sizeof(uint8_t), 0);
	//for (int i = 0; i < 4; i++)
	//{
	//	txpCtrlgain[i] = (uint8_t *)gTxCtrlBuf;
	//}
	LOG_I(HW, "[dpdkrf] Start dpdkrf ...\n");
    sleep(2);
    openair0_cfg = device->openair0_cfg;
#if 1
	dpdk_device_start(3,4, 5000, openair0_cfg->tx_num_channels, openair0_cfg->rx_num_channels);
    //printf("tx txbase %p, txbase[0] %p, &txbase[0][0] %p,  rx addr %p ,rxbase[0] %p, &rxbase[0][0] %p\n", 
    //openair0_cfg->txbase, 
    //openair0_cfg->txbase[0],
   // &openair0_cfg->txbase[0][0],
   // openair0_cfg->rxbase,
   // openair0_cfg->rxbase[0],
   // &openair0_cfg->rxbase[0][0]);
	//sleep(1);
    //sleep(1);
    //printf("trx_dpdkrf_start FREQ NOT SET!!!!!!!\n\n\n\n\n\n\n\n\n\n\n\n");
	
    trx_dpdkrf_setFreq((int64_t)openair0_cfg->rx_freq[0], (int64_t)openair0_cfg->tx_freq[0], 0);
    printf("2freq = %ld %ld\n\n",(int64_t)(openair0_cfg->rx_freq[0]), (int64_t)(openair0_cfg->tx_freq[0]));

    //trx_dpdkrf_setFreq(3349381000, 3349381000, 0);
    
	trx_dpdkrf_gainTx(openair0_cfg->tx_gain[0], openair0_cfg->tx_gain[0], openair0_cfg->tx_gain[0], openair0_cfg->tx_gain[0], 0);
	//sleep(1);
	trx_dpdkrf_gainRx(openair0_cfg->rx_gain[0], openair0_cfg->rx_gain[0], openair0_cfg->rx_gain[0], openair0_cfg->rx_gain[0], 0);
	printf("txgains (0-30) %d, rxgains(30-0) %d\n", (int)(openair0_cfg->tx_gain[0]), (int)(openair0_cfg->rx_gain[0]));
    

#endif	
	return 0;
}

void trx_dpdkrf_end(openair0_device *device){
}
int trx_dpdkrf_get_stats(openair0_device* device) {
    return(0);
}
int trx_dpdkrf_reset_stats(openair0_device* device) {
    return(0);
}
int trx_dpdkrf_stop(openair0_device* device) {
    return(0);
}
int trx_dpdkrf_set_freq(openair0_device* device, openair0_config_t *openair0_cfg) {
    printf("trx_dpdkrf_set_freq \n");
    printf("1freq = %ld %ld\n\n",(int64_t)(openair0_cfg->rx_freq[0]), (int64_t)(openair0_cfg->tx_freq[0]));
    // uint8_t **txpCtrl = rte_malloc(NULL, 4 * sizeof(uint8_t), 0);
    // trx_setMsgFreq(gTxCtrlBuf, (uint64_t)(openair0_cfg->rx_freq[0]),(uint64_t)(openair0_cfg->tx_freq[0]));
	// txpCtrl[0] = (uint8_t *)gTxCtrlBuf;
	// trx_dpdkrf_write_ctrl(txpCtrl);
    printf("trx_dpdkrf_set_freq FREQ NOT SET!!!!!!!\n\n\n\n\n\n\n\n\n\n\n\n");
    //trx_dpdkrf_setFreq(openair0_cfg->rx_freq[0], openair0_cfg->tx_freq[0]);

    trx_dpdkrf_setFreq((int64_t)openair0_cfg->rx_freq[0], (int64_t)openair0_cfg->tx_freq[0], 0);

       
    return(0);
}
int trx_dpdkrf_set_gains(openair0_device* device, openair0_config_t *openair0_cfg) {

   #if 0
   if (openair0_cfg->rx_gain[0] > 65+openair0_cfg->rx_gain_offset[0]) {
        LOG_E(HW, "[oxgrf] Reduce RX Gain 0 by %f dB\n", openair0_cfg->rx_gain[0] - openair0_cfg->rx_gain_offset[0] - 65);
    return -1;
    }

    uint8_t **txpCtrl = rte_malloc(NULL, 4 * sizeof(uint8_t), 0);
    trx_setMsgGainRx(gTxCtrlBuf,(uint32_t)(openair0_cfg->rx_gain[0] > 65?65:openair0_cfg->rx_gain[0]),(uint32_t)(openair0_cfg->rx_gain[1] > 65?65:openair0_cfg->rx_gain[1]),100,100 );
    int tx_gain1 = ((uint32_t)openair0_cfg->tx_gain[0] > 90?90:(uint32_t)openair0_cfg->tx_gain[0]);
    int tx_gain2 = ((uint32_t)openair0_cfg->tx_gain[1] > 90?90:(uint32_t)openair0_cfg->tx_gain[1]);
    trx_setMsgGainTx(gTxCtrlBuf, (90 - tx_gain1) * 1000,(90 - tx_gain2) * 1000,100,100);
	txpCtrl[0] = (uint8_t *)gTxCtrlBuf;
	trx_dpdkrf_write_ctrl(txpCtrl);
    #endif
    printf("trx_dpdkrf_set_gains GAIN NOT SET!!!!!!!\n\n\n\n\n\n\n\n\n\n\n\n");
	//trx_dpdkrf_gainTx(openair0_cfg->tx_gain[0], openair0_cfg->tx_gain[0], openair0_cfg->tx_gain[0], openair0_cfg->tx_gain[0]);
	sleep(1);
	//trx_dpdkrf_gainTx(openair0_cfg->rx_gain[0], openair0_cfg->rx_gain[0], openair0_cfg->rx_gain[0], openair0_cfg->rx_gain[0]);
	printf("txgains (0-30) %d, rxgains(30-0) %d\n", (int)(openair0_cfg->tx_gain[0]), (int)(openair0_cfg->rx_gain[0]));

    return(0);
}


void *dpdk_init(void *arg) {
    printf("This is a new thread.\n");
	int argc = 3;
    char *argv[3];
    argv[0] = "modem";
    argv[1] = "-c";
    argv[2] = "0x1E";
	if (rte_eal_init(argc,argv) < 0) {
		rte_exit(EXIT_FAILURE, "Error with EAL init\n");
	}

    while(1)
        sleep(100);

    return NULL;
}

int device_init(openair0_device *device, openair0_config_t *openair0_cfg) {

    dpdkrf_state_t *dpdkrf = (dpdkrf_state_t*)malloc(sizeof(dpdkrf_state_t));
    memset(dpdkrf, 0, sizeof(dpdkrf_state_t));

    LOG_I(HW, "[dpdkrf] openair0_cfg[0].sdr_addrs == '%s'\n", openair0_cfg[0].sdr_addrs);
    LOG_I(HW, "[dpdkrf] openair0_cfg[0].rx_num_channels == '%d'\n", openair0_cfg[0].rx_num_channels);
    LOG_I(HW, "[dpdkrf] openair0_cfg[0].tx_num_channels == '%d'\n", openair0_cfg[0].tx_num_channels);
    // init required params
    switch ((int)openair0_cfg->sample_rate) {
    case 122880000:
        openair0_cfg->samples_per_packet    = 122880;
        openair0_cfg->tx_sample_advance     = 0;//1232;
        openair0_cfg[0].tx_bw               = 100e6;
        openair0_cfg[0].rx_bw               = 100e6;
        break;
    case 61440000:
        openair0_cfg->samples_per_packet    = 61440;
        openair0_cfg->tx_sample_advance     = 70;
        openair0_cfg[0].tx_bw               = 40e6;
        openair0_cfg[0].rx_bw               = 40e6;
        break;
    case 30720000:
        openair0_cfg->samples_per_packet    = 30720;
        openair0_cfg->tx_sample_advance     = 70;
        openair0_cfg[0].tx_bw               = 20e6;
        openair0_cfg[0].rx_bw               = 20e6;
        break;
    case 15360000:
        openair0_cfg->samples_per_packet    = 15360;
        openair0_cfg->tx_sample_advance     = 68;
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
        LOG_I(HW, "[dpdkrf] Error: unknown sampling rate %f\n", openair0_cfg->sample_rate);
        free(dpdkrf);
        exit(-1);
        break;
    }


    dpdkrf->sample_rate = (unsigned int)openair0_cfg->sample_rate;
    LOG_I(HW, "[rf] sampling_rate %d\n", dpdkrf->sample_rate);
    dpdkrf->rx_num_channels = openair0_cfg[0].rx_num_channels;
    dpdkrf->tx_num_channels = openair0_cfg[0].tx_num_channels;


	for (int i = 0; i < 1920 * 2; i++)
		gTxCtrlBuf[i] = i;


    pthread_t thread_id;
    int ret;

    ret = pthread_create(&thread_id, NULL, dpdk_init, NULL);
    if (ret!= 0) {
        fprintf(stderr, "Error creating thread dpdk_init.\n");
        return 1;
    }
    ret = rte_thread_setname(thread_id,"rte_telemetry");	

    device->Mod_id               = num_devices++;
    device->type                 = DPDKRF_DEV;
    device->trx_start_func       = trx_dpdkrf_start;
    device->trx_end_func         = trx_dpdkrf_end;
    device->trx_read_func        = test_dpdkrf_read;
    device->trx_write_func       = test_dpdkrf_write;
    device->trx_get_stats_func   = trx_dpdkrf_get_stats;
    device->trx_reset_stats_func = trx_dpdkrf_reset_stats;
    device->trx_stop_func        = trx_dpdkrf_stop;
    device->trx_set_freq_func    = trx_dpdkrf_set_freq;
    device->trx_set_gains_func   = trx_dpdkrf_set_gains;
    device->openair0_cfg         = openair0_cfg;
    device->priv                 = (void *)dpdkrf;

#if 0
	Dpdk_device_init(14, 15, 2, 2 , 50000, 1);
	sleep(1);
	trx_dpdkrf_setFreq(openair0_cfg->rx_freq[0], openair0_cfg->tx_freq[0]);
    //trx_dpdkrf_setFreq(3349380000, 3349380000);
	sleep(1);
    trx_dpdkrf_gainTx(openair0_cfg->tx_gain[0], openair0_cfg->tx_gain[0], openair0_cfg->tx_gain[0], openair0_cfg->tx_gain[0]);
	sleep(1);
	trx_dpdkrf_gainTx(openair0_cfg->rx_gain[0], openair0_cfg->rx_gain[0], openair0_cfg->rx_gain[0], openair0_cfg->rx_gain[0]);
	printf("txgains (0-30) %d, rxgains(30-0) %d\n", (int)(openair0_cfg->tx_gain[0]), (int)(openair0_cfg->rx_gain[0]));

#endif
    // rte_timer_setup();
    return 0;
}
