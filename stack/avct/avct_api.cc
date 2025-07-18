/******************************************************************************
 *
 *  Copyright 2003-2016 Broadcom Corporation
 *
 *  Licensed under the Apache License, Version 2.0 (the "License");
 *  you may not use this file except in compliance with the License.
 *  You may obtain a copy of the License at:
 *
 *  http://www.apache.org/licenses/LICENSE-2.0
 *
 *  Unless required by applicable law or agreed to in writing, software
 *  distributed under the License is distributed on an "AS IS" BASIS,
 *  WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 *  See the License for the specific language governing permissions and
 *  limitations under the License.
 *
 ******************************************************************************/

/******************************************************************************
 *
 *  This module contains API of the audio/video control transport protocol.
 *
 ******************************************************************************/

#include "avct_api.h"
#include <string.h>
#include "avct_int.h"
#include "bt_common.h"
#include "bt_target.h"
#include "bt_types.h"
#include "bt_utils.h"
#include "bta_api.h"
#include "btm_api.h"
#include "l2c_api.h"
#include "l2cdefs.h"
#include "osi/include/osi.h"

/* Control block for AVCT */
tAVCT_CB avct_cb;

/*******************************************************************************
 *
 * Function         AVCT_Register
 *
 * Description      This is the system level registration function for the
 *                  AVCTP protocol.  This function initializes AVCTP and
 *                  prepares the protocol stack for its use.  This function
 *                  must be called once by the system or platform using AVCTP
 *                  before the other functions of the API an be used.
 *
 *
 * Returns          void
 *
 ******************************************************************************/
void AVCT_Register(uint16_t mtu, UNUSED_ATTR uint16_t mtu_br,
                   uint8_t sec_mask) {
  AVCT_TRACE_API("AVCT_Register");

  /* register PSM with L2CAP */
  L2CA_Register(AVCT_PSM, (tL2CAP_APPL_INFO*)&avct_l2c_appl,
                true /* enable_snoop */);

  /* set security level */
  BTM_SetSecurityLevel(true, "", BTM_SEC_SERVICE_AVCTP, sec_mask, AVCT_PSM, 0,
                       0);
  BTM_SetSecurityLevel(false, "", BTM_SEC_SERVICE_AVCTP, sec_mask, AVCT_PSM, 0,
                       0);

  /* initialize AVCTP data structures */
  memset(&avct_cb, 0, sizeof(tAVCT_CB));

  uint8_t sec = (uint8_t)(sec_mask | BTA_SEC_ENCRYPT);

  /* Include the browsing channel which uses eFCR */
  L2CA_Register(AVCT_BR_PSM, (tL2CAP_APPL_INFO*)&avct_l2c_br_appl,
                true /*enable_snoop*/);

  /* AVCTP browsing channel uses the same security service as AVCTP control
   * channel */
  BTM_SetSecurityLevel(true, "", BTM_SEC_SERVICE_AVCTP, sec, AVCT_BR_PSM,
                       0, 0);
  BTM_SetSecurityLevel(false, "", BTM_SEC_SERVICE_AVCTP, sec, AVCT_BR_PSM,
                       0, 0);

  if (mtu_br < AVCT_MIN_BROWSE_MTU) mtu_br = AVCT_MIN_BROWSE_MTU;
  avct_cb.mtu_br = mtu_br;

#if defined(AVCT_INITIAL_TRACE_LEVEL)
  avct_cb.trace_level = AVCT_INITIAL_TRACE_LEVEL;
#else
  avct_cb.trace_level = BT_TRACE_LEVEL_NONE;
#endif

  if (mtu < AVCT_MIN_CONTROL_MTU) mtu = AVCT_MIN_CONTROL_MTU;
  /* store mtu */
  avct_cb.mtu = mtu;
}

/*******************************************************************************
 *
 * Function         AVCT_Deregister
 *
 * Description      This function is called to deregister use AVCTP protocol.
 *                  It is called when AVCTP is no longer being used by any
 *                  application in the system.  Before this function can be
 *                  called, all connections must be removed with
 *                  AVCT_RemoveConn().
 *
 *
 * Returns          void
 *
 ******************************************************************************/
