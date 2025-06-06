#ifndef DPDKRF_IF_H
#define DPDKRF_IF_H
#include <stdint.h>
#include <unistd.h>

/**
 * init dpdk parameter, start dpdk thread.
 * @param tx_buff
 * 	二维指针，指向4个天线的发射数据buffer。每个buffer应可存储FRAME_SAMPLE_NUM个数据
 * @param rx_buff
 * 	二维指针，指向4个天线的接收数据buffer。
 * @param dpdkLoopCoreId
 *   core ID at which dpdk thread run.
 * @param startDelay_us
 *   dpdk thread start delay, unit is us.
 * @return
 *   -
 */
void dpdk_device_start(int dpdkLoopCoreId0, int dpdkLoopCoreId1, int startDelay, int antTxNum, int antRxNum, int dataRate, int attl);
/**
 * 驱动回调函数，在接收到整slot的数据后，驱动调用这个函数.函数内容可由用户改写。
 * @param rxSlotIdx
 * 	当前slot号，由驱动传入
 * @param logBuf
 * 	驱动log，用于显示驱动运行信息。
 * @return
 *   - 0
 */
int32_t trx_dpdkrf_read(uint8_t **buff, uint32_t nsamps, uint64_t *timestamp, int cc);
int32_t trx_dpdkrf_write(uint8_t **buff, int nsamps, uint64_t timestamp, int cc);

void trx_dpdkrf_gainRx(int g1, int g2, int g3, int g4, int op);
void trx_dpdkrf_gainTx(int g1, int g2, int g3, int g4, int op);
void trx_dpdkrf_setFreq(int64_t freqRx, int64_t freqTx, int op);
int trx_dpdkrf_getFreq(int64_t * freqRx, int64_t * freqTx, int op);
void trx_dpdkrf_getInfo(int op);
void trx_dpdkrf_setOsc(int Oscoffset, int op);
#endif // DPDKRF_IF_H
