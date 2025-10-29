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

/*! \file rrc_gNB_UE_context.h
 * \brief rrc procedures for UE context
 * \author Lionel GAUTHIER
 * \date 2015
 * \version 1.0
 * \company Eurecom
 * \email: lionel.gauthier@eurecom.fr
 */

#include "rrc_gNB_UE_context.h"
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include "RRC/NR/nr_rrc_defs.h"
#include "T.h"
#include "assertions.h"
#include "common/platform_constants.h"
#include "common/utils/LOG/log.h"
#include "linear_alloc.h"
#include "openair2/F1AP/f1ap_ids.h"
#include "tree.h"

static void rrc_gNB_ue_context_update_time(rrc_gNB_ue_context_t *ctxt)
{
  ctxt->ue_context.last_seen = time(NULL);
}

//------------------------------------------------------------------------------
int rrc_gNB_compare_ue_rnti_id(rrc_gNB_ue_context_t *c1_pP, rrc_gNB_ue_context_t *c2_pP)
//------------------------------------------------------------------------------
{
  if (c1_pP->ue_context.rrc_ue_id > c2_pP->ue_context.rrc_ue_id) {
    return 1;
  }

  if (c1_pP->ue_context.rrc_ue_id < c2_pP->ue_context.rrc_ue_id) {
    return -1;
  }

  return 0;
}

/* Generate the tree management functions */
RB_GENERATE(rrc_nr_ue_tree_s, rrc_gNB_ue_context_s, entries,
            rrc_gNB_compare_ue_rnti_id);

//------------------------------------------------------------------------------
rrc_gNB_ue_context_t *rrc_gNB_allocate_new_ue_context(gNB_RRC_INST *rrc_instance_pP)
//------------------------------------------------------------------------------
{
  rrc_gNB_ue_context_t *new_p = calloc(1, sizeof(*new_p));

  if (new_p == NULL) {
    LOG_E(NR_RRC, "Cannot allocate new ue context\n");
    return NULL;
  }
  new_p->ue_context.rrc_ue_id = uid_linear_allocator_new(&rrc_instance_pP->uid_allocator) + 1;
  rrc_gNB_ue_context_update_time(new_p);

  for(int i = 0; i < NB_RB_MAX; i++)
    new_p->ue_context.pduSession[i].xid = -1;

  LOG_D(NR_RRC, "Returning new RRC UE context RRC ue id: %d\n", new_p->ue_context.rrc_ue_id);
  return(new_p);
}


//------------------------------------------------------------------------------
rrc_gNB_ue_context_t *rrc_gNB_get_ue_context(gNB_RRC_INST *rrc_instance_pP, ue_id_t ue)
//------------------------------------------------------------------------------
{
  rrc_gNB_ue_context_t temp;
  /* gNB ue rrc id = 24 bits wide */
  temp.ue_context.rrc_ue_id = ue;
  return RB_FIND(rrc_nr_ue_tree_s, &rrc_instance_pP->rrc_ue_head, &temp);
}

rrc_gNB_ue_context_t *rrc_gNB_get_ue_context_by_rnti(gNB_RRC_INST *rrc_instance_pP, sctp_assoc_t assoc_id, rnti_t rntiP)
{
  rrc_gNB_ue_context_t *ue_context_p;
  RB_FOREACH(ue_context_p, rrc_nr_ue_tree_s, &(rrc_instance_pP->rrc_ue_head)) {
    f1_ue_data_t ue_data = cu_get_f1_ue_data(ue_context_p->ue_context.rrc_ue_id);
    if (ue_data.du_assoc_id == assoc_id && ue_context_p->ue_context.rnti == rntiP) {
      rrc_gNB_ue_context_update_time(ue_context_p);
      return ue_context_p;
    }
  }
  LOG_W(NR_RRC, "search by RNTI %04x and assoc_id %d: no UE found\n", rntiP, assoc_id);
  return NULL;
}

rrc_gNB_ue_context_t *rrc_gNB_get_ue_context_by_rnti_any_du(gNB_RRC_INST *rrc_instance_pP, rnti_t rntiP)
{
  rrc_gNB_ue_context_t *ue_context_p;
  RB_FOREACH(ue_context_p, rrc_nr_ue_tree_s, &(rrc_instance_pP->rrc_ue_head))
  {
    if (ue_context_p->ue_context.rnti == rntiP) {
      rrc_gNB_ue_context_update_time(ue_context_p);
      return ue_context_p;
    }
  }
  LOG_W(NR_RRC, "search by rnti not found %04x\n", rntiP);
  return NULL;
}

void rrc_gNB_free_mem_ue_context(rrc_gNB_ue_context_t *const ue_context_pP)
//-----------------------------------------------------------------------------
{
  LOG_T(NR_RRC, " Clearing UE context 0x%p (free internal structs)\n", ue_context_pP);
  free(ue_context_pP);
}