void AVCT_Deregister(void) {
  AVCT_TRACE_API("AVCT_Deregister");

  /* deregister PSM with L2CAP */
  L2CA_Deregister(AVCT_PSM);
}

/*******************************************************************************
 *
 * Function         AVCT_CreateConn
 *
 * Description      Create an AVCTP connection.  There are two types of
 *                  connections, initiator and acceptor, as determined by
 *                  the p_cc->role parameter.  When this function is called to
 *                  create an initiator connection, an AVCTP connection to
 *                  the peer device is initiated if one does not already exist.
 *                  If an acceptor connection is created, the connection waits
 *                  passively for an incoming AVCTP connection from a peer
 *                  device.
 *
 *
 * Returns          AVCT_SUCCESS if successful, otherwise error.
 *
 ******************************************************************************/
uint16_t AVCT_CreateConn(uint8_t* p_handle, tAVCT_CC* p_cc,
                         const RawAddress& peer_addr) {
  uint16_t result = AVCT_SUCCESS;
  tAVCT_CCB* p_ccb;
  tAVCT_LCB* p_lcb;

  AVCT_TRACE_API("AVCT_CreateConn: %d, control:%d", p_cc->role, p_cc->control);

  /* Allocate ccb; if no ccbs, return failure */
  p_ccb = avct_ccb_alloc(p_cc);
  if (p_ccb == NULL) {
    result = AVCT_NO_RESOURCES;
  } else {
    /* get handle */
    *p_handle = avct_ccb_to_idx(p_ccb);

    /* if initiator connection */
    if (p_cc->role == AVCT_INT) {
      /* find link; if none allocate a new one */
      p_lcb = avct_lcb_by_bd(peer_addr);
      if (p_lcb == NULL) {
        p_lcb = avct_lcb_alloc(peer_addr);
        if (p_lcb == NULL) {
          /* no link resources; free ccb as well */
          avct_ccb_dealloc(p_ccb, AVCT_NO_EVT, 0, NULL);
          result = AVCT_NO_RESOURCES;
        }
      }
      /* check if PID already in use */
      else if (avct_lcb_has_pid(p_lcb, p_cc->pid)) {
        avct_ccb_dealloc(p_ccb, AVCT_NO_EVT, 0, NULL);
        result = AVCT_PID_IN_USE;
      }

      if (result == AVCT_SUCCESS) {
        /* bind lcb to ccb */
        p_ccb->p_lcb = p_lcb;
        AVCT_TRACE_DEBUG("ch_state: %d", p_lcb->ch_state);
        tAVCT_LCB_EVT avct_lcb_evt;
        avct_lcb_evt.p_ccb = p_ccb;
        avct_lcb_event(p_lcb, AVCT_LCB_UL_BIND_EVT, &avct_lcb_evt);
      }
    }
  }
  return result;
}

/*******************************************************************************
 *
 * Function         AVCT_RemoveConn
 *
 * Description      Remove an AVCTP connection.  This function is called when
 *                  the application is no longer using a connection.  If this
 *                  is the last connection to a peer the L2CAP channel for AVCTP
 *                  will be closed.
 *
 *
 * Returns          AVCT_SUCCESS if successful, otherwise error.
 *
 ******************************************************************************/
uint16_t AVCT_RemoveConn(uint8_t handle) {
  uint16_t result = AVCT_SUCCESS;
  tAVCT_CCB* p_ccb;

  AVCT_TRACE_API("AVCT_RemoveConn");

  /* map handle to ccb */
  p_ccb = avct_ccb_by_idx(handle);
  if (p_ccb == NULL) {
    result = AVCT_BAD_HANDLE;
  }
  /* if connection not bound to lcb, dealloc */
  else if (p_ccb->p_lcb == NULL) {
    avct_ccb_dealloc(p_ccb, AVCT_NO_EVT, 0, NULL);
  }
  /* send unbind event to lcb */
  else {
    tAVCT_LCB_EVT avct_lcb_evt;
    avct_lcb_evt.p_ccb = p_ccb;
    avct_lcb_event(p_ccb->p_lcb, AVCT_LCB_UL_UNBIND_EVT, &avct_lcb_evt);
  }
  return result;
}

