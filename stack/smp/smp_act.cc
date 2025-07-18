/******************************************************************************
 *
 *  Copyright 2003-2012 Broadcom Corporation
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

#include <cutils/log.h>
#include <log/log.h>
#include <string.h>
#include "btif_api.h"
#include "btif_common.h"
#include "btif_storage.h"
#include "device/include/interop.h"
#include "internal_include/bt_target.h"
#include "stack/btm/btm_int.h"
#include "stack/include/l2c_api.h"
#include "stack/smp/p_256_ecc_pp.h"
#include "stack/smp/smp_int.h"
#include "utils/include/bt_utils.h"

#define SMP_KEY_DIST_TYPE_MAX 4

const tSMP_ACT smp_distribute_act[] = {
    smp_generate_ltk,       /* SMP_SEC_KEY_TYPE_ENC - '1' bit index */
    smp_send_id_info,       /* SMP_SEC_KEY_TYPE_ID - '1' bit index */
    smp_generate_csrk,      /* SMP_SEC_KEY_TYPE_CSRK - '1' bit index */
    smp_set_derive_link_key /* SMP_SEC_KEY_TYPE_LK - '1' bit index */
};

static bool lmp_version_below(const RawAddress& bda, uint8_t version) {
  tACL_CONN* acl = btm_bda_to_acl(bda, BT_TRANSPORT_LE);
  if (acl == NULL || acl->lmp_version == 0) {
    SMP_TRACE_WARNING("%s cannot retrieve LMP version...", __func__);
    return false;
  }
  SMP_TRACE_WARNING("%s LMP version %d < %d", __func__, acl->lmp_version,
                    version);
  return acl->lmp_version < version;
}

static bool pts_test_send_authentication_complete_failure(tSMP_CB* p_cb) {
  uint8_t reason = p_cb->cert_failure;
  if (reason == SMP_PAIR_AUTH_FAIL || reason == SMP_PAIR_FAIL_UNKNOWN ||
      reason == SMP_PAIR_NOT_SUPPORT || reason == SMP_PASSKEY_ENTRY_FAIL ||
      reason == SMP_REPEATED_ATTEMPTS) {
    tSMP_INT_DATA smp_int_data;
    smp_int_data.status = reason;
    smp_sm_event(p_cb, SMP_AUTH_CMPL_EVT, &smp_int_data);
    return true;
  }
  return false;
}

/*******************************************************************************
 * Function         smp_update_key_mask
 * Description      This function updates the key mask for sending or receiving.
 ******************************************************************************/
static void smp_update_key_mask(tSMP_CB* p_cb, uint8_t key_type, bool recv) {
  SMP_TRACE_DEBUG(
      "%s before update role=%d recv=%d local_i_key = %02x, local_r_key = %02x",
      __func__, p_cb->role, recv, p_cb->local_i_key, p_cb->local_r_key);

  if (((p_cb->le_secure_connections_mode_is_used) || (p_cb->smp_over_br)) &&
      ((key_type == SMP_SEC_KEY_TYPE_ENC) ||
       (key_type == SMP_SEC_KEY_TYPE_LK))) {
    /* in LE SC mode LTK, CSRK and BR/EDR LK are derived locally instead of
    ** being exchanged with the peer */
    p_cb->local_i_key &= ~key_type;
    p_cb->local_r_key &= ~key_type;
  } else if (p_cb->role == HCI_ROLE_SLAVE) {
    if (recv)
      p_cb->local_i_key &= ~key_type;
    else
      p_cb->local_r_key &= ~key_type;
  } else {
    if (recv)
      p_cb->local_r_key &= ~key_type;
    else
      p_cb->local_i_key &= ~key_type;
  }

  SMP_TRACE_DEBUG("updated local_i_key = %02x, local_r_key = %02x",
                  p_cb->local_i_key, p_cb->local_r_key);
}

/*******************************************************************************
 * Function     smp_send_app_cback
 * Description  notifies application about the events the application is
 *              interested in
 ******************************************************************************/
void smp_send_app_cback(tSMP_CB* p_cb, tSMP_INT_DATA* p_data) {
  tSMP_EVT_DATA cb_data;
  tSMP_STATUS callback_rc;
  SMP_TRACE_DEBUG("%s p_cb->cb_evt=%d", __func__, p_cb->cb_evt);
  if (p_cb->p_callback && p_cb->cb_evt != 0) {
    switch (p_cb->cb_evt) {
      case SMP_IO_CAP_REQ_EVT:
        cb_data.io_req.auth_req = p_cb->peer_auth_req;
        cb_data.io_req.oob_data = SMP_OOB_NONE;
        cb_data.io_req.io_cap = btif_storage_get_local_io_caps_ble();
        cb_data.io_req.max_key_size = SMP_MAX_ENC_KEY_SIZE;
        cb_data.io_req.init_keys = p_cb->local_i_key;
        cb_data.io_req.resp_keys = p_cb->local_r_key;
        SMP_TRACE_WARNING("io_cap = %d", cb_data.io_req.io_cap);
        break;

      case SMP_NC_REQ_EVT:
        cb_data.passkey = p_data->passkey;
        break;
      case SMP_SC_OOB_REQ_EVT:
        cb_data.req_oob_type = p_data->req_oob_type;
        break;
      case SMP_SC_LOC_OOB_DATA_UP_EVT:
        cb_data.loc_oob_data = p_cb->sc_oob_data.loc_oob_data;
        break;

      case SMP_BR_KEYS_REQ_EVT:
        cb_data.io_req.auth_req = 0;
        cb_data.io_req.oob_data = SMP_OOB_NONE;
        cb_data.io_req.io_cap = 0;
        cb_data.io_req.max_key_size = SMP_MAX_ENC_KEY_SIZE;
        cb_data.io_req.init_keys = SMP_BR_SEC_DEFAULT_KEY;
        cb_data.io_req.resp_keys = SMP_BR_SEC_DEFAULT_KEY;
        break;

      default:
        break;
    }

    callback_rc =
        (*p_cb->p_callback)(p_cb->cb_evt, p_cb->pairing_bda, &cb_data);

    SMP_TRACE_DEBUG("%s: callback_rc=%d  p_cb->cb_evt=%d", __func__,
                    callback_rc, p_cb->cb_evt);

    if (callback_rc == SMP_SUCCESS) {
      switch (p_cb->cb_evt) {
        case SMP_IO_CAP_REQ_EVT:
          p_cb->loc_auth_req = cb_data.io_req.auth_req;
          p_cb->local_io_capability = cb_data.io_req.io_cap;
          p_cb->loc_oob_flag = cb_data.io_req.oob_data;
          p_cb->loc_enc_size = cb_data.io_req.max_key_size;
          p_cb->local_i_key = cb_data.io_req.init_keys;
          p_cb->local_r_key = cb_data.io_req.resp_keys;

          if (!(p_cb->loc_auth_req & SMP_AUTH_BOND)) {
            SMP_TRACE_WARNING("Non bonding: No keys will be exchanged");
            p_cb->local_i_key = 0;
            p_cb->local_r_key = 0;
          }

          SMP_TRACE_WARNING(
              "rcvd auth_req: 0x%02x, io_cap: %d "
              "loc_oob_flag: %d loc_enc_size: %d, "
              "local_i_key: 0x%02x, local_r_key: 0x%02x",
              p_cb->loc_auth_req, p_cb->local_io_capability, p_cb->loc_oob_flag,
              p_cb->loc_enc_size, p_cb->local_i_key, p_cb->local_r_key);

          p_cb->secure_connections_only_mode_required =
              (btm_cb.security_mode == BTM_SEC_MODE_SC) ? true : false;
          /* just for PTS, force SC bit */
          if (p_cb->secure_connections_only_mode_required) {
            p_cb->loc_auth_req |= SMP_SC_SUPPORT_BIT;
          }

          if (!p_cb->secure_connections_only_mode_required &&
              (!(p_cb->loc_auth_req & SMP_SC_SUPPORT_BIT) ||
               lmp_version_below(p_cb->pairing_bda, HCI_PROTO_VERSION_4_2) ||
               interop_match_addr(INTEROP_DISABLE_LE_SECURE_CONNECTIONS,
                                  (const RawAddress*)&p_cb->pairing_bda))) {
            p_cb->loc_auth_req &= ~SMP_SC_SUPPORT_BIT;
            p_cb->loc_auth_req &= ~SMP_KP_SUPPORT_BIT;
            p_cb->local_i_key &= ~SMP_SEC_KEY_TYPE_LK;
            p_cb->local_r_key &= ~SMP_SEC_KEY_TYPE_LK;
          }

          if (lmp_version_below(p_cb->pairing_bda, HCI_PROTO_VERSION_5_0)) {
            p_cb->loc_auth_req &= ~SMP_H7_SUPPORT_BIT;
          }

          SMP_TRACE_WARNING(
              "set auth_req: 0x%02x, local_i_key: 0x%02x, local_r_key: 0x%02x",
              p_cb->loc_auth_req, p_cb->local_i_key, p_cb->local_r_key);

          smp_sm_event(p_cb, SMP_IO_RSP_EVT, NULL);
          break;

        case SMP_BR_KEYS_REQ_EVT:
          p_cb->loc_enc_size = cb_data.io_req.max_key_size;
          p_cb->local_i_key = cb_data.io_req.init_keys;
          p_cb->local_r_key = cb_data.io_req.resp_keys;
          p_cb->loc_auth_req |= SMP_H7_SUPPORT_BIT;

          p_cb->local_i_key &= ~SMP_SEC_KEY_TYPE_LK;
          p_cb->local_r_key &= ~SMP_SEC_KEY_TYPE_LK;

          SMP_TRACE_WARNING(
              "for SMP over BR max_key_size: 0x%02x, local_i_key: 0x%02x, "
              "local_r_key: 0x%02x, p_cb->loc_auth_req: 0x%02x",
              p_cb->loc_enc_size, p_cb->local_i_key, p_cb->local_r_key,
              p_cb->loc_auth_req);

          smp_br_state_machine_event(p_cb, SMP_BR_KEYS_RSP_EVT, NULL);
          break;
      }
    }
  }

  if (!p_cb->cb_evt && p_cb->discard_sec_req) {
    p_cb->discard_sec_req = false;
    smp_sm_event(p_cb, SMP_DISCARD_SEC_REQ_EVT, NULL);
  }

  SMP_TRACE_DEBUG("%s: return", __func__);
}

/*******************************************************************************
 * Function     smp_send_pair_fail
 * Description  pairing failure to peer device if needed.
 ******************************************************************************/
void smp_send_pair_fail(tSMP_CB* p_cb, tSMP_INT_DATA* p_data) {
  p_cb->status = p_data->status;
  p_cb->failure = p_data->status;

  SMP_TRACE_DEBUG("%s: status=%d failure=%d ", __func__, p_cb->status,
                  p_cb->failure);

  if (p_cb->status <= SMP_MAX_FAIL_RSN_PER_SPEC &&
      p_cb->status != SMP_SUCCESS) {
    smp_send_cmd(SMP_OPCODE_PAIRING_FAILED, p_cb);
    p_cb->wait_for_authorization_complete = true;
  }
}

/*******************************************************************************
 * Function     smp_send_pair_req
 * Description  actions related to sending pairing request
 ******************************************************************************/
void smp_send_pair_req(tSMP_CB* p_cb, tSMP_INT_DATA* p_data) {
  tBTM_SEC_DEV_REC* p_dev_rec = btm_find_dev(p_cb->pairing_bda);
  SMP_TRACE_DEBUG("%s", __func__);

  /* erase all keys when master sends pairing req*/
  if (p_dev_rec) btm_sec_clear_ble_keys(p_dev_rec);
  /* do not manipulate the key, let app decide,
     leave out to BTM to mandate key distribution for bonding case */
  smp_send_cmd(SMP_OPCODE_PAIRING_REQ, p_cb);
}

/*******************************************************************************
 * Function     smp_send_pair_rsp
 * Description  actions related to sending pairing response
 ******************************************************************************/