//------------------------------------------------------------------------------
void rrc_gNB_remove_ue_context(gNB_RRC_INST *rrc_instance_pP, rrc_gNB_ue_context_t *ue_context_pP)
//------------------------------------------------------------------------------
{
  if (rrc_instance_pP == NULL) {
    LOG_E(NR_RRC, " Bad RRC instance\n");
    return;
  }

  if (ue_context_pP == NULL) {
    LOG_E(NR_RRC, "Trying to free a NULL UE context\n");
    return;
  }

  LOG_UE_EVENT(&ue_context_pP->ue_context, "Remove UE context\n");
  RB_REMOVE(rrc_nr_ue_tree_s, &rrc_instance_pP->rrc_ue_head, ue_context_pP);
  uid_linear_allocator_free(&rrc_instance_pP->uid_allocator, ue_context_pP->ue_context.rrc_ue_id - 1);
  cu_remove_f1_ue_data(ue_context_pP->ue_context.rrc_ue_id);
  rrc_gNB_free_mem_ue_context(ue_context_pP);
}

//-----------------------------------------------------------------------------
// return the ue context if there is already an UE with ue_identityP, NULL otherwise
rrc_gNB_ue_context_t *rrc_gNB_ue_context_random_exist(gNB_RRC_INST *rrc_instance_pP, const uint64_t ue_identityP)
//-----------------------------------------------------------------------------
{
  rrc_gNB_ue_context_t *ue_context_p = NULL;
  RB_FOREACH(ue_context_p, rrc_nr_ue_tree_s, &rrc_instance_pP->rrc_ue_head) {
    if (ue_context_p->ue_context.random_ue_identity == ue_identityP) {
      rrc_gNB_ue_context_update_time(ue_context_p);
      return ue_context_p;
    }
  }
  return NULL;
}

//-----------------------------------------------------------------------------
// return the ue context if there is already an UE with the same S-TMSI, NULL otherwise
rrc_gNB_ue_context_t *rrc_gNB_ue_context_5g_s_tmsi_exist(gNB_RRC_INST *rrc_instance_pP, const uint64_t s_TMSI)
//-----------------------------------------------------------------------------
{
  rrc_gNB_ue_context_t *ue_context_p = NULL;
  RB_FOREACH(ue_context_p, rrc_nr_ue_tree_s, &rrc_instance_pP->rrc_ue_head)
  {
    LOG_I(NR_RRC, "Checking for UE 5G S-TMSI %ld: RNTI %04x\n", s_TMSI, ue_context_p->ue_context.rnti);
    if (ue_context_p->ue_context.ng_5G_S_TMSI_Part1 == s_TMSI) {
      rrc_gNB_ue_context_update_time(ue_context_p);
      return ue_context_p;
    }
  }
    return NULL;
}

//-----------------------------------------------------------------------------
// return a new ue context structure if ue_identityP, rnti not found in collection
rrc_gNB_ue_context_t *rrc_gNB_create_ue_context(sctp_assoc_t assoc_id,
                                                rnti_t rnti,
                                                gNB_RRC_INST *rrc_instance_pP,
                                                const uint64_t ue_identityP,
                                                uint32_t du_ue_id)
//-----------------------------------------------------------------------------
{
  rrc_gNB_ue_context_t *ue_context_p = rrc_gNB_allocate_new_ue_context(rrc_instance_pP);
  if (ue_context_p == NULL)
    return NULL;

  gNB_RRC_UE_t *ue = &ue_context_p->ue_context;
  ue->rnti = rnti;
  ue->random_ue_identity = ue_identityP;
  // signal "no AMF UE NGAP ID" because valid range is 0..2^40-1
  ue->amf_ue_ngap_id = INT64_MAX;
  f1_ue_data_t ue_data = {.secondary_ue = du_ue_id, .du_assoc_id = assoc_id};
  AssertFatal(!cu_exists_f1_ue_data(ue->rrc_ue_id),
              "UE F1 Context for ID %d already exists, logic bug\n",
              ue->rrc_ue_id);
  bool success = cu_add_f1_ue_data(ue->rrc_ue_id, &ue_data);
  DevAssert(success);
  ue->max_delays_pdu_session = 20; /* see rrc_gNB_process_NGAP_PDUSESSION_SETUP_REQ() */
  ue->ongoing_pdusession_setup_request = false;

  RB_INSERT(rrc_nr_ue_tree_s, &rrc_instance_pP->rrc_ue_head, ue_context_p);
  LOG_UE_EVENT(ue,
               "Create UE context: CU UE ID %u DU UE ID %u (rnti: %04x, random ue id %lx)\n",
               ue->rrc_ue_id,
               du_ue_id,
               ue->rnti,
               ue->random_ue_identity);
  return ue_context_p;
}
// ==========================
// 🎯 IP-RNTI 映射管理
// ==========================

#define MAX_IP_MAPPINGS 256

typedef struct {
    uint16_t rnti;
    char ue_ip[16];  // 存储如 "10.0.1.2"
    uint8_t pdu_session_id;
    bool valid;
} ip_rnti_mapping_t;

static ip_rnti_mapping_t ip_mapping_table[MAX_IP_MAPPINGS];
static pthread_mutex_t mapping_mutex = PTHREAD_MUTEX_INITIALIZER;