/*******************************************************************************
 *
 * Function         AVCT_CreateBrowse
 *
 * Description      Create an AVCTP Browse channel.  There are two types of
 *                  connections, initiator and acceptor, as determined by
 *                  the role parameter.  When this function is called to
 *                  create an initiator connection, the Browse channel to
 *                  the peer device is initiated if one does not already exist.
 *                  If an acceptor connection is created, the connection waits
 *                  passively for an incoming AVCTP connection from a peer
 *                  device.
 *
 *
 * Returns          AVCT_SUCCESS if successful, otherwise error.
 *
 ******************************************************************************/
uint16_t AVCT_CreateBrowse(uint8_t handle, uint8_t role) {
  uint16_t result = AVCT_SUCCESS;
  tAVCT_CCB* p_ccb;
  tAVCT_BCB* p_bcb;
  int index;

  AVCT_TRACE_API("AVCT_CreateBrowse: %d", role);

  /* map handle to ccb */
  p_ccb = avct_ccb_by_idx(handle);
  if (p_ccb == NULL) {
    return AVCT_BAD_HANDLE;
  } else {
    /* mark this CCB as supporting browsing channel */
    if ((p_ccb->allocated & AVCT_ALOC_BCB) == 0) {
      p_ccb->allocated |= AVCT_ALOC_BCB;
    }
  }

  /* if initiator connection */
  if (role == AVCT_INT) {
    /* the link control block must exist before this function is called as INT.
     */
    if ((p_ccb->p_lcb == NULL) || (p_ccb->p_lcb->allocated == 0)) {
      result = AVCT_NOT_OPEN;
    } else {
      /* find link; if none allocate a new one */
      index = p_ccb->p_lcb->allocated;
      if (index > AVCT_NUM_LINKS) {
        result = AVCT_BAD_HANDLE;
      } else {
        p_bcb = &avct_cb.bcb[index - 1];
        p_bcb->allocated = index;
      }
    }

    if (result == AVCT_SUCCESS) {
      /* bind bcb to ccb */
      p_ccb->p_bcb = p_bcb;
      p_bcb->peer_addr = p_ccb->p_lcb->peer_addr;
      AVCT_TRACE_DEBUG("ch_state: %d", p_bcb->ch_state);
      tAVCT_LCB_EVT avct_lcb_evt;
      avct_lcb_evt.p_ccb = p_ccb;
      avct_bcb_event(p_bcb, AVCT_LCB_UL_BIND_EVT, &avct_lcb_evt);
    }
  }

  return result;
}

/*******************************************************************************
 *
 * Function         AVCT_RemoveBrowse
 *
 * Description      Remove an AVCTP Browse channel.  This function is called
 *                  when the application is no longer using a connection.  If
 *                  this is the last connection to a peer the L2CAP channel for
 *                  AVCTP will be closed.
 *
 *
 * Returns          AVCT_SUCCESS if successful, otherwise error.
 *
 ******************************************************************************/
uint16_t AVCT_RemoveBrowse(uint8_t handle) {
  uint16_t result = AVCT_SUCCESS;
  tAVCT_CCB* p_ccb;

  AVCT_TRACE_API("AVCT_RemoveBrowse");

  /* map handle to ccb */
  p_ccb = avct_ccb_by_idx(handle);
  if (p_ccb == NULL) {
    result = AVCT_BAD_HANDLE;
  } else if (p_ccb->p_bcb != NULL)
  /* send unbind event to bcb */
  {
    tAVCT_LCB_EVT avct_lcb_evt;
    avct_lcb_evt.p_ccb = p_ccb;
    avct_bcb_event(p_ccb->p_bcb, AVCT_LCB_UL_UNBIND_EVT, &avct_lcb_evt);
  }

  return result;
}

/*******************************************************************************
 *
 * Function         AVCT_GetBrowseMtu
 *
 * Description      Get the peer_mtu for the AVCTP Browse channel of the given
 *                  connection.
 *
 * Returns          the peer browsing channel MTU.
 *
 ******************************************************************************/
uint16_t AVCT_GetBrowseMtu(uint8_t handle) {
  uint16_t peer_mtu = AVCT_MIN_BROWSE_MTU;

  tAVCT_CCB* p_ccb = avct_ccb_by_idx(handle);

  if (p_ccb != NULL && p_ccb->p_bcb != NULL) {
    peer_mtu = p_ccb->p_bcb->peer_mtu;
  }

  return peer_mtu;
}