void smp_send_pair_rsp(tSMP_CB* p_cb, tSMP_INT_DATA* p_data) {
  SMP_TRACE_DEBUG("%s", __func__);

  p_cb->local_i_key &= p_cb->peer_i_key;
  p_cb->local_r_key &= p_cb->peer_r_key;

  if (smp_send_cmd(SMP_OPCODE_PAIRING_RSP, p_cb)) {
    if (p_cb->selected_association_model == SMP_MODEL_SEC_CONN_OOB)
      smp_use_oob_private_key(p_cb, NULL);
    else
      smp_decide_association_model(p_cb, NULL);
  }
}

/*******************************************************************************
 * Function     smp_send_confirm
 * Description  send confirmation to the peer
 ******************************************************************************/
void smp_send_confirm(tSMP_CB* p_cb, tSMP_INT_DATA* p_data) {
  SMP_TRACE_DEBUG("%s", __func__);
  smp_send_cmd(SMP_OPCODE_CONFIRM, p_cb);
  p_cb->flags |= SMP_PAIR_FLAGS_CMD_CONFIRM_SENT;
}

/*******************************************************************************
 * Function     smp_send_init
 * Description  process pairing initializer to slave device
 ******************************************************************************/
void smp_send_init(tSMP_CB* p_cb, tSMP_INT_DATA* p_data) {
  SMP_TRACE_DEBUG("%s", __func__);
  smp_send_cmd(SMP_OPCODE_INIT, p_cb);
}

/*******************************************************************************
 * Function     smp_send_rand
 * Description  send pairing random to the peer
 ******************************************************************************/
void smp_send_rand(tSMP_CB* p_cb, tSMP_INT_DATA* p_data) {
  SMP_TRACE_DEBUG("%s", __func__);
  smp_send_cmd(SMP_OPCODE_RAND, p_cb);
}

/*******************************************************************************
 * Function     smp_send_pair_public_key
 * Description  send pairing public key command to the peer
 ******************************************************************************/
void smp_send_pair_public_key(tSMP_CB* p_cb, tSMP_INT_DATA* p_data) {
  SMP_TRACE_DEBUG("%s", __func__);
  smp_send_cmd(SMP_OPCODE_PAIR_PUBLIC_KEY, p_cb);
}

/*******************************************************************************
 * Function     SMP_SEND_COMMITMENT
 * Description send commitment command to the peer
 ******************************************************************************/
void smp_send_commitment(tSMP_CB* p_cb, tSMP_INT_DATA* p_data) {
  SMP_TRACE_DEBUG("%s", __func__);
  smp_send_cmd(SMP_OPCODE_PAIR_COMMITM, p_cb);
}

/*******************************************************************************
 * Function     smp_send_dhkey_check
 * Description send DHKey Check command to the peer
 ******************************************************************************/
void smp_send_dhkey_check(tSMP_CB* p_cb, tSMP_INT_DATA* p_data) {
  SMP_TRACE_DEBUG("%s", __func__);
  smp_send_cmd(SMP_OPCODE_PAIR_DHKEY_CHECK, p_cb);
}

/*******************************************************************************
 * Function     smp_send_keypress_notification
 * Description send Keypress Notification command to the peer
 ******************************************************************************/
void smp_send_keypress_notification(tSMP_CB* p_cb, tSMP_INT_DATA* p_data) {
  p_cb->local_keypress_notification = p_data->status;
  smp_send_cmd(SMP_OPCODE_PAIR_KEYPR_NOTIF, p_cb);
}

/*******************************************************************************
 * Function     smp_send_enc_info
 * Description  send encryption information command.
 ******************************************************************************/
void smp_send_enc_info(tSMP_CB* p_cb, tSMP_INT_DATA* p_data) {
  tBTM_LE_KEY_VALUE le_key;

  SMP_TRACE_DEBUG("%s: p_cb->loc_enc_size = %d", __func__, p_cb->loc_enc_size);
  smp_update_key_mask(p_cb, SMP_SEC_KEY_TYPE_ENC, false);

  smp_send_cmd(SMP_OPCODE_ENCRYPT_INFO, p_cb);
  smp_send_cmd(SMP_OPCODE_MASTER_ID, p_cb);

  /* save the DIV and key size information when acting as slave device */
  le_key.lenc_key.ltk = p_cb->ltk;
  le_key.lenc_key.div = p_cb->div;
  le_key.lenc_key.key_size = p_cb->loc_enc_size;
  le_key.lenc_key.sec_level = p_cb->sec_level;

  if ((p_cb->peer_auth_req & SMP_AUTH_BOND) &&
      (p_cb->loc_auth_req & SMP_AUTH_BOND))
    btm_sec_save_le_key(p_cb->pairing_bda, BTM_LE_KEY_LENC, &le_key, true);

  SMP_TRACE_WARNING("%s", __func__);

  smp_key_distribution(p_cb, NULL);
}

/*******************************************************************************
 * Function     smp_send_id_info
 * Description  send ID information command.
 ******************************************************************************/
void smp_send_id_info(tSMP_CB* p_cb, tSMP_INT_DATA* p_data) {
  tBTM_LE_KEY_VALUE le_key;
  SMP_TRACE_DEBUG("%s", __func__);
  smp_update_key_mask(p_cb, SMP_SEC_KEY_TYPE_ID, false);

  smp_send_cmd(SMP_OPCODE_IDENTITY_INFO, p_cb);
  smp_send_cmd(SMP_OPCODE_ID_ADDR, p_cb);

  if ((p_cb->peer_auth_req & SMP_AUTH_BOND) &&
      (p_cb->loc_auth_req & SMP_AUTH_BOND))
    btm_sec_save_le_key(p_cb->pairing_bda, BTM_LE_KEY_LID, &le_key, true);

  SMP_TRACE_WARNING("%s", __func__);
  smp_key_distribution_by_transport(p_cb, NULL);
}

/**  send CSRK command. */
void smp_send_csrk_info(tSMP_CB* p_cb, tSMP_INT_DATA* p_data) {
  tBTM_LE_KEY_VALUE key;
  SMP_TRACE_DEBUG("%s", __func__);
  smp_update_key_mask(p_cb, SMP_SEC_KEY_TYPE_CSRK, false);

  if (smp_send_cmd(SMP_OPCODE_SIGN_INFO, p_cb)) {
    key.lcsrk_key.div = p_cb->div;
    key.lcsrk_key.sec_level = p_cb->sec_level;
    key.lcsrk_key.counter = 0; /* initialize the local counter */
    key.lcsrk_key.csrk = p_cb->csrk;
    btm_sec_save_le_key(p_cb->pairing_bda, BTM_LE_KEY_LCSRK, &key, true);
  }

  smp_key_distribution_by_transport(p_cb, NULL);
}

/*******************************************************************************
 * Function     smp_send_ltk_reply
 * Description  send LTK reply
 ******************************************************************************/
void smp_send_ltk_reply(tSMP_CB* p_cb, tSMP_INT_DATA* p_data) {
  SMP_TRACE_DEBUG("%s", __func__);

  Octet16 stk;
  memcpy(stk.data(), p_data->key.p_data, stk.size());
  /* send stk as LTK response */
  btm_ble_ltk_request_reply(p_cb->pairing_bda, true, stk);
}

/*******************************************************************************
 * Function     smp_proc_sec_req
 * Description  process security request.
 ******************************************************************************/
void smp_proc_sec_req(tSMP_CB* p_cb, tSMP_INT_DATA* p_data) {
  if (smp_command_has_invalid_length(p_cb)) {
    tSMP_INT_DATA smp_int_data;
    smp_int_data.status = SMP_INVALID_PARAMETERS;
    smp_sm_event(p_cb, SMP_AUTH_CMPL_EVT, &smp_int_data);
    return;
  }

  tBTM_LE_AUTH_REQ auth_req = *(tBTM_LE_AUTH_REQ*)p_data->p_data;
  tBTM_BLE_SEC_REQ_ACT sec_req_act;

  SMP_TRACE_DEBUG("%s: auth_req=0x%x", __func__, auth_req);

  p_cb->cb_evt = 0;

  btm_ble_link_sec_check(p_cb->pairing_bda, auth_req, &sec_req_act);

  SMP_TRACE_DEBUG("%s: sec_req_act=0x%x", __func__, sec_req_act);

  switch (sec_req_act) {
    case BTM_BLE_SEC_REQ_ACT_ENCRYPT:
      SMP_TRACE_DEBUG("%s: BTM_BLE_SEC_REQ_ACT_ENCRYPT", __func__);
      smp_sm_event(p_cb, SMP_ENC_REQ_EVT, NULL);
      break;

    case BTM_BLE_SEC_REQ_ACT_PAIR:
      p_cb->secure_connections_only_mode_required =
          (btm_cb.security_mode == BTM_SEC_MODE_SC) ? true : false;

      /* respond to non SC pairing request as failure in SC only mode */
      if (p_cb->secure_connections_only_mode_required &&
          (auth_req & SMP_SC_SUPPORT_BIT) == 0) {
        tSMP_INT_DATA smp_int_data;
        smp_int_data.status = SMP_PAIR_AUTH_FAIL;
        smp_sm_event(p_cb, SMP_AUTH_CMPL_EVT, &smp_int_data);
      } else {
        /* initialize local i/r key to be default keys */
        p_cb->peer_auth_req = auth_req;
        p_cb->local_r_key = p_cb->local_i_key = SMP_SEC_DEFAULT_KEY;
        p_cb->cb_evt = SMP_SEC_REQUEST_EVT;
      }
      break;

    case BTM_BLE_SEC_REQ_ACT_DISCARD:
      p_cb->discard_sec_req = true;
      break;

    default:
      /* do nothing */
      break;
  }
}

/*******************************************************************************
 * Function     smp_proc_sec_grant
 * Description  process security grant.
 ******************************************************************************/
void smp_proc_sec_grant(tSMP_CB* p_cb, tSMP_INT_DATA* p_data) {
  uint8_t res = p_data->status;
  SMP_TRACE_DEBUG("%s", __func__);
  if (res != SMP_SUCCESS) {
    smp_sm_event(p_cb, SMP_AUTH_CMPL_EVT, p_data);
  } else /*otherwise, start pairing */
  {
    /* send IO request callback */
    p_cb->cb_evt = SMP_IO_CAP_REQ_EVT;
  }
}

/*******************************************************************************
 * Function     smp_proc_pair_fail
 * Description  process pairing failure from peer device
 ******************************************************************************/
void smp_proc_pair_fail(tSMP_CB* p_cb, tSMP_INT_DATA* p_data) {
  SMP_TRACE_DEBUG("%s", __func__);

  if (p_cb->rcvd_cmd_len < 2) {
    android_errorWriteLog(0x534e4554, "111214739");
    SMP_TRACE_WARNING("%s: rcvd_cmd_len %d too short: must be at least 2",
                      __func__, p_cb->rcvd_cmd_len);
    p_cb->status = SMP_INVALID_PARAMETERS;
  } else {
    p_cb->status = p_data->status;
  }

  /* Cancel pending auth complete timer if set */
  alarm_cancel(p_cb->delayed_auth_timer_ent);
}

/*******************************************************************************
 * Function     smp_proc_pair_cmd
 * Description  Process the SMP pairing request/response from peer device
 ******************************************************************************/