void store_ue_ip_rnti_mapping(uint16_t rnti, const char *ue_ip, uint8_t pdu_session_id)
{
    pthread_mutex_lock(&mapping_mutex);
    
    int free_slot = -1;
    for (int i = 0; i < MAX_IP_MAPPINGS; i++) {
        if (ip_mapping_table[i].valid) {
            // 更新相同RNTI和Session的条目
            if (ip_mapping_table[i].rnti == rnti && 
                ip_mapping_table[i].pdu_session_id == pdu_session_id) {
                strncpy(ip_mapping_table[i].ue_ip, ue_ip, sizeof(ip_mapping_table[i].ue_ip) - 1);
                ip_mapping_table[i].ue_ip[sizeof(ip_mapping_table[i].ue_ip) - 1] = '\0';
                
                LOG_I(NR_RRC, "Updated IP mapping: RNTI=0x%04x -> IP=%s (Session=%d)", 
                      rnti, ue_ip, pdu_session_id);
                pthread_mutex_unlock(&mapping_mutex);
                return;
            }
        } else if (free_slot == -1) {
            free_slot = i;
        }
    }
    
    if (free_slot != -1) {
        ip_mapping_table[free_slot].rnti = rnti;
        strncpy(ip_mapping_table[free_slot].ue_ip, ue_ip, sizeof(ip_mapping_table[free_slot].ue_ip) - 1);
        ip_mapping_table[free_slot].ue_ip[sizeof(ip_mapping_table[free_slot].ue_ip) - 1] = '\0';
        ip_mapping_table[free_slot].pdu_session_id = pdu_session_id;
        ip_mapping_table[free_slot].valid = true;
        
        LOG_I(NR_RRC, "Stored new IP mapping: RNTI=0x%04x -> IP=%s (Session=%d)", 
              rnti, ue_ip, pdu_session_id);
    } else {
        LOG_E(NR_RRC, "IP mapping table full! Cannot store RNTI=0x%04x -> IP=%s", rnti, ue_ip);
    }
    
    pthread_mutex_unlock(&mapping_mutex);
}

const char *find_ue_ip_by_rnti(uint16_t rnti, uint8_t pdu_session_id)
{
    pthread_mutex_lock(&mapping_mutex);
    
    for (int i = 0; i < MAX_IP_MAPPINGS; i++) {
        if (ip_mapping_table[i].valid && 
            ip_mapping_table[i].rnti == rnti && 
            ip_mapping_table[i].pdu_session_id == pdu_session_id) {
            pthread_mutex_unlock(&mapping_mutex);
            return ip_mapping_table[i].ue_ip;
        }
    }
    
    pthread_mutex_unlock(&mapping_mutex);
    return NULL;
}

uint16_t find_rnti_by_ue_ip(const char *ue_ip, uint8_t pdu_session_id)
{
    pthread_mutex_lock(&mapping_mutex);
    
    for (int i = 0; i < MAX_IP_MAPPINGS; i++) {
        if (ip_mapping_table[i].valid && 
            ip_mapping_table[i].pdu_session_id == pdu_session_id &&
            strcmp(ip_mapping_table[i].ue_ip, ue_ip) == 0) {
            pthread_mutex_unlock(&mapping_mutex);
            return ip_mapping_table[i].rnti;
        }
    }
    
    pthread_mutex_unlock(&mapping_mutex);
    return 0;
}

void remove_ue_ip_mapping(uint16_t rnti, uint8_t pdu_session_id)
{
    pthread_mutex_lock(&mapping_mutex);
    
    for (int i = 0; i < MAX_IP_MAPPINGS; i++) {
        if (ip_mapping_table[i].valid && 
            ip_mapping_table[i].rnti == rnti && 
            ip_mapping_table[i].pdu_session_id == pdu_session_id) {
            ip_mapping_table[i].valid = false;
            LOG_I(NR_RRC, "Removed IP mapping: RNTI=0x%04x (Session=%d)", rnti, pdu_session_id);
            break;
        }
    }
    
    pthread_mutex_unlock(&mapping_mutex);
}

void dump_ip_mapping_table(void)
{
    pthread_mutex_lock(&mapping_mutex);
    
    LOG_I(NR_RRC, "=== IP-RNTI Mapping Table ===");
    int count = 0;
    for (int i = 0; i < MAX_IP_MAPPINGS; i++) {
        if (ip_mapping_table[i].valid) {
            LOG_I(NR_RRC, "[%d] RNTI=0x%04x -> IP=%s (Session=%d)", 
                  i, ip_mapping_table[i].rnti, ip_mapping_table[i].ue_ip, 
                  ip_mapping_table[i].pdu_session_id);
            count++;
        }
    }
    LOG_I(NR_RRC, "Total mappings: %d", count);
    
    pthread_mutex_unlock(&mapping_mutex);
}

void init_ip_mapping_table(void)
{
    pthread_mutex_lock(&mapping_mutex);
    memset(ip_mapping_table, 0, sizeof(ip_mapping_table));
    pthread_mutex_unlock(&mapping_mutex);
    LOG_I(NR_RRC, "IP mapping table initialized");
}