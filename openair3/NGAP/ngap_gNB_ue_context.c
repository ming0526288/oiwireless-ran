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

/*! \file ngap_gNB_ue_context.c
 * \brief ngap UE context management within gNB
 * \author Yoshio INOUE, Masayuki HARADA
 * \date 2020
 * \version 0.1
 * \email: yoshio.inoue@fujitsu.com,masayuki.harada@fujitsu.com (yoshio.inoue%40fujitsu.com%2cmasayuki.harada%40fujitsu.com)
 */ 

#include "ngap_gNB_ue_context.h"
#include <stdint.h>
#include <stdio.h>
#include "T.h"
#include "common/utils/T/T.h"
#include "ngap_common.h"
#include "tree.h"
#include "ngap_gNB_defs.h"
#include "assertions.h"
#include "common/utils/LOG/log.h"

ue_ip_map_t ue_ip_map_table[MAX_UE_NUM] = {0};

void store_ue_ip_rnti_mapping(uint16_t rnti, const char *ue_ip) {
    for (int i = 0; i < MAX_UE_NUM; i++) {
        if (ue_ip_map_table[i].rnti == 0 || ue_ip_map_table[i].rnti == rnti) {
            ue_ip_map_table[i].rnti = rnti;
            strncpy(ue_ip_map_table[i].ue_ip, ue_ip, sizeof(ue_ip_map_table[i].ue_ip));
            return;
        }
    }
    LOG_W(NGAP, "UE IP mapping table full, cannot store mapping for RNTI 0x%04x", rnti);
}

const char *find_ue_ip_by_rnti(uint16_t rnti) {
    for (int i = 0; i < MAX_UE_NUM; i++) {
        if (ue_ip_map_table[i].rnti == rnti)
            return ue_ip_map_table[i].ue_ip;
    }
    return NULL;
}

void dump_ue_ip_map_table(void) {
    LOG_I(NGAP, "========== UE IP ? RNTI Mapping ==========");
    for (int i = 0; i < MAX_UE_NUM; i++) {
        if (ue_ip_map_table[i].rnti != 0)
            LOG_I(NGAP, "RNTI: 0x%04x <--> IP: %s", ue_ip_map_table[i].rnti, ue_ip_map_table[i].ue_ip);
    }
}
/* Tree of UE ordered by gNB_ue_ngap_id's
 * NO INSTANCE, the 32 bits id is large enough to handle all UEs, regardless the cell, gNB, ...
 */
static RB_HEAD(ngap_ue_map, ngap_gNB_ue_context_s) ngap_ue_head = RB_INITIALIZER(&ngap_ue_head);

/* Generate the tree management functions prototypes */
RB_PROTOTYPE(ngap_ue_map, ngap_gNB_ue_context_s, entries, ngap_gNB_compare_gNB_ue_ngap_id);

static int ngap_gNB_compare_gNB_ue_ngap_id(struct ngap_gNB_ue_context_s *p1, struct ngap_gNB_ue_context_s *p2)
{
  if (p1->gNB_ue_ngap_id > p2->gNB_ue_ngap_id) {
    return 1;
  }

  if (p1->gNB_ue_ngap_id < p2->gNB_ue_ngap_id) {
    return -1;
  }

  return 0;
}

/* Generate the tree management functions */
RB_GENERATE(ngap_ue_map, ngap_gNB_ue_context_s, entries,
            ngap_gNB_compare_gNB_ue_ngap_id);

void ngap_store_ue_context(const ngap_gNB_ue_context_t *ue)
{
  LOG_I(NGAP, "Create UE context (ID %d) for AMF '%s' (assoc_id %d)\n", ue->gNB_ue_ngap_id, ue->amf_ref->amf_name, ue->amf_ref->assoc_id);
  ngap_gNB_ue_context_t *ue_desc_p = calloc_or_fail(1, sizeof(*ue_desc_p));
  *ue_desc_p = *ue;
  if (RB_INSERT(ngap_ue_map, &ngap_ue_head, ue_desc_p))
    LOG_E(NGAP, "Bug in UE uniq number allocation %u, we try to add a existing UE\n", ue_desc_p->gNB_ue_ngap_id);
  return;
}

struct ngap_gNB_ue_context_s *ngap_get_ue_context(uint32_t gNB_ue_ngap_id)
{
  ngap_gNB_ue_context_t temp = {.gNB_ue_ngap_id = gNB_ue_ngap_id};
  return RB_FIND(ngap_ue_map, &ngap_ue_head, &temp);
}

struct ngap_gNB_ue_context_s *ngap_detach_ue_context(uint32_t gNB_ue_ngap_id)
{
  struct ngap_gNB_ue_context_s *tmp = ngap_get_ue_context(gNB_ue_ngap_id);
  if (tmp == NULL) {
    NGAP_ERROR("Trying to free a NULL UE context, %u\n", gNB_ue_ngap_id);
    return NULL;
  }
  RB_REMOVE(ngap_ue_map, &ngap_ue_head, tmp);
  return tmp;
}