void smp_proc_pair_cmd(tSMP_CB* p_cb, tSMP_INT_DATA* p_data) {
  uint8_t* p = p_data->p_data;
  tBTM_SEC_DEV_REC* p_dev_rec = btm_find_dev(p_cb->pairing_bda);

  SMP_TRACE_DEBUG("%s: pairing_bda=%s", __func__,
                  p_cb->pairing_bda.ToString().c_str());

  /* erase all keys if it is slave proc pairing req */
  if (p_dev_rec && (p_cb->role == HCI_ROLE_SLAVE))
    btm_sec_clear_ble_keys(p_dev_rec);

  p_cb->flags |= SMP_PAIR_FLAG_ENC_AFTER_PAIR;

  if (smp_command_has_invalid_length(p_cb)) {
    tSMP_INT_DATA smp_int_data;
    smp_int_data.status = SMP_INVALID_PARAMETERS;
    android_errorWriteLog(0x534e4554, "111850706");
    smp_sm_event(p_cb, SMP_AUTH_CMPL_EVT, &smp_int_data);
    return;
  }

  STREAM_TO_UINT8(p_cb->peer_io_caps, p);
  STREAM_TO_UINT8(p_cb->peer_oob_flag, p);
  STREAM_TO_UINT8(p_cb->peer_auth_req, p);
  STREAM_TO_UINT8(p_cb->peer_enc_size, p);
  STREAM_TO_UINT8(p_cb->peer_i_key, p);
  STREAM_TO_UINT8(p_cb->peer_r_key, p);

  if (smp_command_has_invalid_parameters(p_cb)) {
    tSMP_INT_DATA smp_int_data;
    smp_int_data.status = SMP_INVALID_PARAMETERS;
    smp_sm_event(p_cb, SMP_AUTH_CMPL_EVT, &smp_int_data);
    return;
  }

  // PTS Testing failure modes
  if (pts_test_send_authentication_complete_failure(p_cb)) return;

  if (p_cb->role == HCI_ROLE_SLAVE) {
    if (!(p_cb->flags & SMP_PAIR_FLAGS_WE_STARTED_DD)) {
      /* peer (master) started pairing sending Pairing Request */
      p_cb->local_i_key = p_cb->peer_i_key;
      p_cb->local_r_key = p_cb->peer_r_key;

      p_cb->cb_evt = SMP_SEC_REQUEST_EVT;
    } else /* update local i/r key according to pairing request */
    {
      /* pairing started with this side (slave) sending Security Request */
      p_cb->local_i_key &= p_cb->peer_i_key;
      p_cb->local_r_key &= p_cb->peer_r_key;
      p_cb->selected_association_model = smp_select_association_model(p_cb);

      if (p_cb->secure_connections_only_mode_required &&
          (!(p_cb->le_secure_connections_mode_is_used) ||
           (p_cb->selected_association_model ==
            SMP_MODEL_SEC_CONN_JUSTWORKS))) {
        SMP_TRACE_ERROR(
            "%s: pairing failed - slave requires secure connection only mode",
            __func__);
        tSMP_INT_DATA smp_int_data;
        smp_int_data.status = SMP_PAIR_AUTH_FAIL;
        smp_sm_event(p_cb, SMP_AUTH_CMPL_EVT, &smp_int_data);
        return;
      }

      if (p_cb->selected_association_model == SMP_MODEL_SEC_CONN_OOB) {
        if (smp_request_oob_data(p_cb)) return;
      } else {
        smp_send_pair_rsp(p_cb, NULL);
      }
    }
  } else /* Master receives pairing response */
  {
    p_cb->selected_association_model = smp_select_association_model(p_cb);

    if (p_cb->secure_connections_only_mode_required &&
        (!(p_cb->le_secure_connections_mode_is_used) ||
         (p_cb->selected_association_model == SMP_MODEL_SEC_CONN_JUSTWORKS))) {
      SMP_TRACE_ERROR(
          "Master requires secure connection only mode "
          "but it can't be provided -> Master fails pairing");
      tSMP_INT_DATA smp_int_data;
      smp_int_data.status = SMP_PAIR_AUTH_FAIL;
      smp_sm_event(p_cb, SMP_AUTH_CMPL_EVT, &smp_int_data);
      return;
    }

    if (p_cb->selected_association_model == SMP_MODEL_SEC_CONN_OOB) {
      if (smp_request_oob_data(p_cb)) return;
    } else {
      smp_decide_association_model(p_cb, NULL);
    }
  }
}

/** process pairing confirm from peer device */
void smp_proc_confirm(tSMP_CB* p_cb, tSMP_INT_DATA* p_data) {
  SMP_TRACE_DEBUG("%s", __func__);

  if (smp_command_has_invalid_parameters(p_cb)) {
    tSMP_INT_DATA smp_int_data;
    smp_int_data.status = SMP_INVALID_PARAMETERS;
    smp_sm_event(p_cb, SMP_AUTH_CMPL_EVT, &smp_int_data);
    return;
  }

  if (p_data) {
    uint8_t* p = p_data->p_data;
    if (p != NULL) {
      /* save the SConfirm for comparison later */
      STREAM_TO_ARRAY(p_cb->rconfirm.data(), p, OCTET16_LEN);
    }
  }

  p_cb->flags |= SMP_PAIR_FLAGS_CMD_CONFIRM;
}

/** process pairing initializer from peer device */
void smp_proc_init(tSMP_CB* p_cb, tSMP_INT_DATA* p_data) {
  uint8_t* p = p_data->p_data;

  SMP_TRACE_DEBUG("%s", __func__);

  if (smp_command_has_invalid_parameters(p_cb)) {
    tSMP_INT_DATA smp_int_data;
    smp_int_data.status = SMP_INVALID_PARAMETERS;
    smp_sm_event(p_cb, SMP_AUTH_CMPL_EVT, &smp_int_data);
    return;
  }

  if (!((p_cb->loc_auth_req & SMP_SC_SUPPORT_BIT) &&
        (p_cb->peer_auth_req & SMP_SC_SUPPORT_BIT)) &&
      !(p_cb->flags & SMP_PAIR_FLAGS_CMD_CONFIRM_SENT)) {
    // in legacy pairing, the peer should send its rand after
    // we send our confirm
    tSMP_INT_DATA smp_int_data{};
    smp_int_data.status = SMP_INVALID_PARAMETERS;
    smp_sm_event(p_cb, SMP_AUTH_CMPL_EVT, &smp_int_data);
    return;
  }

  /* save the SRand for comparison */
  STREAM_TO_ARRAY(p_cb->rrand.data(), p, OCTET16_LEN);
}

/*******************************************************************************
 * Function     smp_proc_rand
 * Description  process pairing random (nonce) from peer device
 ******************************************************************************/
void smp_proc_rand(tSMP_CB* p_cb, tSMP_INT_DATA* p_data) {
  uint8_t* p = p_data->p_data;

  SMP_TRACE_DEBUG("%s", __func__);

  if (smp_command_has_invalid_parameters(p_cb)) {
    tSMP_INT_DATA smp_int_data;
    smp_int_data.status = SMP_INVALID_PARAMETERS;
    smp_sm_event(p_cb, SMP_AUTH_CMPL_EVT, &smp_int_data);
    return;
  }

  if (!((p_cb->loc_auth_req & SMP_SC_SUPPORT_BIT) &&
        (p_cb->peer_auth_req & SMP_SC_SUPPORT_BIT)) &&
      !(p_cb->flags & SMP_PAIR_FLAGS_CMD_CONFIRM_SENT)) {
    // in legacy pairing, the peer should send its rand after
    // we send our confirm
    tSMP_INT_DATA smp_int_data{};
    smp_int_data.status = SMP_INVALID_PARAMETERS;
    smp_sm_event(p_cb, SMP_AUTH_CMPL_EVT, &smp_int_data);
    return;
  }

  /* save the SRand for comparison */
  STREAM_TO_ARRAY(p_cb->rrand.data(), p, OCTET16_LEN);
}

/*******************************************************************************
 * Function     smp_process_pairing_public_key
 * Description  process pairing public key command from the peer device
 *              - saves the peer public key;
 *              - sets the flag indicating that the peer public key is received;
 *              - calls smp_wait_for_both_public_keys(...).
 *
 ******************************************************************************/
void smp_process_pairing_public_key(tSMP_CB* p_cb, tSMP_INT_DATA* p_data) {
  uint8_t* p = p_data->p_data;

  SMP_TRACE_DEBUG("%s", __func__);

  if (smp_command_has_invalid_parameters(p_cb)) {
    tSMP_INT_DATA smp_int_data;
    smp_int_data.status = SMP_INVALID_PARAMETERS;
    smp_sm_event(p_cb, SMP_AUTH_CMPL_EVT, &smp_int_data);
    return;
  }

  STREAM_TO_ARRAY(p_cb->peer_publ_key.x, p, BT_OCTET32_LEN);
  STREAM_TO_ARRAY(p_cb->peer_publ_key.y, p, BT_OCTET32_LEN);

  Point pt;
  memcpy(pt.x, p_cb->peer_publ_key.x, BT_OCTET32_LEN);
  memcpy(pt.y, p_cb->peer_publ_key.y, BT_OCTET32_LEN);

  if (!memcmp(p_cb->peer_publ_key.x, p_cb->loc_publ_key.x, BT_OCTET32_LEN)) {
    android_errorWriteLog(0x534e4554, "174886838");
    SMP_TRACE_WARNING("Remote and local public keys can't match");
    tSMP_INT_DATA smp;
    smp.status = SMP_PAIR_AUTH_FAIL;
    smp_sm_event(p_cb, SMP_AUTH_CMPL_EVT, &smp);
    return;
  }

  if (!ECC_ValidatePoint(pt)) {
    android_errorWriteLog(0x534e4554, "72377774");
    tSMP_INT_DATA smp;
    smp.status = SMP_PAIR_AUTH_FAIL;
    smp_sm_event(p_cb, SMP_AUTH_CMPL_EVT, &smp);
    return;
  }

  p_cb->flags |= SMP_PAIR_FLAG_HAVE_PEER_PUBL_KEY;

  smp_wait_for_both_public_keys(p_cb, NULL);
}

/*******************************************************************************
 * Function     smp_process_pairing_commitment
 * Description  process pairing commitment from peer device
 ******************************************************************************/
void smp_process_pairing_commitment(tSMP_CB* p_cb, tSMP_INT_DATA* p_data) {
  uint8_t* p = p_data->p_data;

  SMP_TRACE_DEBUG("%s", __func__);

  if (smp_command_has_invalid_parameters(p_cb)) {
    tSMP_INT_DATA smp_int_data;
    smp_int_data.status = SMP_INVALID_PARAMETERS;
    smp_sm_event(p_cb, SMP_AUTH_CMPL_EVT, &smp_int_data);
    return;
  }

  p_cb->flags |= SMP_PAIR_FLAG_HAVE_PEER_COMM;

  if (p != NULL) {
    STREAM_TO_ARRAY(p_cb->remote_commitment.data(), p, OCTET16_LEN);
  }
}

/*******************************************************************************
 * Function     smp_process_dhkey_check
 * Description  process DHKey Check from peer device
 ******************************************************************************/
void smp_process_dhkey_check(tSMP_CB* p_cb, tSMP_INT_DATA* p_data) {
  uint8_t* p = p_data->p_data;

  SMP_TRACE_DEBUG("%s", __func__);

  if (smp_command_has_invalid_parameters(p_cb)) {
    tSMP_INT_DATA smp_int_data;
    smp_int_data.status = SMP_INVALID_PARAMETERS;
    smp_sm_event(p_cb, SMP_AUTH_CMPL_EVT, &smp_int_data);
    return;
  }

  if (p != NULL) {
    STREAM_TO_ARRAY(p_cb->remote_dhkey_check.data(), p, OCTET16_LEN);
  }

  p_cb->flags |= SMP_PAIR_FLAG_HAVE_PEER_DHK_CHK;
}

/*******************************************************************************
 * Function     smp_process_keypress_notification
 * Description  process pairing keypress notification from peer device
 ******************************************************************************/
void smp_process_keypress_notification(tSMP_CB* p_cb, tSMP_INT_DATA* p_data) {
  uint8_t* p = p_data->p_data;

  SMP_TRACE_DEBUG("%s", __func__);
  p_cb->status = p_data->status;

  if (smp_command_has_invalid_parameters(p_cb)) {
    tSMP_INT_DATA smp_int_data;
    smp_int_data.status = SMP_INVALID_PARAMETERS;
    smp_sm_event(p_cb, SMP_AUTH_CMPL_EVT, &smp_int_data);
    return;
  }

  if (p != NULL) {
    STREAM_TO_UINT8(p_cb->peer_keypress_notification, p);
  } else {
    p_cb->peer_keypress_notification = BTM_SP_KEY_OUT_OF_RANGE;
  }
  p_cb->cb_evt = SMP_PEER_KEYPR_NOT_EVT;
}

/*******************************************************************************
 * Function     smp_br_process_pairing_command
 * Description  Process the SMP pairing request/response from peer device via
 *              BR/EDR transport.
 ******************************************************************************/