/*******************************************************************************
 *
 * Function         AVCT_GetPeerMtu
 *
 * Description      Get the peer_mtu for the AVCTP channel of the given
 *                  connection.
 *
 * Returns          the peer MTU size.
 *
 ******************************************************************************/
uint16_t AVCT_GetPeerMtu(uint8_t handle) {
  uint16_t peer_mtu = L2CAP_DEFAULT_MTU;
  tAVCT_CCB* p_ccb;

  /* map handle to ccb */
  p_ccb = avct_ccb_by_idx(handle);
  if (p_ccb != NULL) {
    if (p_ccb->p_lcb) {
      peer_mtu = p_ccb->p_lcb->peer_mtu;
    }
  }

  return peer_mtu;
}

/*******************************************************************************
 *
 * Function         AVCT_MsgReq
 *
 * Description      Send an AVCTP message to a peer device.  In calling
 *                  AVCT_MsgReq(), the application should keep track of the
 *                  congestion state of AVCTP as communicated with events
 *                  AVCT_CONG_IND_EVT and AVCT_UNCONG_IND_EVT.   If the
 *                  application calls AVCT_MsgReq() when AVCTP is congested
 *                  the message may be discarded.  The application may make its
 *                  first call to AVCT_MsgReq() after it receives an
 *                  AVCT_CONNECT_CFM_EVT or AVCT_CONNECT_IND_EVT on control
 *                  channel or AVCT_BROWSE_CONN_CFM_EVT or
 *                  AVCT_BROWSE_CONN_IND_EVT on browsing channel.
 *
 *                  p_msg->layer_specific must be set to
 *                  AVCT_DATA_CTRL for control channel traffic;
 *                  AVCT_DATA_BROWSE for for browse channel traffic.
 *
 * Returns          AVCT_SUCCESS if successful, otherwise error.
 *
 ******************************************************************************/
uint16_t AVCT_MsgReq(uint8_t handle, uint8_t label, uint8_t cr, BT_HDR* p_msg) {
  uint16_t result = AVCT_SUCCESS;
  tAVCT_CCB* p_ccb;
  tAVCT_UL_MSG ul_msg;

  AVCT_TRACE_API("%s", __func__);

  /* verify p_msg parameter */
  if (p_msg == NULL) {
    return AVCT_NO_RESOURCES;
  }
  AVCT_TRACE_API("%s len: %d layer_specific: %d", __func__, p_msg->len,
                 p_msg->layer_specific);

  /* map handle to ccb */
  p_ccb = avct_ccb_by_idx(handle);
  if (p_ccb == NULL) {
    result = AVCT_BAD_HANDLE;
    osi_free(p_msg);
  }
  /* verify channel is bound to link */
  else if (p_ccb->p_lcb == NULL) {
    result = AVCT_NOT_OPEN;
    osi_free(p_msg);
  }

  if (result == AVCT_SUCCESS) {
    ul_msg.p_buf = p_msg;
    ul_msg.p_ccb = p_ccb;
    ul_msg.label = label;
    ul_msg.cr = cr;

    /* send msg event to bcb */
    if (p_msg->layer_specific == AVCT_DATA_BROWSE) {
      if (p_ccb->p_bcb == NULL && (p_ccb->allocated & AVCT_ALOC_BCB) == 0) {
        /* BCB channel is not open and not allocated */
        result = AVCT_BAD_HANDLE;
        osi_free(p_msg);
      } else {
        p_ccb->p_bcb = avct_bcb_by_lcb(p_ccb->p_lcb);
        tAVCT_LCB_EVT avct_lcb_evt;
        avct_lcb_evt.ul_msg = ul_msg;
        avct_bcb_event(p_ccb->p_bcb, AVCT_LCB_UL_MSG_EVT, &avct_lcb_evt);
      }
    }
    /* send msg event to lcb */
    else {
      tAVCT_LCB_EVT avct_lcb_evt;
      avct_lcb_evt.ul_msg = ul_msg;
      avct_lcb_event(p_ccb->p_lcb, AVCT_LCB_UL_MSG_EVT, &avct_lcb_evt);
    }
  }
  return result;
}
