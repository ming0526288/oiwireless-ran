/*
 * Licensed to the OpenAirInterface (OAI) Software Alliance under one or more
 * contributor license agreements.  See the NOTICE file distributed with
 * this work for additional information regarding copyright ownership.
 * The OpenAirInterface Software Alliance licenses this file to You under
 * the OAI Public License, Version 1.1  (the "License"); you may not use this file
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

#include "nr_sdap.h"
#include "assertions.h"
#include "utils.h"
#include <inttypes.h>
#include <pthread.h>
#include <stddef.h>
#include "nr_sdap_entity.h"
#include "common/utils/LOG/log.h"
#include "errno.h"
#include "rlc.h"
#include "tun_if.h"
#include "system.h"
#include <fcntl.h>    // for fcntl
#include <string.h>   // for strerror, memcpy
#include "common/utils/threadPool/notified_fifo.h"
#include <stdatomic.h>   // ? 计数器
#include <ctype.h>
#include "openair3/NGAP/ngap_gNB_ue_context.h"

notifiedFIFO_t ue_queue;
notifiedFIFO_t gnb_queue;
static void ipq_ue_init(void) { initNotifiedFIFO(&ue_queue); }
static void ipq_gnb_init(void) { initNotifiedFIFO(&gnb_queue); }
static pthread_once_t ipq_once = PTHREAD_ONCE_INIT;
static inline void ipq_ue_init_once(void){ pthread_once(&ipq_once, ipq_ue_init); }
static inline void ipq_gnb_init_once(void){ pthread_once(&ipq_once, ipq_gnb_init); }

// 统计用：push/pop 次数
static atomic_ulong ipq_push_cnt = 0;
static atomic_ulong ipq_pop_cnt  = 0;

// 消费者线程句柄（只启动一次）
static pthread_t ipq_consumer_th;
static bool ipq_consumer_started = false;

static void reblock_tun_socket(int fd)
{
  int f;

  f = fcntl(fd, F_GETFL, 0);
  f &= ~(O_NONBLOCK);
  if (fcntl(fd, F_SETFL, f) == -1) {
    LOG_E(PDCP, "fcntl(F_SETFL) failed on fd %d: errno %d, %s\n", fd, errno, strerror(errno));
  }
}


bool sdap_data_req(protocol_ctxt_t *ctxt_p,
                   const ue_id_t ue_id,
                   const srb_flag_t srb_flag,
                   const rb_id_t rb_id,
                   const mui_t mui,
                   const confirm_t confirm,
                   const sdu_size_t sdu_buffer_size,
                   unsigned char *const sdu_buffer,
                   const pdcp_transmission_mode_t pt_mode,
                   const uint32_t *sourceL2Id,
                   const uint32_t *destinationL2Id,
                   const uint8_t qfi,
                   const bool rqi,
                   const int pdusession_id) {
  nr_sdap_entity_t *sdap_entity;
  sdap_entity = nr_sdap_get_entity(ue_id, pdusession_id);

  if(sdap_entity == NULL) {
    LOG_E(SDAP, "%s:%d:%s: Entity not found with ue: 0x%"PRIx64" and pdusession id: %d\n", __FILE__, __LINE__, __FUNCTION__, ue_id, pdusession_id);
    return 0;
  }

  bool ret = sdap_entity->tx_entity(sdap_entity,
                                    ctxt_p,
                                    srb_flag,
                                    rb_id,
                                    mui,
                                    confirm,
                                    sdu_buffer_size,
                                    sdu_buffer,
                                    pt_mode,
                                    sourceL2Id,
                                    destinationL2Id,
                                    qfi,
                                    rqi);
  return ret;
}

void sdap_data_ind(rb_id_t pdcp_entity,
                   int is_gnb,
                   bool has_sdap_rx,
                   int pdusession_id,
                   ue_id_t ue_id,
                   char *buf,
                   int size) {
  nr_sdap_entity_t *sdap_entity;
  sdap_entity = nr_sdap_get_entity(ue_id, pdusession_id);

  if (sdap_entity == NULL) {
    LOG_E(SDAP, "%s:%d:%s: Entity not found for ue rnti/ue_id: %lx and pdusession id: %d\n", __FILE__, __LINE__, __FUNCTION__, ue_id, pdusession_id);
    return;
  }

  sdap_entity->rx_entity(sdap_entity,
                         pdcp_entity,
                         is_gnb,
                         has_sdap_rx,
                         pdusession_id,
                         ue_id,
                         buf,
                         size);
}

//打印全部
static void *sdap_ue_fifo_consumer(void *arg)
{
  (void)arg;
  LOG_I(SDAP, "[FIFO] consumer thread started\n");
  for (;;) {
    notifiedFIFO_elt_t *elt = pullNotifiedFIFO(&ue_queue); // 阻塞等待
    if (!elt) continue;

    // 取指针+长度（不同分支：有的是宏，有的是结构成员）
    uint8_t *p = (uint8_t*)NotifiedFifoData(elt);
    uint32_t  nlen = 0;
    memcpy(&nlen, p, sizeof(uint32_t));
    uint8_t  *data = p + sizeof(uint32_t);

    // char hex[3*nlen+1]; // 你要把 nlen 个字节的二进制数据，用十六进制字符串打印出来，每个字节会变成 2 个十六进制字符 + 1 个空格
    // for (int i = 0; i < nlen; i++) sprintf(hex + 3*i, "%02X ", data[i]);
    // if (nlen > 0) hex[3*nlen-1] = '\0'; else hex[0] = '\0';

    unsigned long pops = atomic_fetch_add_explicit(&ipq_pop_cnt, 1, memory_order_relaxed) + 1;
    LOG_I(SDAP, "[FIFO] pop#%lu len=%u\n", pops, nlen);
    // LOG_I(SDAP, "%s\n", hex);

    // log_dump(SDAP, data, nlen, LOG_DUMP_C16, "\n");
    log_dump(SDAP, data, nlen, LOG_DUMP_CHAR, "\n");

    delNotifiedFIFO_elt(elt);
  }
  return NULL;
}

// static void *sdap_gnb_fifo_consumer(void *arg)
// {
//   (void)arg;
//   LOG_I(SDAP, "[FIFO] consumer thread started\n");
//   for (;;) {
//     notifiedFIFO_elt_t *elt = pullNotifiedFIFO(&gnb_queue); // 阻塞等待
//     if (!elt) continue;

//     // 取指针+长度（不同分支：有的是宏，有的是结构成员）
//     uint8_t *p = (uint8_t*)NotifiedFifoData(elt);
//     uint32_t  nlen = 0;
//     memcpy(&nlen, p, sizeof(uint32_t));
//     uint8_t  *data = p + sizeof(uint32_t);

//     // char hex[3*nlen+1]; // 你要把 nlen 个字节的二进制数据，用十六进制字符串打印出来，每个字节会变成 2 个十六进制字符 + 1 个空格，整串末尾还要一个 '\0' 作为 C 字符串结束符
//     // for (int i = 0; i < nlen; i++) sprintf(hex + 3*i, "%02X ", data[i]); // 用sprintf把每个字节格式化成十六进制字符串，"%02X " → 2 个十六进制位 + 1 个空格
//     // if (nlen > 0) hex[3*nlen-1] = '\0'; else hex[0] = '\0'; //把最后一个空格替换成 '\0'，这样日志里不会多出收尾空格。

//     unsigned long pops = atomic_fetch_add_explicit(&ipq_pop_cnt, 1, memory_order_relaxed) + 1;
//     LOG_I(SDAP, "[FIFO] pop#%lu len=%u\n", pops, nlen);
//     // LOG_I(SDAP, "%s\n", hex);

//     // log_dump(SDAP, data, nlen, LOG_DUMP_C16, "\n");
//     log_dump(SDAP, data, nlen, LOG_DUMP_CHAR, "\n");

//     delNotifiedFIFO_elt(elt);
//   }
//   return NULL;
// }

// 辅助函数：只启动一次消费者线程
static inline void maybe_start_ue_ipq_consumer(void)
{
  if (!ipq_consumer_started) {
    threadCreate(&ipq_consumer_th, sdap_ue_fifo_consumer, NULL,
                 "sdap_ue_fifo_consumer", -1, OAI_PRIORITY_RT_LOW);
    ipq_consumer_started = true;
  }
}

// static inline void maybe_start_gnb_ipq_consumer(void)
// {
//   if (!ipq_consumer_started) {
//     threadCreate(&ipq_consumer_th, sdap_gnb_fifo_consumer, NULL,
//                  "sdap_gnb_fifo_consumer", -1, OAI_PRIORITY_RT_LOW);
//     ipq_consumer_started = true;
//   }
// }

static void *sdap_tun_read_thread(void *arg)
{
  DevAssert(arg != NULL);
  nr_sdap_entity_t *entity = arg;

  char rx_buf[NL_MAX_PAYLOAD]; //rx_buf时接收缓冲区，用来存放从TUN网卡读到的数据包
  int len;
  reblock_tun_socket(entity->pdusession_sock);//用于设置TUN接口的socket为非阻塞模式(避免线程死等数据)

  int rb_id = 1;//假定RB ID = 1(无线承载ID)，在正式环境中，这个ID应该由上层(RRC配置)决定

  while (!entity->stop_thread) { //主循环持续运行，直到线程stop_thread置位或fd被关闭
    len = read(entity->pdusession_sock, &rx_buf, NL_MAX_PAYLOAD);//从TUN设备读取上行数据(即从用户态应用发来的IP包)
    if (len == -1) {
      if (errno == EINTR)
        continue; // interrupted system call信号终端，重试

      if (errno == EBADF || errno == EINVAL) {
        LOG_I(SDAP, "Socket closed, exiting TUN read thread for UE %ld, PDU session %d\n", entity->ue_id, entity->pdusession_id);
        break;
      }

      LOG_E(PDCP, "read() failed: errno %d (%s)\n", errno, strerror(errno));
      break;
    }

    if (len == 0) {
      LOG_W(SDAP, "TUN socket returned EOF - exiting thread\n");//如果返回0，表示退出线程
      break;
    }

    // // 创建 FIFO 元素并存入队列
    // notifiedFIFO_elt_t *elt = newNotifiedFIFO_elt(len, 0, NULL, NULL);  // 传输的长度和数据
    // memcpy(NotifiedFifoData(elt), rx_buf, len);  // 将读取到的 IP 数据包放入 FIFO 元素
    // pushNotifiedFIFO(&ip_queue, elt);  // 将数据包存入 FIFO 队列

    // LOG_I(SDAP, "[SDAP-TUN] read data of size %d\n", len);

    // ---- 入队：[长度(4B) + 数据] ----
    size_t total = sizeof(uint32_t) + (size_t)len; // 计算要分配的总字节数:预留4个字节存放"报文长度"(uint32_t),后面紧跟IP包数据len字节
    notifiedFIFO_elt_t *elt = newNotifiedFIFO_elt(total, 0, NULL, NULL);//向OAI的通知队列分配一个元素(FIFO节点)，数据区大小为total。后面两个NULL与回调相关这里不需要；0是优先级/标志位(实现里不用到就填0)
    uint8_t *p = (uint8_t*)NotifiedFifoData(elt);
    uint32_t nlen = (uint32_t)len;
    memcpy(p, &nlen, sizeof(uint32_t)); //把长度写道数据区开头的4个字节里。这里队列消费者拿到一块内存后，先读这4个字节就知道后面真正的报文长度是多少
    memcpy(p + sizeof(uint32_t), rx_buf, (size_t)len); // 把实际的IP包内容拷贝到长度字节之后，实现[长度|数据]格式
    pushNotifiedFIFO(&ue_queue, elt); //把这个元素压入之前初始化好的ue_queue。

    atomic_fetch_add_explicit(&ipq_push_cnt, 1, memory_order_relaxed);
    LOG_I(SDAP, "[SDAP-TUN] read data of size %d (push=%lu pop=%lu)\n",
          len,
          atomic_load_explicit(&ipq_push_cnt, memory_order_relaxed),
          atomic_load_explicit(&ipq_pop_cnt,  memory_order_relaxed));

    protocol_ctxt_t ctxt = {.enb_flag = entity->is_gnb, .rntiMaybeUEid = entity->ue_id}; //跨层传递上下文的结构，rntiMaybeUEid:在gNB侧表示UE的表示(RNTI)

    bool dc = entity->is_gnb ? false : SDAP_HDR_UL_DATA_PDU; //确定方向，若在gNB侧：dc = false，表示接收到上行数据；若在UE侧，dc = SDAP_UDR_UL_DATA_PDU,表示准备上行发送

    DevAssert(entity != NULL);
    entity->tx_entity(entity, //当前SDAP实体
                      &ctxt,  //协议上下文(包含RNTI/UE ID)
                      SRB_FLAG_NO,//表示这是数据承载(DRB),不是信令承载(SRB)
                      rb_id,  //无线承载ID
                      RLC_MUI_UNDEFINED, 
                      RLC_SDU_CONFIRM_NO,
                      len,    //数据长度
                      (unsigned char *)rx_buf, //要发送的数据内容
                      PDCP_TRANSMISSION_MODE_DATA, // 表示走PDCP数据模式
                      NULL,
                      NULL,
                      entity->qfi, //Qos Flow Identifier(用于 5G Qos流调度)
                      dc);// 数据方向标志(是否上行UL)
    LOG_I(SDAP, "[SDAP-TUN] UE ID=%" PRIu64 " PDU Session=%d len=%d\n",
      (uint64_t)entity->ue_id, entity->pdusession_id, len);
  }

  return NULL;
}

static void *sdap_direct_tun_read_thread(void *arg)
{
  DevAssert(arg != NULL);
  nr_sdap_entity_t *entity = arg;

  char rx_buf[NL_MAX_PAYLOAD]; //rx_buf时接收缓冲区，用来存放从TUN网卡读到的数据包
  int len;
  reblock_tun_socket(entity->pdusession_sock);//用于设置TUN接口的socket为非阻塞模式(避免线程死等数据)

  // int rb_id = 1;//假定RB ID = 1(无线承载ID)，在正式环境中，这个ID应该由上层(RRC配置)决定

  while (!entity->stop_thread) { //主循环持续运行，直到线程stop_thread置位或fd被关闭
    len = read(entity->pdusession_sock, &rx_buf, NL_MAX_PAYLOAD);//从TUN设备读取上行数据(即从用户态应用发来的IP包)
    if (len == -1) {
      if (errno == EINTR)
        continue; // interrupted system call信号终端，重试

      if (errno == EBADF || errno == EINVAL) {
        LOG_I(SDAP, "Socket closed, exiting TUN read thread for UE %ld, PDU session %d\n", entity->ue_id, entity->pdusession_id);
        break;
      }

      LOG_E(PDCP, "read() failed: errno %d (%s)\n", errno, strerror(errno));
      break;
    }

    if (len == 0) {
      LOG_W(SDAP, "TUN socket returned EOF - exiting thread\n");//如果返回0，表示退出线程
      break;
    }

    // // 创建 FIFO 元素并存入队列
    // notifiedFIFO_elt_t *elt = newNotifiedFIFO_elt(len, 0, NULL, NULL);  // 传输的长度和数据
    // memcpy(NotifiedFifoData(elt), rx_buf, len);  // 将读取到的 IP 数据包放入 FIFO 元素
    // pushNotifiedFIFO(&ip_queue, elt);  // 将数据包存入 FIFO 队列

    // LOG_I(SDAP, "[SDAP-TUN] read data of size %d\n", len);

    // ---- 入队：[长度(4B) + 数据] ----
    size_t total = sizeof(uint32_t) + (size_t)len; // 计算要分配的总字节数:预留4个字节存放"报文长度"(uint32_t),后面紧跟IP包数据len字节
    notifiedFIFO_elt_t *elt = newNotifiedFIFO_elt(total, 0, NULL, NULL);//向OAI的通知队列分配一个元素(FIFO节点)，数据区大小为total。后面两个NULL与回调相关这里不需要；0是优先级/标志位(实现里不用到就填0)
    uint8_t *p = (uint8_t*)NotifiedFifoData(elt);
    uint32_t nlen = (uint32_t)len;
    memcpy(p, &nlen, sizeof(uint32_t)); //把长度写道数据区开头的4个字节里。这里队列消费者拿到一块内存后，先读这4个字节就知道后面真正的报文长度是多少
    memcpy(p + sizeof(uint32_t), rx_buf, (size_t)len); // 把实际的IP包内容拷贝到长度字节之后，实现[长度|数据]格式
    pushNotifiedFIFO(&gnb_queue, elt); //把这个元素压入之前初始化好的gnb_queue。

    atomic_fetch_add_explicit(&ipq_push_cnt, 1, memory_order_relaxed);

    LOG_I(SDAP, "[SDAP-TUN] UE ID=%" PRIu64 " PDU Session=%d len=%d\n",
      (uint64_t)entity->ue_id, entity->pdusession_id, len);

    // const char *ue_ip = find_ue_ip_by_rnti((uint16_t)entity->pdusession_id);
    // LOG_I(SDAP, "[SDAP-TUN] UE IP=%s\n", ue_ip);


    LOG_I(SDAP, "[SDAP-TUN] read data of size %d (push=%lu pop=%lu)\n",
          len,
          atomic_load_explicit(&ipq_push_cnt, memory_order_relaxed),
          atomic_load_explicit(&ipq_pop_cnt,  memory_order_relaxed));

    // protocol_ctxt_t ctxt = {.enb_flag = entity->is_gnb, .rntiMaybeUEid = entity->ue_id}; //跨层传递上下文的结构，rntiMaybeUEid:在gNB侧表示UE的表示(RNTI)

    // bool dc = entity->is_gnb ? false : SDAP_HDR_UL_DATA_PDU; //确定方向，若在gNB侧：dc = false，表示接收到上行数据；若在UE侧，dc = SDAP_UDR_UL_DATA_PDU,表示准备上行发送

    // DevAssert(entity != NULL);
    // entity->tx_entity(entity, //当前SDAP实体
    //                   &ctxt,  //协议上下文(包含RNTI/UE ID)
    //                   SRB_FLAG_NO,//表示这是数据承载(DRB),不是信令承载(SRB)
    //                   rb_id,  //无线承载ID
    //                   RLC_MUI_UNDEFINED, 
    //                   RLC_SDU_CONFIRM_NO,
    //                   len,    //数据长度
    //                   (unsigned char *)rx_buf, //要发送的数据内容
    //                   PDCP_TRANSMISSION_MODE_DATA, // 表示走PDCP数据模式
    //                   NULL,
    //                   NULL,
    //                   entity->qfi, //Qos Flow Identifier(用于 5G Qos流调度)
    //                   dc);// 数据方向标志(是否上行UL)
  }

  return NULL;
}

void start_sdap_tun_gnb_first_ue_default_pdu_session(ue_id_t ue_id)
{
  nr_sdap_entity_t *entity = nr_sdap_get_entity(ue_id, get_softmodem_params()->default_pdu_session_id);
  DevAssert(entity != NULL);
  DevAssert(entity->is_gnb);
  char *ifprefix = get_softmodem_params()->nsa ? "oaitun_gnb" : "oaitun_enb";
  char ifname[IFNAMSIZ];
  tun_generate_ifname(ifname, ifprefix, ue_id - 1);
  entity->pdusession_sock = tun_alloc(ifname);
  tun_config(ifname, "10.0.1.1", NULL);
  threadCreate(&entity->pdusession_thread, sdap_tun_read_thread, entity, "gnb_tun_read_thread", -1, OAI_PRIORITY_RT_LOW);
}

void start_direct_sdap_tun_gnb_first_ue_default_pdu_session(ue_id_t ue_id)
{
  ipq_gnb_init_once();  //队列初始化（仅执行一次）
<<<<<<< HEAD
  //maybe_start_gnb_ipq_consumer();//启动消费者线程（只启动一次），进行测试打印队列内容.
=======
  // maybe_start_gnb_ipq_consumer();//启动消费者线程（只启动一次），进行测试打印队列内容
>>>>>>> e93ae2058c84fbf3a719b99327eeffd55fece7b4
  nr_sdap_entity_t *entity = nr_sdap_get_entity(ue_id, get_softmodem_params()->default_pdu_session_id);
  DevAssert(entity != NULL);
  DevAssert(entity->is_gnb);//启动基站侧的虚拟网卡
  char *ifprefix = get_softmodem_params()->nsa ? "oaitun_gnb" : "oaitun_enb";
  char ifname[IFNAMSIZ];
  tun_generate_ifname(ifname, ifprefix, 0);
  entity->pdusession_sock = tun_alloc(ifname);
  tun_config(ifname, "192.169.0.99", NULL);
  threadCreate(&entity->pdusession_thread, sdap_direct_tun_read_thread, entity, "gnb_tun_read_thread", -1, OAI_PRIORITY_RT_LOW);
}

void start_sdap_tun_ue(ue_id_t ue_id, int pdu_session_id, int sock)
{
  ipq_ue_init_once();  //队列初始化（仅执行一次）
  //maybe_start_ue_ipq_consumer();//启动消费者线程（只启动一次），进行测试打印队列内容
  nr_sdap_entity_t *entity = nr_sdap_get_entity(ue_id, pdu_session_id);
  DevAssert(entity != NULL);
  DevAssert(!entity->is_gnb);//UE侧的
  entity->pdusession_sock = sock;
  entity->stop_thread = false;
  char thread_name[64];
  snprintf(thread_name, sizeof(thread_name), "ue_tun_read_%ld_p%d", ue_id, pdu_session_id);
  threadCreate(&entity->pdusession_thread, sdap_tun_read_thread, entity, thread_name, -1, OAI_PRIORITY_RT_LOW);
}


void create_ue_ip_if(const char *ipv4, const char *ipv6, int ue_id, int pdu_session_id)
{
  int default_pdu = get_softmodem_params()->default_pdu_session_id;
  char ifname[IFNAMSIZ];
  tun_generate_ue_ifname(ifname, ue_id, pdu_session_id != default_pdu ? pdu_session_id : -1);
  const int sock = tun_alloc(ifname);
  tun_config(ifname, ipv4, ipv6);
  if (ipv4) {
    setup_ue_ipv4_route(ifname, ue_id, ipv4);
  }
  start_sdap_tun_ue(ue_id, pdu_session_id, sock); // interface name suffix is ue_id+1
}

void remove_ip_if(nr_sdap_entity_t *entity)
{
  DevAssert(entity != NULL);
  // Stop the read thread
  entity->stop_thread = true;

  // Close the socket: read() will get EBADF and exit
  close(entity->pdusession_sock);

  int ret = pthread_join(entity->pdusession_thread, NULL);
  AssertFatal(ret == 0, "pthread_join() failed, errno: %d, %s\n", errno, strerror(errno));
  // Bring down the IP interface
  int default_pdu = get_softmodem_params()->default_pdu_session_id;
  char ifname[IFNAMSIZ];
  if (entity->is_gnb) {
    char *ifprefix = get_softmodem_params()->nsa ? "oaitun_gnb" : "oaitun_enb";
    tun_generate_ifname(ifname, ifprefix, entity->ue_id - 1);
  } else {
    tun_generate_ue_ifname(ifname, entity->ue_id, entity->pdusession_id != default_pdu ? entity->pdusession_id : -1);
  }
  tun_destroy(ifname);
}