void smp_br_process_pairing_command(tSMP_CB* p_cb, tSMP_INT_DATA* p_data) {
  uint8_t* p = p_data->p_data;
  tBTM_SEC_DEV_REC* p_dev_rec = btm_find_dev(p_cb->pairing_bda);

  SMP_TRACE_DEBUG("%s", __func__);
  /* rejecting BR pairing request over non-SC BR link */
  if (!p_dev_rec->new_encryption_key_is_p256 && p_cb->role == HCI_ROLE_SLAVE) {
    tSMP_INT_DATA smp_int_data;
    smp_int_data.status = SMP_XTRANS_DERIVE_NOT_ALLOW;
    smp_br_state_machine_event(p_cb, SMP_BR_AUTH_CMPL_EVT, &smp_int_data);
    return;
  }

  /* erase all keys if it is slave proc pairing req*/
  if (p_dev_rec && (p_cb->role == HCI_ROLE_SLAVE))
    btm_sec_clear_ble_keys(p_dev_rec);

  p_cb->flags |= SMP_PAIR_FLAG_ENC_AFTER_PAIR;

  if (smp_command_has_invalid_length(p_cb)) {
    tSMP_INT_DATA smp_int_data;
    smp_int_data.status = SMP_INVALID_PARAMETERS;
    android_errorWriteLog(0x534e4554, "111213909");
    smp_br_state_machine_event(p_cb, SMP_BR_AUTH_CMPL_EVT, &smp_int_data);
    return;
  }

  STREAM_TO_UINT8(p_cb->peer_io_caps, p);
  STREAM_TO_UINT8(p_cb->peer_oob_flag, p);
  STREAM_TO_UINT8(p_cb->peer_auth_req, p);
  STREAM_TO_UINT8(p_cb->peer_enc_size, p);
  STREAM_TO_UINT8(p_cb->peer_i_key, p);
  STREAM_TO_UINT8(p_cb->peer_r_key, p);

  if (smp_command_has_invalid_parameters(p_cb)) {
    tSMP_INT_DATA smp_int_data;
    smp_int_data.status = SMP_INVALID_PARAMETERS;
    smp_br_state_machine_event(p_cb, SMP_BR_AUTH_CMPL_EVT, &smp_int_data);
    return;
  }

  /* peer (master) started pairing sending Pairing Request */
  /* or being master device always use received i/r key as keys to distribute */
  p_cb->local_i_key = p_cb->peer_i_key;
  p_cb->local_r_key = p_cb->peer_r_key;

  if (p_cb->role == HCI_ROLE_SLAVE) {
    p_dev_rec->new_encryption_key_is_p256 = false;
    /* shortcut to skip Security Grant step */
    p_cb->cb_evt = SMP_BR_KEYS_REQ_EVT;
  } else {
    /* Master receives pairing response */
    SMP_TRACE_DEBUG(
        "%s master rcvs valid PAIRING RESPONSE."
        " Supposed to move to key distribution phase. ",
        __func__);
  }

  /* auth_req received via BR/EDR SM channel is set to 0,
     but everything derived/exchanged has to be saved */
  p_cb->peer_auth_req |= SMP_AUTH_BOND;
  p_cb->loc_auth_req |= SMP_AUTH_BOND;
}

/*******************************************************************************
 * Function     smp_br_process_security_grant
 * Description  process security grant in case of pairing over BR/EDR transport.
 ******************************************************************************/
void smp_br_process_security_grant(tSMP_CB* p_cb, tSMP_INT_DATA* p_data) {
  SMP_TRACE_DEBUG("%s", __func__);
  if (p_data->status != SMP_SUCCESS) {
    smp_br_state_machine_event(p_cb, SMP_BR_AUTH_CMPL_EVT, p_data);
  } else {
    /* otherwise, start pairing; send IO request callback */
    p_cb->cb_evt = SMP_BR_KEYS_REQ_EVT;
  }
}

/*******************************************************************************
 * Function     smp_br_check_authorization_request
 * Description  sets the SMP kes to be derived/distribute over BR/EDR transport
 *              before starting the distribution/derivation
 ******************************************************************************/
void smp_br_check_authorization_request(tSMP_CB* p_cb, tSMP_INT_DATA* p_data) {
  SMP_TRACE_DEBUG("%s rcvs i_keys=0x%x r_keys=0x%x (i-initiator r-responder)",
                  __func__, p_cb->local_i_key, p_cb->local_r_key);

  /* In LE SC mode LK field is ignored when BR/EDR transport is used */
  p_cb->local_i_key &= ~SMP_SEC_KEY_TYPE_LK;
  p_cb->local_r_key &= ~SMP_SEC_KEY_TYPE_LK;

  /* In LE SC mode only IRK, IAI, CSRK are exchanged with the peer.
  ** Set local_r_key on master to expect only these keys. */
  if (p_cb->role == HCI_ROLE_MASTER) {
    p_cb->local_r_key &= (SMP_SEC_KEY_TYPE_ID | SMP_SEC_KEY_TYPE_CSRK);
  }

  /* Check if H7 function needs to be used for key derivation*/
  if ((p_cb->loc_auth_req & SMP_H7_SUPPORT_BIT) &&
      (p_cb->peer_auth_req & SMP_H7_SUPPORT_BIT)) {
    p_cb->key_derivation_h7_used = TRUE;
  }
  SMP_TRACE_DEBUG("%s: use h7 = %d", __func__, p_cb->key_derivation_h7_used);

  SMP_TRACE_DEBUG(
      "%s rcvs upgrades: i_keys=0x%x r_keys=0x%x (i-initiator r-responder)",
      __func__, p_cb->local_i_key, p_cb->local_r_key);

  if (/*((p_cb->peer_auth_req & SMP_AUTH_BOND) ||
          (p_cb->loc_auth_req & SMP_AUTH_BOND)) &&*/
      (p_cb->local_i_key || p_cb->local_r_key)) {
    smp_br_state_machine_event(p_cb, SMP_BR_BOND_REQ_EVT, NULL);

    /* if no peer key is expected, start master key distribution */
    if (p_cb->role == HCI_ROLE_MASTER && p_cb->local_r_key == 0)
      smp_key_distribution_by_transport(p_cb, NULL);
  } else {
    tSMP_INT_DATA smp_int_data;
    smp_int_data.status = SMP_SUCCESS;
    smp_br_state_machine_event(p_cb, SMP_BR_AUTH_CMPL_EVT, &smp_int_data);
  }
}

/*******************************************************************************
 * Function     smp_br_select_next_key
 * Description  selects the next key to derive/send when BR/EDR transport is
 *              used.
 ******************************************************************************/
void smp_br_select_next_key(tSMP_CB* p_cb, tSMP_INT_DATA* p_data) {
  SMP_TRACE_DEBUG("%s role=%d (0-master) r_keys=0x%x i_keys=0x%x", __func__,
                  p_cb->role, p_cb->local_r_key, p_cb->local_i_key);

  if (p_cb->role == HCI_ROLE_SLAVE ||
      (!p_cb->local_r_key && p_cb->role == HCI_ROLE_MASTER)) {
    smp_key_pick_key(p_cb, p_data);
  }

  if (!p_cb->local_i_key && !p_cb->local_r_key) {
    /* state check to prevent re-entrance */
    if (smp_get_br_state() == SMP_BR_STATE_BOND_PENDING) {
      if (p_cb->total_tx_unacked == 0) {
        tSMP_INT_DATA smp_int_data;
        smp_int_data.status = SMP_SUCCESS;
        smp_br_state_machine_event(p_cb, SMP_BR_AUTH_CMPL_EVT, &smp_int_data);
      } else {
        p_cb->wait_for_authorization_complete = true;
      }
    }
  }
}

/** process encryption information from peer device */
void smp_proc_enc_info(tSMP_CB* p_cb, tSMP_INT_DATA* p_data) {
  uint8_t* p = p_data->p_data;

  SMP_TRACE_DEBUG("%s", __func__);

  if (smp_command_has_invalid_parameters(p_cb)) {
    tSMP_INT_DATA smp_int_data;
    smp_int_data.status = SMP_INVALID_PARAMETERS;
    android_errorWriteLog(0x534e4554, "111937065");
    smp_sm_event(p_cb, SMP_AUTH_CMPL_EVT, &smp_int_data);
    return;
  }

  STREAM_TO_ARRAY(p_cb->ltk.data(), p, OCTET16_LEN);

  smp_key_distribution(p_cb, NULL);
}

/** process master ID from slave device */
void smp_proc_master_id(tSMP_CB* p_cb, tSMP_INT_DATA* p_data) {
  uint8_t* p = p_data->p_data;
  tBTM_LE_KEY_VALUE le_key;

  SMP_TRACE_DEBUG("%s", __func__);

  if (p_cb->rcvd_cmd_len < 11) {  // 1(Code) + 2(EDIV) + 8(Rand)
    android_errorWriteLog(0x534e4554, "111937027");
    SMP_TRACE_ERROR("%s: Invalid command length: %d, should be at least 11",
                    __func__, p_cb->rcvd_cmd_len);
    return;
  }

  smp_update_key_mask(p_cb, SMP_SEC_KEY_TYPE_ENC, true);

  STREAM_TO_UINT16(le_key.penc_key.ediv, p);
  STREAM_TO_ARRAY(le_key.penc_key.rand, p, BT_OCTET8_LEN);

  /* store the encryption keys from peer device */
  le_key.penc_key.ltk = p_cb->ltk;
  le_key.penc_key.sec_level = p_cb->sec_level;
  le_key.penc_key.key_size = p_cb->loc_enc_size;

  if ((p_cb->peer_auth_req & SMP_AUTH_BOND) &&
      (p_cb->loc_auth_req & SMP_AUTH_BOND))
    btm_sec_save_le_key(p_cb->pairing_bda, BTM_LE_KEY_PENC, &le_key, true);

  smp_key_distribution(p_cb, NULL);
}

/** process identity information from peer device */
void smp_proc_id_info(tSMP_CB* p_cb, tSMP_INT_DATA* p_data) {
  uint8_t* p = p_data->p_data;

  SMP_TRACE_DEBUG("%s", __func__);

  if (smp_command_has_invalid_parameters(p_cb)) {
    tSMP_INT_DATA smp_int_data;
    smp_int_data.status = SMP_INVALID_PARAMETERS;
    android_errorWriteLog(0x534e4554, "111937065");
    smp_sm_event(p_cb, SMP_AUTH_CMPL_EVT, &smp_int_data);
    return;
  }

  STREAM_TO_ARRAY(p_cb->tk.data(), p, OCTET16_LEN); /* reuse TK for IRK */
  smp_key_distribution_by_transport(p_cb, NULL);
}

/** process identity address from peer device */
void smp_proc_id_addr(tSMP_CB* p_cb, tSMP_INT_DATA* p_data) {
  uint8_t* p = p_data->p_data;
  tBTM_LE_KEY_VALUE pid_key;

  SMP_TRACE_DEBUG("%s", __func__);

  if (smp_command_has_invalid_parameters(p_cb)) {
    tSMP_INT_DATA smp_int_data;
    smp_int_data.status = SMP_INVALID_PARAMETERS;
    android_errorWriteLog(0x534e4554, "111214770");
    smp_sm_event(p_cb, SMP_AUTH_CMPL_EVT, &smp_int_data);
    return;
  }

  smp_update_key_mask(p_cb, SMP_SEC_KEY_TYPE_ID, true);

  STREAM_TO_UINT8(pid_key.pid_key.identity_addr_type, p);
  STREAM_TO_BDADDR(pid_key.pid_key.identity_addr, p);
  pid_key.pid_key.irk = p_cb->tk;

  /* to use as BD_ADDR for lk derived from ltk */
  p_cb->id_addr_rcvd = true;
  p_cb->id_addr_type = pid_key.pid_key.identity_addr_type;
  p_cb->id_addr = pid_key.pid_key.identity_addr;

  /* store the ID key from peer device */
  if ((p_cb->peer_auth_req & SMP_AUTH_BOND) &&
      (p_cb->loc_auth_req & SMP_AUTH_BOND))
    btm_sec_save_le_key(p_cb->pairing_bda, BTM_LE_KEY_PID, &pid_key, true);
  smp_key_distribution_by_transport(p_cb, NULL);
}

/* process security information from peer device */
void smp_proc_srk_info(tSMP_CB* p_cb, tSMP_INT_DATA* p_data) {
  tBTM_LE_KEY_VALUE le_key;

  SMP_TRACE_DEBUG("%s", __func__);

  if (smp_command_has_invalid_parameters(p_cb)) {
    tSMP_INT_DATA smp_int_data;
    smp_int_data.status = SMP_INVALID_PARAMETERS;
    android_errorWriteLog(0x534e4554, "111214470");
    smp_sm_event(p_cb, SMP_AUTH_CMPL_EVT, &smp_int_data);
    return;
  }

  smp_update_key_mask(p_cb, SMP_SEC_KEY_TYPE_CSRK, true);

  /* save CSRK to security record */
  le_key.pcsrk_key.sec_level = p_cb->sec_level;

  /* get peer CSRK */
  maybe_non_aligned_memcpy(le_key.pcsrk_key.csrk.data(), p_data->p_data,
                           OCTET16_LEN);

  /* initialize the peer counter */
  le_key.pcsrk_key.counter = 0;

  if ((p_cb->peer_auth_req & SMP_AUTH_BOND) &&
      (p_cb->loc_auth_req & SMP_AUTH_BOND))
    btm_sec_save_le_key(p_cb->pairing_bda, BTM_LE_KEY_PCSRK, &le_key, true);
  smp_key_distribution_by_transport(p_cb, NULL);
}

/*******************************************************************************
 * Function     smp_proc_compare
 * Description  process compare value
 ******************************************************************************/
void smp_proc_compare(tSMP_CB* p_cb, tSMP_INT_DATA* p_data) {
  SMP_TRACE_DEBUG("%s", __func__);
  if (!memcmp(p_cb->rconfirm.data(), p_data->key.p_data, OCTET16_LEN)) {
    /* compare the max encryption key size, and save the smaller one for the
     * link */
    if (p_cb->peer_enc_size < p_cb->loc_enc_size)
      p_cb->loc_enc_size = p_cb->peer_enc_size;

    if (p_cb->role == HCI_ROLE_SLAVE)
      smp_sm_event(p_cb, SMP_RAND_EVT, NULL);
    else {
      /* master device always use received i/r key as keys to distribute */
      p_cb->local_i_key = p_cb->peer_i_key;
      p_cb->local_r_key = p_cb->peer_r_key;

      smp_sm_event(p_cb, SMP_ENC_REQ_EVT, NULL);
    }

  } else {
    tSMP_INT_DATA smp_int_data;
    smp_int_data.status = SMP_CONFIRM_VALUE_ERR;
    p_cb->failure = SMP_CONFIRM_VALUE_ERR;
    smp_sm_event(p_cb, SMP_AUTH_CMPL_EVT, &smp_int_data);
  }
}

/*******************************************************************************
 * Function     smp_proc_sl_key
 * Description  process key ready events.
 ******************************************************************************/
void smp_proc_sl_key(tSMP_CB* p_cb, tSMP_INT_DATA* p_data) {
  uint8_t key_type = p_data->key.key_type;

  SMP_TRACE_DEBUG("%s", __func__);
  if (key_type == SMP_KEY_TYPE_TK) {
    smp_generate_srand_mrand_confirm(p_cb, NULL);
  } else if (key_type == SMP_KEY_TYPE_CFM) {
    smp_set_state(SMP_STATE_WAIT_CONFIRM);

    if (p_cb->flags & SMP_PAIR_FLAGS_CMD_CONFIRM)
      smp_sm_event(p_cb, SMP_CONFIRM_EVT, NULL);
  }
}

/*******************************************************************************
 * Function     smp_start_enc
 * Description  start encryption
 ******************************************************************************/
void smp_start_enc(tSMP_CB* p_cb, tSMP_INT_DATA* p_data) {
  tBTM_STATUS cmd;

  SMP_TRACE_DEBUG("%s", __func__);
  if (p_data != NULL) {
    cmd = btm_ble_start_encrypt(p_cb->pairing_bda, true,
                                (Octet16*)p_data->key.p_data);
  } else {
    cmd = btm_ble_start_encrypt(p_cb->pairing_bda, false, NULL);
  }

  if (cmd != BTM_CMD_STARTED && cmd != BTM_BUSY) {
    tSMP_INT_DATA smp_int_data;
    smp_int_data.status = SMP_ENC_FAIL;
    smp_sm_event(p_cb, SMP_AUTH_CMPL_EVT, &smp_int_data);
  }
}

/*******************************************************************************
 * Function     smp_proc_discard
 * Description   processing for discard security request
 ******************************************************************************/
void smp_proc_discard(tSMP_CB* p_cb, tSMP_INT_DATA* p_data) {
  SMP_TRACE_DEBUG("%s", __func__);
  if (!(p_cb->flags & SMP_PAIR_FLAGS_WE_STARTED_DD))
    smp_reset_control_value(p_cb);
}

/*******************************************************************************
 * Function     smp_enc_cmpl
 * Description   encryption success
 ******************************************************************************/
void smp_enc_cmpl(tSMP_CB* p_cb, tSMP_INT_DATA* p_data) {
  uint8_t enc_enable = p_data->status;

  SMP_TRACE_DEBUG("%s", __func__);
  tSMP_INT_DATA smp_int_data;
  smp_int_data.status = enc_enable ? SMP_SUCCESS : SMP_ENC_FAIL;
  smp_sm_event(p_cb, SMP_AUTH_CMPL_EVT, &smp_int_data);
}

/*******************************************************************************
 * Function     smp_check_auth_req
 * Description  check authentication request
 ******************************************************************************/
void smp_check_auth_req(tSMP_CB* p_cb, tSMP_INT_DATA* p_data) {
  uint8_t enc_enable = p_data->status;

  SMP_TRACE_DEBUG(
      "%s rcvs enc_enable=%d i_keys=0x%x r_keys=0x%x (i-initiator r-responder)",
      __func__, enc_enable, p_cb->local_i_key, p_cb->local_r_key);
  if (enc_enable == 1) {
    if (p_cb->le_secure_connections_mode_is_used) {
      /* In LE SC mode LTK is used instead of STK and has to be always saved */
      p_cb->local_i_key |= SMP_SEC_KEY_TYPE_ENC;
      p_cb->local_r_key |= SMP_SEC_KEY_TYPE_ENC;

      /* In LE SC mode LK is derived from LTK only if both sides request it */
      if (!(p_cb->local_i_key & SMP_SEC_KEY_TYPE_LK) ||
          !(p_cb->local_r_key & SMP_SEC_KEY_TYPE_LK)) {
        p_cb->local_i_key &= ~SMP_SEC_KEY_TYPE_LK;
        p_cb->local_r_key &= ~SMP_SEC_KEY_TYPE_LK;
      }

      /* In LE SC mode only IRK, IAI, CSRK are exchanged with the peer.
      ** Set local_r_key on master to expect only these keys.
      */
      if (p_cb->role == HCI_ROLE_MASTER) {
        p_cb->local_r_key &= (SMP_SEC_KEY_TYPE_ID | SMP_SEC_KEY_TYPE_CSRK);
      }
    } else {
      /* in legacy mode derivation of BR/EDR LK is not supported */
      p_cb->local_i_key &= ~SMP_SEC_KEY_TYPE_LK;
      p_cb->local_r_key &= ~SMP_SEC_KEY_TYPE_LK;
    }
    SMP_TRACE_DEBUG(
        "%s rcvs upgrades: i_keys=0x%x r_keys=0x%x (i-initiator r-responder)",
        __func__, p_cb->local_i_key, p_cb->local_r_key);

    if (/*((p_cb->peer_auth_req & SMP_AUTH_BOND) ||
         (p_cb->loc_auth_req & SMP_AUTH_BOND)) &&*/
        (p_cb->local_i_key || p_cb->local_r_key)) {
      smp_sm_event(p_cb, SMP_BOND_REQ_EVT, NULL);
    } else {
      tSMP_INT_DATA smp_int_data;
      smp_int_data.status = enc_enable ? SMP_SUCCESS : SMP_ENC_FAIL;
      smp_sm_event(p_cb, SMP_AUTH_CMPL_EVT, &smp_int_data);
    }
  } else if (enc_enable == 0) {
    tSMP_INT_DATA smp_int_data;
    smp_int_data.status = enc_enable ? SMP_SUCCESS : SMP_ENC_FAIL;
    /* if failed for encryption after pairing, send callback */
    if (p_cb->flags & SMP_PAIR_FLAG_ENC_AFTER_PAIR)
      smp_sm_event(p_cb, SMP_AUTH_CMPL_EVT, &smp_int_data);
    /* if enc failed for old security information */
    /* if master device, clean up and abck to idle; slave device do nothing */
    else if (p_cb->role == HCI_ROLE_MASTER) {
      smp_sm_event(p_cb, SMP_AUTH_CMPL_EVT, &smp_int_data);
    }
  }
}

/*******************************************************************************
 * Function     smp_key_pick_key
 * Description  Pick a key distribution function based on the key mask.
 ******************************************************************************/
void smp_key_pick_key(tSMP_CB* p_cb, tSMP_INT_DATA* p_data) {
  uint8_t key_to_dist =
      (p_cb->role == HCI_ROLE_SLAVE) ? p_cb->local_r_key : p_cb->local_i_key;
  uint8_t i = 0;

  SMP_TRACE_DEBUG("%s key_to_dist=0x%x", __func__, key_to_dist);
  while (i < SMP_KEY_DIST_TYPE_MAX) {
    SMP_TRACE_DEBUG("key to send = %02x, i = %d", key_to_dist, i);

    if (key_to_dist & (1 << i)) {
      SMP_TRACE_DEBUG("smp_distribute_act[%d]", i);
      (*smp_distribute_act[i])(p_cb, p_data);
      break;
    }
    i++;
  }
}
/*******************************************************************************
 * Function     smp_key_distribution
 * Description  start key distribution if required.
 ******************************************************************************/
void smp_key_distribution(tSMP_CB* p_cb, tSMP_INT_DATA* p_data) {
  SMP_TRACE_DEBUG("%s role=%d (0-master) r_keys=0x%x i_keys=0x%x", __func__,
                  p_cb->role, p_cb->local_r_key, p_cb->local_i_key);

  if (p_cb->role == HCI_ROLE_SLAVE ||
      (!p_cb->local_r_key && p_cb->role == HCI_ROLE_MASTER)) {
    smp_key_pick_key(p_cb, p_data);
  }

  if (!p_cb->local_i_key && !p_cb->local_r_key) {
    /* state check to prevent re-entrant */
    if (smp_get_state() == SMP_STATE_BOND_PENDING) {
      if (p_cb->derive_lk) {
        tBTM_SEC_DEV_REC* p_dev_rec = btm_find_dev(p_cb->pairing_bda);
        if (!(p_dev_rec->sec_flags & BTM_SEC_LE_LINK_KEY_AUTHED) &&
            (p_dev_rec->sec_flags & BTM_SEC_LINK_KEY_AUTHED)) {
          SMP_TRACE_DEBUG(
              "%s BR key is higher security than existing LE keys, don't "
              "derive LK from LTK",
              __func__);
          android_errorWriteLog(0x534e4554, "158854097");
        } else {
          smp_derive_link_key_from_long_term_key(p_cb, NULL);
        }
        p_cb->derive_lk = false;
      }

      if (p_cb->total_tx_unacked == 0) {
        /*
         * Instead of declaring authorization complete immediately,
         * delay the event from being sent by SMP_DELAYED_AUTH_TIMEOUT_MS.
         * This allows the slave to send over Pairing Failed if the
         * last key is rejected.  During this waiting window, the
         * state should remain in SMP_STATE_BOND_PENDING.
         */
        if (!alarm_is_scheduled(p_cb->delayed_auth_timer_ent)) {
          SMP_TRACE_DEBUG("%s delaying auth complete.", __func__);
          alarm_set_on_mloop(p_cb->delayed_auth_timer_ent,
                             SMP_DELAYED_AUTH_TIMEOUT_MS,
                             smp_delayed_auth_complete_timeout, NULL);
        }
      } else {
        p_cb->wait_for_authorization_complete = true;
      }
    }
  }
}

/*******************************************************************************
 * Function         smp_decide_association_model
 * Description      This function is called to select assoc model to be used for
 *                  STK generation and to start STK generation process.
 *
 ******************************************************************************/
void smp_decide_association_model(tSMP_CB* p_cb, tSMP_INT_DATA* p_data) {
  uint8_t int_evt = 0;
  tSMP_INT_DATA smp_int_data;

  SMP_TRACE_DEBUG("%s Association Model = %d", __func__,
                  p_cb->selected_association_model);

  switch (p_cb->selected_association_model) {
    case SMP_MODEL_ENCRYPTION_ONLY: /* TK = 0, go calculate Confirm */
      if (p_cb->role == HCI_ROLE_MASTER &&
          ((p_cb->peer_auth_req & SMP_AUTH_YN_BIT) != 0) &&
          ((p_cb->loc_auth_req & SMP_AUTH_YN_BIT) == 0)) {
        SMP_TRACE_ERROR(
            "IO capability does not meet authentication requirement");
        smp_int_data.status = SMP_PAIR_AUTH_FAIL;
        int_evt = SMP_AUTH_CMPL_EVT;
      } else {
        if (!is_atv_device() &&
            (p_cb->local_io_capability == SMP_IO_CAP_IO ||
             p_cb->local_io_capability == SMP_IO_CAP_KBDISP)) {
          /* display consent dialog if this device has a display */
          SMP_TRACE_DEBUG("ENCRYPTION_ONLY showing Consent Dialog");
          p_cb->cb_evt = SMP_CONSENT_REQ_EVT;
          smp_set_state(SMP_STATE_WAIT_NONCE);
          smp_sm_event(p_cb, SMP_SC_DSPL_NC_EVT, NULL);
        } else {
          p_cb->sec_level = SMP_SEC_UNAUTHENTICATE;
          SMP_TRACE_EVENT("p_cb->sec_level =%d (SMP_SEC_UNAUTHENTICATE) ",
                          p_cb->sec_level);

          tSMP_KEY key;
          key.key_type = SMP_KEY_TYPE_TK;
          key.p_data = p_cb->tk.data();
          smp_int_data.key = key;

          p_cb->tk = {0};
          /* TK, ready  */
          int_evt = SMP_KEY_READY_EVT;
        }
      }
      break;

    case SMP_MODEL_PASSKEY:
      p_cb->sec_level = SMP_SEC_AUTHENTICATED;
      SMP_TRACE_EVENT("p_cb->sec_level =%d (SMP_SEC_AUTHENTICATED) ",
                      p_cb->sec_level);

      p_cb->cb_evt = SMP_PASSKEY_REQ_EVT;
      int_evt = SMP_TK_REQ_EVT;
      break;

    case SMP_MODEL_OOB:
      SMP_TRACE_ERROR("Association Model = SMP_MODEL_OOB");
      p_cb->sec_level = SMP_SEC_AUTHENTICATED;
      SMP_TRACE_EVENT("p_cb->sec_level =%d (SMP_SEC_AUTHENTICATED) ",
                      p_cb->sec_level);

      p_cb->cb_evt = SMP_OOB_REQ_EVT;
      int_evt = SMP_TK_REQ_EVT;
      break;

    case SMP_MODEL_KEY_NOTIF:
      p_cb->sec_level = SMP_SEC_AUTHENTICATED;
      SMP_TRACE_DEBUG("Need to generate Passkey");

      /* generate passkey and notify application */
      smp_generate_passkey(p_cb, NULL);
      break;

    case SMP_MODEL_SEC_CONN_JUSTWORKS:
    case SMP_MODEL_SEC_CONN_NUM_COMP:
    case SMP_MODEL_SEC_CONN_PASSKEY_ENT:
    case SMP_MODEL_SEC_CONN_PASSKEY_DISP:
    case SMP_MODEL_SEC_CONN_OOB:
      int_evt = SMP_PUBL_KEY_EXCH_REQ_EVT;
      break;

    case SMP_MODEL_OUT_OF_RANGE:
      SMP_TRACE_ERROR("Association Model = SMP_MODEL_OUT_OF_RANGE (failed)");
      smp_int_data.status = SMP_UNKNOWN_IO_CAP;
      int_evt = SMP_AUTH_CMPL_EVT;
      break;

    default:
      SMP_TRACE_ERROR(
          "Association Model = %d (SOMETHING IS WRONG WITH THE CODE)",
          p_cb->selected_association_model);
      smp_int_data.status = SMP_UNKNOWN_IO_CAP;
      int_evt = SMP_AUTH_CMPL_EVT;
  }

  SMP_TRACE_EVENT("sec_level=%d ", p_cb->sec_level);
  if (int_evt) smp_sm_event(p_cb, int_evt, &smp_int_data);
}

/*******************************************************************************
 * Function     smp_process_io_response
 * Description  process IO response for a slave device.
 ******************************************************************************/
void smp_process_io_response(tSMP_CB* p_cb, tSMP_INT_DATA* p_data) {

  SMP_TRACE_DEBUG("%s", __func__);
  if (p_cb->flags & SMP_PAIR_FLAGS_WE_STARTED_DD) {
    /* pairing started by local (slave) Security Request */
    smp_set_state(SMP_STATE_SEC_REQ_PENDING);
    smp_send_cmd(SMP_OPCODE_SEC_REQ, p_cb);
  } else /* plan to send pairing respond */
  {
    /* pairing started by peer (master) Pairing Request */
    p_cb->selected_association_model = smp_select_association_model(p_cb);

    if (p_cb->secure_connections_only_mode_required &&
        (!(p_cb->le_secure_connections_mode_is_used) ||
         (p_cb->selected_association_model == SMP_MODEL_SEC_CONN_JUSTWORKS))) {
      SMP_TRACE_ERROR(
          "Slave requires secure connection only mode "
          "but it can't be provided -> Slave fails pairing");
      tSMP_INT_DATA smp_int_data;
      smp_int_data.status = SMP_PAIR_AUTH_FAIL;
      smp_sm_event(p_cb, SMP_AUTH_CMPL_EVT, &smp_int_data);
      return;
    }

    if (p_cb->selected_association_model == SMP_MODEL_SEC_CONN_OOB) {
      if (smp_request_oob_data(p_cb)) return;
    }

    // PTS Testing failure modes
    if (pts_test_send_authentication_complete_failure(p_cb)) return;

    smp_send_pair_rsp(p_cb, NULL);
  }
}

/*******************************************************************************
 * Function     smp_br_process_slave_keys_response
 * Description  process application keys response for a slave device
 *              (BR/EDR transport).
 ******************************************************************************/
void smp_br_process_slave_keys_response(tSMP_CB* p_cb, tSMP_INT_DATA* p_data) {
  smp_br_send_pair_response(p_cb, NULL);
}

/*******************************************************************************
 * Function     smp_br_send_pair_response
 * Description  actions related to sending pairing response over BR/EDR
 *              transport.
 ******************************************************************************/
void smp_br_send_pair_response(tSMP_CB* p_cb, tSMP_INT_DATA* p_data) {
  SMP_TRACE_DEBUG("%s", __func__);

  p_cb->local_i_key &= p_cb->peer_i_key;
  p_cb->local_r_key &= p_cb->peer_r_key;

  smp_send_cmd(SMP_OPCODE_PAIRING_RSP, p_cb);
}

/*******************************************************************************
 * Function         smp_pairing_cmpl
 * Description      This function is called to send the pairing complete
 *                  callback and remove the connection if needed.
 ******************************************************************************/
void smp_pairing_cmpl(tSMP_CB* p_cb, tSMP_INT_DATA* p_data) {
  if (p_cb->total_tx_unacked == 0) {
    /* process the pairing complete */
    smp_proc_pairing_cmpl(p_cb);
  }
}

/*******************************************************************************
 * Function         smp_pair_terminate
 * Description      This function is called to send the pairing complete
 *                  callback and remove the connection if needed.
 ******************************************************************************/
void smp_pair_terminate(tSMP_CB* p_cb, tSMP_INT_DATA* p_data) {
  SMP_TRACE_DEBUG("%s", __func__);
  p_cb->status = SMP_CONN_TOUT;
  smp_proc_pairing_cmpl(p_cb);
}

/*******************************************************************************
 * Function         smp_idle_terminate
 * Description      This function calledin idle state to determine to send
 *                  authentication complete or not.
 ******************************************************************************/
void smp_idle_terminate(tSMP_CB* p_cb, tSMP_INT_DATA* p_data) {
  if (p_cb->flags & SMP_PAIR_FLAGS_WE_STARTED_DD) {
    SMP_TRACE_DEBUG("Pairing terminated at IDLE state.");
    p_cb->status = SMP_FAIL;
    smp_proc_pairing_cmpl(p_cb);
  }
}

/*******************************************************************************
 * Function     smp_both_have_public_keys
 * Description  The function is called when both local and peer public keys are
 *              saved.
 *              Actions:
 *              - invokes DHKey computation;
 *              - on slave side invokes sending local public key to the peer.
 *              - invokes SC phase 1 process.
 ******************************************************************************/
void smp_both_have_public_keys(tSMP_CB* p_cb, tSMP_INT_DATA* p_data) {
  SMP_TRACE_DEBUG("%s", __func__);

  /* invokes DHKey computation */
  smp_compute_dhkey(p_cb);

  /* on slave side invokes sending local public key to the peer */
  if (p_cb->role == HCI_ROLE_SLAVE) smp_send_pair_public_key(p_cb, NULL);

  smp_sm_event(p_cb, SMP_SC_DHKEY_CMPLT_EVT, NULL);
}

/*******************************************************************************
 * Function     smp_start_secure_connection_phase1
 * Description  Start Secure Connection phase1 i.e. invokes initialization of
 *              Secure Connection phase 1 parameters and starts building/sending
 *              to the peer messages appropriate for the role and association
 *              model.
 ******************************************************************************/
void smp_start_secure_connection_phase1(tSMP_CB* p_cb, tSMP_INT_DATA* p_data) {
  SMP_TRACE_DEBUG("%s", __func__);

  if (p_cb->selected_association_model == SMP_MODEL_SEC_CONN_JUSTWORKS) {
    p_cb->sec_level = SMP_SEC_UNAUTHENTICATE;
    SMP_TRACE_EVENT("p_cb->sec_level =%d (SMP_SEC_UNAUTHENTICATE) ",
                    p_cb->sec_level);
  } else {
    p_cb->sec_level = SMP_SEC_AUTHENTICATED;
    SMP_TRACE_EVENT("p_cb->sec_level =%d (SMP_SEC_AUTHENTICATED) ",
                    p_cb->sec_level);
  }

  switch (p_cb->selected_association_model) {
    case SMP_MODEL_SEC_CONN_JUSTWORKS:
    case SMP_MODEL_SEC_CONN_NUM_COMP:
      p_cb->local_random = {0};
      smp_start_nonce_generation(p_cb);
      break;
    case SMP_MODEL_SEC_CONN_PASSKEY_ENT:
      /* user has to provide passkey */
      p_cb->cb_evt = SMP_PASSKEY_REQ_EVT;
      smp_sm_event(p_cb, SMP_TK_REQ_EVT, NULL);
      break;
    case SMP_MODEL_SEC_CONN_PASSKEY_DISP:
      /* passkey has to be provided to user */
      SMP_TRACE_DEBUG("Need to generate SC Passkey");
      smp_generate_passkey(p_cb, NULL);
      break;
    case SMP_MODEL_SEC_CONN_OOB:
      /* use the available OOB information */
      smp_process_secure_connection_oob_data(p_cb, NULL);
      break;
    default:
      SMP_TRACE_ERROR("Association Model = %d is not used in LE SC",
                      p_cb->selected_association_model);
      break;
  }
}

/*******************************************************************************
 * Function     smp_process_local_nonce
 * Description  The function processes new local nonce.
 *
 * Note         It is supposed to be called in SC phase1.
 ******************************************************************************/
void smp_process_local_nonce(tSMP_CB* p_cb, tSMP_INT_DATA* p_data) {
  SMP_TRACE_DEBUG("%s", __func__);

  switch (p_cb->selected_association_model) {
    case SMP_MODEL_SEC_CONN_JUSTWORKS:
    case SMP_MODEL_SEC_CONN_NUM_COMP:
      if (p_cb->role == HCI_ROLE_SLAVE) {
        /* slave calculates and sends local commitment */
        smp_calculate_local_commitment(p_cb);
        smp_send_commitment(p_cb, NULL);
        /* slave has to wait for peer nonce */
        smp_set_state(SMP_STATE_WAIT_NONCE);
      } else /* i.e. master */
      {
        if (p_cb->flags & SMP_PAIR_FLAG_HAVE_PEER_COMM) {
          /* slave commitment is already received, send local nonce, wait for
           * remote nonce*/
          SMP_TRACE_DEBUG(
              "master in assoc mode = %d "
              "already rcvd slave commitment - race condition",
              p_cb->selected_association_model);
          p_cb->flags &= ~SMP_PAIR_FLAG_HAVE_PEER_COMM;
          smp_send_rand(p_cb, NULL);
          smp_set_state(SMP_STATE_WAIT_NONCE);
        }
      }
      break;
    case SMP_MODEL_SEC_CONN_PASSKEY_ENT:
    case SMP_MODEL_SEC_CONN_PASSKEY_DISP:
      smp_calculate_local_commitment(p_cb);

      if (p_cb->role == HCI_ROLE_MASTER) {
        smp_send_commitment(p_cb, NULL);
      } else /* slave */
      {
        if (p_cb->flags & SMP_PAIR_FLAG_HAVE_PEER_COMM) {
          /* master commitment is already received */
          smp_send_commitment(p_cb, NULL);
          smp_set_state(SMP_STATE_WAIT_NONCE);
        }
      }
      break;
    case SMP_MODEL_SEC_CONN_OOB:
      if (p_cb->role == HCI_ROLE_MASTER) {
        smp_send_rand(p_cb, NULL);
      }

      smp_set_state(SMP_STATE_WAIT_NONCE);
      break;
    default:
      SMP_TRACE_ERROR("Association Model = %d is not used in LE SC",
                      p_cb->selected_association_model);
      break;
  }
}

/*******************************************************************************
 * Function     smp_process_peer_nonce
 * Description  The function processes newly received and saved in CB peer
 *              nonce. The actions depend on the selected association model and
 *              the role.
 *
 * Note         It is supposed to be called in SC phase1.
 ******************************************************************************/
void smp_process_peer_nonce(tSMP_CB* p_cb, tSMP_INT_DATA* p_data) {
  SMP_TRACE_DEBUG("%s start ", __func__);

  // PTS Testing failure modes
  if (p_cb->cert_failure == SMP_CONFIRM_VALUE_ERR) {
    SMP_TRACE_ERROR("%s failure case = %d", __func__, p_cb->cert_failure);
    tSMP_INT_DATA smp_int_data;
    smp_int_data.status = SMP_CONFIRM_VALUE_ERR;
    p_cb->failure = SMP_CONFIRM_VALUE_ERR;
    smp_sm_event(p_cb, SMP_AUTH_CMPL_EVT, &smp_int_data);
    return;
  }
  // PTS Testing failure modes (for LT)
  if ((p_cb->cert_failure == SMP_NUMERIC_COMPAR_FAIL) &&
      (p_cb->selected_association_model == SMP_MODEL_SEC_CONN_JUSTWORKS) &&
      (p_cb->role == HCI_ROLE_SLAVE)) {
    SMP_TRACE_ERROR("%s failure case = %d", __func__, p_cb->cert_failure);
    tSMP_INT_DATA smp_int_data;
    smp_int_data.status = SMP_NUMERIC_COMPAR_FAIL;
    p_cb->failure = SMP_NUMERIC_COMPAR_FAIL;
    smp_sm_event(p_cb, SMP_AUTH_CMPL_EVT, &smp_int_data);
    return;
  }

  switch (p_cb->selected_association_model) {
    case SMP_MODEL_SEC_CONN_JUSTWORKS:
    case SMP_MODEL_SEC_CONN_NUM_COMP:
      /* in these models only master receives commitment */
      if (p_cb->role == HCI_ROLE_MASTER) {
        if (!smp_check_commitment(p_cb)) {
          tSMP_INT_DATA smp_int_data;
          smp_int_data.status = SMP_CONFIRM_VALUE_ERR;
          p_cb->failure = SMP_CONFIRM_VALUE_ERR;
          smp_sm_event(p_cb, SMP_AUTH_CMPL_EVT, &smp_int_data);
          break;
        }
      } else {
        /* slave sends local nonce */
        smp_send_rand(p_cb, NULL);
      }

      if (p_cb->selected_association_model == SMP_MODEL_SEC_CONN_JUSTWORKS) {
        if (!is_atv_device() &&
            (p_cb->local_io_capability == SMP_IO_CAP_IO ||
             p_cb->local_io_capability == SMP_IO_CAP_KBDISP)) {
          /* display consent dialog */
          SMP_TRACE_DEBUG("JUST WORKS showing Consent Dialog");
          p_cb->cb_evt = SMP_CONSENT_REQ_EVT;
          smp_set_state(SMP_STATE_WAIT_NONCE);
          smp_sm_event(p_cb, SMP_SC_DSPL_NC_EVT, NULL);
        } else {
          /* go directly to phase 2 */
          smp_sm_event(p_cb, SMP_SC_PHASE1_CMPLT_EVT, NULL);
        }
      } else /* numeric comparison */
      {
        smp_set_state(SMP_STATE_WAIT_NONCE);
        smp_sm_event(p_cb, SMP_SC_CALC_NC_EVT, NULL);
      }
      break;
    case SMP_MODEL_SEC_CONN_PASSKEY_ENT:
    case SMP_MODEL_SEC_CONN_PASSKEY_DISP:
      if (!smp_check_commitment(p_cb) &&
          p_cb->cert_failure != SMP_NUMERIC_COMPAR_FAIL) {
        tSMP_INT_DATA smp_int_data;
        smp_int_data.status = SMP_CONFIRM_VALUE_ERR;
        p_cb->failure = SMP_CONFIRM_VALUE_ERR;
        smp_sm_event(p_cb, SMP_AUTH_CMPL_EVT, &smp_int_data);
        break;
      }

      if (p_cb->role == HCI_ROLE_SLAVE) {
        smp_send_rand(p_cb, NULL);
      }

      if (++p_cb->round < 20) {
        smp_set_state(SMP_STATE_SEC_CONN_PHS1_START);
        p_cb->flags &= ~SMP_PAIR_FLAG_HAVE_PEER_COMM;
        smp_start_nonce_generation(p_cb);
        break;
      }

      smp_sm_event(p_cb, SMP_SC_PHASE1_CMPLT_EVT, NULL);
      break;
    case SMP_MODEL_SEC_CONN_OOB:
      if (p_cb->role == HCI_ROLE_SLAVE) {
        smp_send_rand(p_cb, NULL);
      }

      smp_sm_event(p_cb, SMP_SC_PHASE1_CMPLT_EVT, NULL);
      break;
    default:
      SMP_TRACE_ERROR("Association Model = %d is not used in LE SC",
                      p_cb->selected_association_model);
      break;
  }

  SMP_TRACE_DEBUG("%s end ", __func__);
}

/*******************************************************************************
 * Function     smp_match_dhkey_checks
 * Description  checks if the calculated peer DHKey Check value is the same as
 *              received from the peer DHKey check value.
 ******************************************************************************/
void smp_match_dhkey_checks(tSMP_CB* p_cb, tSMP_INT_DATA* p_data) {
  SMP_TRACE_DEBUG("%s", __func__);

  if (memcmp(p_data->key.p_data, p_cb->remote_dhkey_check.data(),
             OCTET16_LEN)) {
    SMP_TRACE_WARNING("dhkey chcks do no match");
    tSMP_INT_DATA smp_int_data;
    smp_int_data.status = SMP_DHKEY_CHK_FAIL;
    p_cb->failure = SMP_DHKEY_CHK_FAIL;
    smp_sm_event(p_cb, SMP_AUTH_CMPL_EVT, &smp_int_data);
    return;
  }

  SMP_TRACE_EVENT("dhkey chcks match");

  /* compare the max encryption key size, and save the smaller one for the link
   */
  if (p_cb->peer_enc_size < p_cb->loc_enc_size)
    p_cb->loc_enc_size = p_cb->peer_enc_size;

  if (p_cb->role == HCI_ROLE_SLAVE) {
    smp_sm_event(p_cb, SMP_PAIR_DHKEY_CHCK_EVT, NULL);
  } else {
    /* master device always use received i/r key as keys to distribute */
    p_cb->local_i_key = p_cb->peer_i_key;
    p_cb->local_r_key = p_cb->peer_r_key;
    smp_sm_event(p_cb, SMP_ENC_REQ_EVT, NULL);
  }
}

/*******************************************************************************
 * Function     smp_move_to_secure_connections_phase2
 * Description  Signal State Machine to start SC phase 2 initialization (to
 *              compute local DHKey Check value).
 *
 * Note         SM is supposed to be in the state SMP_STATE_SEC_CONN_PHS2_START.
 ******************************************************************************/
void smp_move_to_secure_connections_phase2(tSMP_CB* p_cb,
                                           tSMP_INT_DATA* p_data) {
  SMP_TRACE_DEBUG("%s", __func__);
  smp_sm_event(p_cb, SMP_SC_PHASE1_CMPLT_EVT, NULL);
}

/*******************************************************************************
 * Function     smp_phase_2_dhkey_checks_are_present
 * Description  generates event if dhkey check from the peer is already
 *              received.
 *
 * Note         It is supposed to be used on slave to prevent race condition.
 *              It is supposed to be called after slave dhkey check is
 *              calculated.
 ******************************************************************************/
void smp_phase_2_dhkey_checks_are_present(tSMP_CB* p_cb,
                                          tSMP_INT_DATA* p_data) {
  SMP_TRACE_DEBUG("%s", __func__);

  if (p_cb->flags & SMP_PAIR_FLAG_HAVE_PEER_DHK_CHK)
    smp_sm_event(p_cb, SMP_SC_2_DHCK_CHKS_PRES_EVT, NULL);
}

/*******************************************************************************
 * Function     smp_wait_for_both_public_keys
 * Description  generates SMP_BOTH_PUBL_KEYS_RCVD_EVT event when both local and
 *              master public keys are available.
 *
 * Note         on the slave it is used to prevent race condition.
 *
 ******************************************************************************/
void smp_wait_for_both_public_keys(tSMP_CB* p_cb, tSMP_INT_DATA* p_data) {
  SMP_TRACE_DEBUG("%s", __func__);

  if ((p_cb->flags & SMP_PAIR_FLAG_HAVE_PEER_PUBL_KEY) &&
      (p_cb->flags & SMP_PAIR_FLAG_HAVE_LOCAL_PUBL_KEY)) {
    if ((p_cb->role == HCI_ROLE_SLAVE) &&
        ((p_cb->req_oob_type == SMP_OOB_LOCAL) ||
         (p_cb->req_oob_type == SMP_OOB_BOTH))) {
      smp_set_state(SMP_STATE_PUBLIC_KEY_EXCH);
    }
    smp_sm_event(p_cb, SMP_BOTH_PUBL_KEYS_RCVD_EVT, NULL);
  }
}

/*******************************************************************************
 * Function     smp_start_passkey_verification
 * Description  Starts SC passkey entry verification.
 ******************************************************************************/
void smp_start_passkey_verification(tSMP_CB* p_cb, tSMP_INT_DATA* p_data) {
  uint8_t* p = NULL;

  SMP_TRACE_DEBUG("%s", __func__);
  p = p_cb->local_random.data();
  UINT32_TO_STREAM(p, p_data->passkey);

  p = p_cb->peer_random.data();
  UINT32_TO_STREAM(p, p_data->passkey);

  p_cb->round = 0;
  smp_start_nonce_generation(p_cb);
}

/*******************************************************************************
 * Function     smp_process_secure_connection_oob_data
 * Description  Processes local/peer SC OOB data received from somewhere.
 ******************************************************************************/
void smp_process_secure_connection_oob_data(tSMP_CB* p_cb,
                                            tSMP_INT_DATA* p_data) {
  SMP_TRACE_DEBUG("%s", __func__);

  tSMP_SC_OOB_DATA* p_sc_oob_data = &p_cb->sc_oob_data;
  if (p_sc_oob_data->loc_oob_data.present) {
    p_cb->local_random = p_sc_oob_data->loc_oob_data.randomizer;
  } else {
    SMP_TRACE_EVENT("%s: local OOB randomizer is absent", __func__);
    p_cb->local_random = {0};
  }

  if (p_cb->peer_oob_flag == SMP_OOB_PRESENT && !p_sc_oob_data->loc_oob_data.present) {
    SMP_TRACE_WARNING(
        "local OOB data is not present but peer claims to have received it; dropping "
        "connection", __func__);
    tSMP_INT_DATA smp_int_data{};
    smp_int_data.status = SMP_OOB_FAIL;
    smp_sm_event(p_cb, SMP_AUTH_CMPL_EVT, &smp_int_data);
    return;
  }

  if (!p_sc_oob_data->peer_oob_data.present) {
    SMP_TRACE_EVENT("%s: peer OOB data is absent", __func__);
    p_cb->peer_random = {0};
  } else {
    p_cb->peer_random = p_sc_oob_data->peer_oob_data.randomizer;
    p_cb->remote_commitment = p_sc_oob_data->peer_oob_data.commitment;

    /* check commitment */
    if (!smp_check_commitment(p_cb)) {
      tSMP_INT_DATA smp_int_data;
      smp_int_data.status = SMP_CONFIRM_VALUE_ERR;
      p_cb->failure = SMP_CONFIRM_VALUE_ERR;
      smp_sm_event(p_cb, SMP_AUTH_CMPL_EVT, &smp_int_data);
      return;
    }

    if (p_cb->peer_oob_flag != SMP_OOB_PRESENT) {
      /* the peer doesn't have local randomiser */
      SMP_TRACE_EVENT(
          "%s: peer didn't receive local OOB data, set local randomizer to 0",
          __func__);
      p_cb->local_random = {0};
    }
  }

  print128(p_cb->local_random, (const uint8_t*)"local OOB randomizer");
  print128(p_cb->peer_random, (const uint8_t*)"peer OOB randomizer");
  smp_start_nonce_generation(p_cb);
}

/*******************************************************************************
 * Function     smp_set_local_oob_keys
 * Description  Saves calculated private/public keys in
 *              sc_oob_data.loc_oob_data, starts nonce generation
 *              (to be saved in sc_oob_data.loc_oob_data.randomizer).
 ******************************************************************************/
void smp_set_local_oob_keys(tSMP_CB* p_cb, tSMP_INT_DATA* p_data) {
  SMP_TRACE_DEBUG("%s", __func__);

  memcpy(p_cb->sc_oob_data.loc_oob_data.private_key_used, p_cb->private_key,
         BT_OCTET32_LEN);
  p_cb->sc_oob_data.loc_oob_data.publ_key_used = p_cb->loc_publ_key;
  smp_start_nonce_generation(p_cb);
}

/*******************************************************************************
 * Function     smp_set_local_oob_random_commitment
 * Description  Saves calculated randomizer and commitment in
 *              sc_oob_data.loc_oob_data, passes sc_oob_data.loc_oob_data up
 *              for safekeeping.
 ******************************************************************************/
void smp_set_local_oob_random_commitment(tSMP_CB* p_cb, tSMP_INT_DATA* p_data) {
  SMP_TRACE_DEBUG("%s", __func__);
  p_cb->sc_oob_data.loc_oob_data.randomizer = p_cb->rand;

  p_cb->sc_oob_data.loc_oob_data.commitment =
      crypto_toolbox::f4(p_cb->sc_oob_data.loc_oob_data.publ_key_used.x,
                         p_cb->sc_oob_data.loc_oob_data.publ_key_used.x,
                         p_cb->sc_oob_data.loc_oob_data.randomizer, 0);

#if (SMP_DEBUG == TRUE)
  uint8_t* p_print = NULL;
  SMP_TRACE_DEBUG("local SC OOB data set:");
  p_print = (uint8_t*)&p_cb->sc_oob_data.loc_oob_data.addr_sent_to;
  smp_debug_print_nbyte_little_endian(p_print, "addr_sent_to",
                                      sizeof(tBLE_BD_ADDR));
  p_print = (uint8_t*)&p_cb->sc_oob_data.loc_oob_data.private_key_used;
  smp_debug_print_nbyte_little_endian(p_print, "private_key_used",
                                      BT_OCTET32_LEN);
  p_print = (uint8_t*)&p_cb->sc_oob_data.loc_oob_data.publ_key_used.x;
  smp_debug_print_nbyte_little_endian(p_print, "publ_key_used.x",
                                      BT_OCTET32_LEN);
  p_print = (uint8_t*)&p_cb->sc_oob_data.loc_oob_data.publ_key_used.y;
  smp_debug_print_nbyte_little_endian(p_print, "publ_key_used.y",
                                      BT_OCTET32_LEN);
  p_print = (uint8_t*)&p_cb->sc_oob_data.loc_oob_data.randomizer;
  smp_debug_print_nbyte_little_endian(p_print, "randomizer", OCTET16_LEN);
  p_print = (uint8_t*)&p_cb->sc_oob_data.loc_oob_data.commitment;
  smp_debug_print_nbyte_little_endian(p_print, "commitment", OCTET16_LEN);
  SMP_TRACE_DEBUG("");
#endif

  /* pass created OOB data up */
  p_cb->cb_evt = SMP_SC_LOC_OOB_DATA_UP_EVT;
  smp_send_app_cback(p_cb, NULL);

  smp_cb_cleanup(p_cb);
}

/*******************************************************************************
 *
 * Function         smp_link_encrypted
 *
 * Description      This function is called when link is encrypted and notified
 *                  to the slave device. Proceed to to send LTK, DIV and ER to
 *                  master if bonding the devices.
 *
 *
 * Returns          void
 *
 ******************************************************************************/
void smp_link_encrypted(const RawAddress& bda, uint8_t encr_enable) {
  tSMP_CB* p_cb = &smp_cb;

  SMP_TRACE_DEBUG("%s: encr_enable=%d", __func__, encr_enable);

  if (smp_cb.pairing_bda == bda) {
    /* encryption completed with STK, remember the key size now, could be
     * overwritten when key exchange happens                                 */
    if (p_cb->loc_enc_size != 0 && encr_enable) {
      /* update the link encryption key size if a SMP pairing just performed */
      btm_ble_update_sec_key_size(bda, p_cb->loc_enc_size);
    }

    tSMP_INT_DATA smp_int_data;
    smp_int_data.status = encr_enable;
    smp_sm_event(&smp_cb, SMP_ENCRYPTED_EVT, &smp_int_data);
  }
}

void smp_cancel_start_encryption_attempt() {
  SMP_TRACE_ERROR("%s: Encryption request cancelled", __func__);
  smp_sm_event(&smp_cb, SMP_DISCARD_SEC_REQ_EVT, NULL);
}

/*******************************************************************************
 *
 * Function         smp_proc_ltk_request
 *
 * Description      This function is called when LTK request is received from
 *                  controller.
 *
 * Returns          void
 *
 ******************************************************************************/
bool smp_proc_ltk_request(const RawAddress& bda) {
  SMP_TRACE_DEBUG("%s state = %d", __func__, smp_cb.state);
  bool match = false;

  if (bda == smp_cb.pairing_bda) {
    match = true;
  } else {
    tBTM_SEC_DEV_REC* p_dev_rec = btm_find_dev(bda);
    if (p_dev_rec != NULL && p_dev_rec->ble.pseudo_addr == smp_cb.pairing_bda &&
        p_dev_rec->ble.pseudo_addr != RawAddress::kEmpty) {
      match = true;
    }
  }

  if (match && smp_cb.state == SMP_STATE_ENCRYPTION_PENDING) {
    smp_sm_event(&smp_cb, SMP_ENC_REQ_EVT, NULL);
    return true;
  }

  return false;
}

/*******************************************************************************
 *
 * Function         smp_process_secure_connection_long_term_key
 *
 * Description      This function is called to process SC LTK.
 *                  SC LTK is calculated and used instead of STK.
 *                  Here SC LTK is saved in BLE DB.
 *
 * Returns          void
 *
 ******************************************************************************/
void smp_process_secure_connection_long_term_key(void) {
  tSMP_CB* p_cb = &smp_cb;

  SMP_TRACE_DEBUG("%s", __func__);
  smp_save_secure_connections_long_term_key(p_cb);

  smp_update_key_mask(p_cb, SMP_SEC_KEY_TYPE_ENC, false);
  smp_key_distribution(p_cb, NULL);
}

/*******************************************************************************
 *
 * Function         smp_set_derive_link_key
 *
 * Description      This function is called to set flag that indicates that
 *                  BR/EDR LK has to be derived from LTK after all keys are
 *                  distributed.
 *
 * Returns          void
 *
 ******************************************************************************/
void smp_set_derive_link_key(tSMP_CB* p_cb, tSMP_INT_DATA* p_data) {
  SMP_TRACE_DEBUG("%s", __func__);
  p_cb->derive_lk = true;
  smp_update_key_mask(p_cb, SMP_SEC_KEY_TYPE_LK, false);
  smp_key_distribution(p_cb, NULL);
}

/*******************************************************************************
 *
 * Function         smp_derive_link_key_from_long_term_key
 *
 * Description      This function is called to derive BR/EDR LK from LTK.
 *
 * Returns          void
 *
 ******************************************************************************/
void smp_derive_link_key_from_long_term_key(tSMP_CB* p_cb,
                                            tSMP_INT_DATA* p_data) {
  tSMP_STATUS status = SMP_PAIR_FAIL_UNKNOWN;

  SMP_TRACE_DEBUG("%s", __func__);
  if (!smp_calculate_link_key_from_long_term_key(p_cb)) {
    SMP_TRACE_ERROR("%s failed", __func__);
    tSMP_INT_DATA smp_int_data;
    smp_int_data.status = status;
    smp_sm_event(p_cb, SMP_AUTH_CMPL_EVT, &smp_int_data);
    return;
  }
}

/*******************************************************************************
 *
 * Function         smp_br_process_link_key
 *
 * Description      This function is called to process BR/EDR LK:
 *                  - to derive SMP LTK from BR/EDR LK;
 *                  - to save SMP LTK.
 *
 * Returns          void
 *
 ******************************************************************************/
void smp_br_process_link_key(tSMP_CB* p_cb, tSMP_INT_DATA* p_data) {
  tSMP_STATUS status = SMP_PAIR_FAIL_UNKNOWN;

  SMP_TRACE_DEBUG("%s", __func__);
  if (!smp_calculate_long_term_key_from_link_key(p_cb)) {
    SMP_TRACE_ERROR("%s: failed", __func__);
    tSMP_INT_DATA smp_int_data;
    smp_int_data.status = status;
    smp_sm_event(p_cb, SMP_BR_AUTH_CMPL_EVT, &smp_int_data);
    return;
  }

  tBTM_SEC_DEV_REC* p_dev_rec = btm_find_dev(p_cb->pairing_bda);
  if (p_dev_rec) {
    SMP_TRACE_DEBUG("%s: dev_type = %d ", __func__, p_dev_rec->device_type);
    p_dev_rec->device_type |= BT_DEVICE_TYPE_BLE;
  } else {
    SMP_TRACE_ERROR("%s failed to find Security Record", __func__);
  }

  SMP_TRACE_DEBUG("%s: LTK derivation from LK successfully completed",
                  __func__);
  smp_save_secure_connections_long_term_key(p_cb);
  smp_update_key_mask(p_cb, SMP_SEC_KEY_TYPE_ENC, false);
  smp_br_select_next_key(p_cb, NULL);
}

/*******************************************************************************
 * Function     smp_key_distribution_by_transport
 * Description  depending on the transport used at the moment calls either
 *              smp_key_distribution(...) or smp_br_key_distribution(...).
 ******************************************************************************/
void smp_key_distribution_by_transport(tSMP_CB* p_cb, tSMP_INT_DATA* p_data) {
  SMP_TRACE_DEBUG("%s", __func__);
  if (p_cb->smp_over_br) {
    smp_br_select_next_key(p_cb, NULL);
  } else {
    smp_key_distribution(p_cb, NULL);
  }
}

/*******************************************************************************
 * Function         smp_br_pairing_complete
 * Description      This function is called to send the pairing complete
 *                  callback and remove the connection if needed.
 ******************************************************************************/
void smp_br_pairing_complete(tSMP_CB* p_cb, tSMP_INT_DATA* p_data) {
  SMP_TRACE_DEBUG("%s", __func__);

  if (p_cb->total_tx_unacked == 0) {
    /* process the pairing complete */
    smp_proc_pairing_cmpl(p_cb);
  }
}
