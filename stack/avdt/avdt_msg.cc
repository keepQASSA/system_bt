/******************************************************************************
 *
 *  Copyright 2002-2012 Broadcom Corporation
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
 *  This module contains functions for parsing and building AVDTP signaling
 *  messages.  It also contains functions called by the SCB or CCB state
 *  machines for sending command, response, and reject messages.  It also
 *  contains a function that processes incoming messages and dispatches them
 *  to the appropriate SCB or CCB.
 *
 ******************************************************************************/

#include <log/log.h>
#include <string.h>
#include "avdt_api.h"
#include "avdt_int.h"
#include "avdtc_api.h"
#include "bt_common.h"
#include "bt_target.h"
#include "bt_types.h"
#include "bt_utils.h"
#include "btu.h"
#include "osi/include/osi.h"

/*****************************************************************************
 * constants
 ****************************************************************************/

/* mask of all psc values */
#define AVDT_MSG_PSC_MASK                                                   \
  (AVDT_PSC_TRANS | AVDT_PSC_REPORT | AVDT_PSC_DELAY_RPT | AVDT_PSC_RECOV | \
   AVDT_PSC_HDRCMP | AVDT_PSC_MUX)
#define AVDT_PSC_PROTECT (1 << 4) /* Content Protection */
#define AVDT_PSC_CODEC (1 << 7)   /* codec */

/*****************************************************************************
 * type definitions
 ****************************************************************************/

/* type for message building functions */
typedef void (*tAVDT_MSG_BLD)(uint8_t** p, tAVDT_MSG* p_msg);

/* type for message parsing functions */
typedef uint8_t (*tAVDT_MSG_PRS)(tAVDT_MSG* p_msg, uint8_t* p, uint16_t len);

/*****************************************************************************
 * local function declarations
 ****************************************************************************/

static void avdt_msg_bld_none(uint8_t** p, tAVDT_MSG* p_msg);
static void avdt_msg_bld_single(uint8_t** p, tAVDT_MSG* p_msg);
static void avdt_msg_bld_setconfig_cmd(uint8_t** p, tAVDT_MSG* p_msg);
static void avdt_msg_bld_reconfig_cmd(uint8_t** p, tAVDT_MSG* p_msg);
static void avdt_msg_bld_multi(uint8_t** p, tAVDT_MSG* p_msg);
static void avdt_msg_bld_security_cmd(uint8_t** p, tAVDT_MSG* p_msg);
static void avdt_msg_bld_discover_rsp(uint8_t** p, tAVDT_MSG* p_msg);
static void avdt_msg_bld_svccap(uint8_t** p, tAVDT_MSG* p_msg);
static void avdt_msg_bld_security_rsp(uint8_t** p, tAVDT_MSG* p_msg);
static void avdt_msg_bld_all_svccap(uint8_t** p, tAVDT_MSG* p_msg);
static void avdt_msg_bld_delay_rpt(uint8_t** p, tAVDT_MSG* p_msg);

static uint8_t avdt_msg_prs_none(tAVDT_MSG* p_msg, uint8_t* p, uint16_t len);
static uint8_t avdt_msg_prs_single(tAVDT_MSG* p_msg, uint8_t* p, uint16_t len);
static uint8_t avdt_msg_prs_setconfig_cmd(tAVDT_MSG* p_msg, uint8_t* p,
                                          uint16_t len);
static uint8_t avdt_msg_prs_reconfig_cmd(tAVDT_MSG* p_msg, uint8_t* p,
                                         uint16_t len);
static uint8_t avdt_msg_prs_multi(tAVDT_MSG* p_msg, uint8_t* p, uint16_t len);
static uint8_t avdt_msg_prs_security_cmd(tAVDT_MSG* p_msg, uint8_t* p,
                                         uint16_t len);
static uint8_t avdt_msg_prs_discover_rsp(tAVDT_MSG* p_msg, uint8_t* p,
                                         uint16_t len);
static uint8_t avdt_msg_prs_svccap(tAVDT_MSG* p_msg, uint8_t* p, uint16_t len);
static uint8_t avdt_msg_prs_all_svccap(tAVDT_MSG* p_msg, uint8_t* p,
                                       uint16_t len);
static uint8_t avdt_msg_prs_security_rsp(tAVDT_MSG* p_msg, uint8_t* p,
                                         uint16_t len);
static uint8_t avdt_msg_prs_delay_rpt(tAVDT_MSG* p_msg, uint8_t* p,
                                      uint16_t len);

/*****************************************************************************
 * constants
 ****************************************************************************/

/* table of information element minimum lengths used for parsing */
const uint8_t avdt_msg_ie_len_min[] = {
    0,                     /* unused */
    AVDT_LEN_TRANS_MIN,    /* media transport */
    AVDT_LEN_REPORT_MIN,   /* reporting */
    AVDT_LEN_RECOV_MIN,    /* recovery */
    AVDT_LEN_PROTECT_MIN,  /* content protection */
    AVDT_LEN_HDRCMP_MIN,   /* header compression */
    AVDT_LEN_MUX_MIN,      /* multiplexing */
    AVDT_LEN_CODEC_MIN,    /* codec */
    AVDT_LEN_DELAY_RPT_MIN /* delay report */
};

/* table of information element minimum lengths used for parsing */
const uint8_t avdt_msg_ie_len_max[] = {
    0,                     /* unused */
    AVDT_LEN_TRANS_MAX,    /* media transport */
    AVDT_LEN_REPORT_MAX,   /* reporting */
    AVDT_LEN_RECOV_MAX,    /* recovery */
    AVDT_LEN_PROTECT_MAX,  /* content protection */
    AVDT_LEN_HDRCMP_MAX,   /* header compression */
    AVDT_LEN_MUX_MAX,      /* multiplexing */
    AVDT_LEN_CODEC_MAX,    /* codec */
    AVDT_LEN_DELAY_RPT_MAX /* delay report */
};

/* table of error codes used when decoding information elements */
const uint8_t avdt_msg_ie_err[] = {
    0,                    /* unused */
    AVDT_ERR_MEDIA_TRANS, /* media transport */
    AVDT_ERR_LENGTH,      /* reporting */
    AVDT_ERR_RECOV_FMT,   /* recovery */
    AVDT_ERR_CP_FMT,      /* content protection */
    AVDT_ERR_ROHC_FMT,    /* header compression */
    AVDT_ERR_MUX_FMT,     /* multiplexing */
    AVDT_ERR_SERVICE,     /* codec */
    AVDT_ERR_SERVICE      /* delay report ?? */
};

/* table of packet type minimum lengths */
static const uint8_t avdt_msg_pkt_type_len[] = {
    AVDT_LEN_TYPE_SINGLE, AVDT_LEN_TYPE_START, AVDT_LEN_TYPE_CONT,
    AVDT_LEN_TYPE_END};

/* function table for building command messages */
const tAVDT_MSG_BLD avdt_msg_bld_cmd[] = {
    avdt_msg_bld_none,          /* discover */
    avdt_msg_bld_single,        /* get capabilities */
    avdt_msg_bld_setconfig_cmd, /* set configuration */
    avdt_msg_bld_single,        /* get configuration */
    avdt_msg_bld_reconfig_cmd,  /* reconfigure */
    avdt_msg_bld_single,        /* open */
    avdt_msg_bld_multi,         /* start */
    avdt_msg_bld_single,        /* close */
    avdt_msg_bld_multi,         /* suspend */
    avdt_msg_bld_single,        /* abort */
    avdt_msg_bld_security_cmd,  /* security control */
    avdt_msg_bld_single,        /* get all capabilities */
    avdt_msg_bld_delay_rpt      /* delay report */
};

/* function table for building response messages */
const tAVDT_MSG_BLD avdt_msg_bld_rsp[] = {
    avdt_msg_bld_discover_rsp, /* discover */
    avdt_msg_bld_svccap,       /* get capabilities */
    avdt_msg_bld_none,         /* set configuration */
    avdt_msg_bld_all_svccap,   /* get configuration */
    avdt_msg_bld_none,         /* reconfigure */
    avdt_msg_bld_none,         /* open */
    avdt_msg_bld_none,         /* start */
    avdt_msg_bld_none,         /* close */
    avdt_msg_bld_none,         /* suspend */
    avdt_msg_bld_none,         /* abort */
    avdt_msg_bld_security_rsp, /* security control */
    avdt_msg_bld_all_svccap,   /* get all capabilities */
    avdt_msg_bld_none          /* delay report */
};

/* function table for parsing command messages */
const tAVDT_MSG_PRS avdt_msg_prs_cmd[] = {
    avdt_msg_prs_none,          /* discover */
    avdt_msg_prs_single,        /* get capabilities */
    avdt_msg_prs_setconfig_cmd, /* set configuration */
    avdt_msg_prs_single,        /* get configuration */
    avdt_msg_prs_reconfig_cmd,  /* reconfigure */
    avdt_msg_prs_single,        /* open */
    avdt_msg_prs_multi,         /* start */
    avdt_msg_prs_single,        /* close */
    avdt_msg_prs_multi,         /* suspend */
    avdt_msg_prs_single,        /* abort */
    avdt_msg_prs_security_cmd,  /* security control */
    avdt_msg_prs_single,        /* get all capabilities */
    avdt_msg_prs_delay_rpt      /* delay report */
};

/* function table for parsing response messages */
const tAVDT_MSG_PRS avdt_msg_prs_rsp[] = {
    avdt_msg_prs_discover_rsp, /* discover */
    avdt_msg_prs_svccap,       /* get capabilities */
    avdt_msg_prs_none,         /* set configuration */
    avdt_msg_prs_all_svccap,   /* get configuration */
    avdt_msg_prs_none,         /* reconfigure */
    avdt_msg_prs_none,         /* open */
    avdt_msg_prs_none,         /* start */
    avdt_msg_prs_none,         /* close */
    avdt_msg_prs_none,         /* suspend */
    avdt_msg_prs_none,         /* abort */
    avdt_msg_prs_security_rsp, /* security control */
    avdt_msg_prs_all_svccap,   /* get all capabilities */
    avdt_msg_prs_none          /* delay report */
};

/* command message-to-event lookup table */
const uint8_t avdt_msg_cmd_2_evt[] = {
    AVDT_CCB_MSG_DISCOVER_CMD_EVT + AVDT_CCB_MKR, /* discover */
    AVDT_CCB_MSG_GETCAP_CMD_EVT + AVDT_CCB_MKR,   /* get capabilities */
    AVDT_SCB_MSG_SETCONFIG_CMD_EVT,               /* set configuration */
    AVDT_SCB_MSG_GETCONFIG_CMD_EVT,               /* get configuration */
    AVDT_SCB_MSG_RECONFIG_CMD_EVT,                /* reconfigure */
    AVDT_SCB_MSG_OPEN_CMD_EVT,                    /* open */
    AVDT_CCB_MSG_START_CMD_EVT + AVDT_CCB_MKR,    /* start */
    AVDT_SCB_MSG_CLOSE_CMD_EVT,                   /* close */
    AVDT_CCB_MSG_SUSPEND_CMD_EVT + AVDT_CCB_MKR,  /* suspend */
    AVDT_SCB_MSG_ABORT_CMD_EVT,                   /* abort */
    AVDT_SCB_MSG_SECURITY_CMD_EVT,                /* security control */
    AVDT_CCB_MSG_GETCAP_CMD_EVT + AVDT_CCB_MKR,   /* get all capabilities */
    AVDT_SCB_MSG_DELAY_RPT_CMD_EVT                /* delay report */
};

/* response message-to-event lookup table */
const uint8_t avdt_msg_rsp_2_evt[] = {
    AVDT_CCB_MSG_DISCOVER_RSP_EVT + AVDT_CCB_MKR, /* discover */
    AVDT_CCB_MSG_GETCAP_RSP_EVT + AVDT_CCB_MKR,   /* get capabilities */
    AVDT_SCB_MSG_SETCONFIG_RSP_EVT,               /* set configuration */
    AVDT_SCB_MSG_GETCONFIG_RSP_EVT,               /* get configuration */
    AVDT_SCB_MSG_RECONFIG_RSP_EVT,                /* reconfigure */
    AVDT_SCB_MSG_OPEN_RSP_EVT,                    /* open */
    AVDT_CCB_MSG_START_RSP_EVT + AVDT_CCB_MKR,    /* start */
    AVDT_SCB_MSG_CLOSE_RSP_EVT,                   /* close */
    AVDT_CCB_MSG_SUSPEND_RSP_EVT + AVDT_CCB_MKR,  /* suspend */
    AVDT_SCB_MSG_ABORT_RSP_EVT,                   /* abort */
    AVDT_SCB_MSG_SECURITY_RSP_EVT,                /* security control */
    AVDT_CCB_MSG_GETCAP_RSP_EVT + AVDT_CCB_MKR,   /* get all capabilities */
    AVDT_SCB_MSG_DELAY_RPT_RSP_EVT                /* delay report */
};

/* reject message-to-event lookup table */
const uint8_t avdt_msg_rej_2_evt[] = {
    AVDT_CCB_MSG_DISCOVER_RSP_EVT + AVDT_CCB_MKR, /* discover */
    AVDT_CCB_MSG_GETCAP_RSP_EVT + AVDT_CCB_MKR,   /* get capabilities */
    AVDT_SCB_MSG_SETCONFIG_REJ_EVT,               /* set configuration */
    AVDT_SCB_MSG_GETCONFIG_RSP_EVT,               /* get configuration */
    AVDT_SCB_MSG_RECONFIG_RSP_EVT,                /* reconfigure */
    AVDT_SCB_MSG_OPEN_REJ_EVT,                    /* open */
    AVDT_CCB_MSG_START_RSP_EVT + AVDT_CCB_MKR,    /* start */
    AVDT_SCB_MSG_CLOSE_RSP_EVT,                   /* close */
    AVDT_CCB_MSG_SUSPEND_RSP_EVT + AVDT_CCB_MKR,  /* suspend */
    AVDT_SCB_MSG_ABORT_RSP_EVT,                   /* abort */
    AVDT_SCB_MSG_SECURITY_RSP_EVT,                /* security control */
    AVDT_CCB_MSG_GETCAP_RSP_EVT + AVDT_CCB_MKR,   /* get all capabilities */
    0                                             /* delay report */
};

/*******************************************************************************
 *
 * Function         avdt_msg_bld_cfg
 *
 * Description      This function builds the configuration parameters contained
 *                  in a command or response message.
 *
 *
 * Returns          void.
 *
 ******************************************************************************/
static void avdt_msg_bld_cfg(uint8_t** p, AvdtpSepConfig* p_cfg) {
  uint8_t len;

  /* for now, just build media transport, codec, and content protection, and
   * multiplexing */

  /* media transport */
  if (p_cfg->psc_mask & AVDT_PSC_TRANS) {
    *(*p)++ = AVDT_CAT_TRANS;
    *(*p)++ = 0; /* length */
  }

  /* reporting transport */
  if (p_cfg->psc_mask & AVDT_PSC_REPORT) {
    *(*p)++ = AVDT_CAT_REPORT;
    *(*p)++ = 0; /* length */
  }

  /* codec */
  if (p_cfg->num_codec != 0) {
    *(*p)++ = AVDT_CAT_CODEC;
    len = p_cfg->codec_info[0] + 1;
    if (len > AVDT_CODEC_SIZE) len = AVDT_CODEC_SIZE;

    memcpy(*p, p_cfg->codec_info, len);
    *p += len;
  }

  /* content protection */
  if (p_cfg->num_protect != 0) {
    *(*p)++ = AVDT_CAT_PROTECT;
    len = p_cfg->protect_info[0] + 1;
    if (len > AVDT_PROTECT_SIZE) len = AVDT_PROTECT_SIZE;

    memcpy(*p, p_cfg->protect_info, len);
    *p += len;
  }

  /* delay report */
  if (p_cfg->psc_mask & AVDT_PSC_DELAY_RPT) {
    *(*p)++ = AVDT_CAT_DELAY_RPT;
    *(*p)++ = 0; /* length */
  }
}

/*******************************************************************************
 *
 * Function         avdt_msg_bld_none
 *
 * Description      This message building function builds an empty message.
 *
 *
 * Returns          void.
 *
 ******************************************************************************/
static void avdt_msg_bld_none(UNUSED_ATTR uint8_t** p,
                              UNUSED_ATTR tAVDT_MSG* p_msg) {
  return;
}

/*******************************************************************************
 *
 * Function         avdt_msg_bld_single
 *
 * Description      This message building function builds a message containing
 *                  a single SEID.
 *
 *
 * Returns          void.
 *
 ******************************************************************************/
static void avdt_msg_bld_single(uint8_t** p, tAVDT_MSG* p_msg) {
  AVDT_MSG_BLD_SEID(*p, p_msg->single.seid);
}

/*******************************************************************************
 *
 * Function         avdt_msg_bld_setconfig_cmd
 *
 * Description      This message building function builds a set configuration
 *                  command message.
 *
 *
 * Returns          void.
 *
 ******************************************************************************/
static void avdt_msg_bld_setconfig_cmd(uint8_t** p, tAVDT_MSG* p_msg) {
  AVDT_MSG_BLD_SEID(*p, p_msg->config_cmd.hdr.seid);
  AVDT_MSG_BLD_SEID(*p, p_msg->config_cmd.int_seid);
  avdt_msg_bld_cfg(p, p_msg->config_cmd.p_cfg);
}

/*******************************************************************************
 *
 * Function         avdt_msg_bld_reconfig_cmd
 *
 * Description      This message building function builds a reconfiguration
 *                  command message.
 *
 *
 * Returns          void.
 *
 ******************************************************************************/
static void avdt_msg_bld_reconfig_cmd(uint8_t** p, tAVDT_MSG* p_msg) {
  AVDT_MSG_BLD_SEID(*p, p_msg->reconfig_cmd.hdr.seid);

  /* force psc mask zero to build only codec and security */
  p_msg->reconfig_cmd.p_cfg->psc_mask = 0;
  avdt_msg_bld_cfg(p, p_msg->reconfig_cmd.p_cfg);
}

/*******************************************************************************
 *
 * Function         avdt_msg_bld_multi
 *
 * Description      This message building function builds a message containing
 *                  multiple SEID's.
 *
 *
 * Returns          void.
 *
 ******************************************************************************/
static void avdt_msg_bld_multi(uint8_t** p, tAVDT_MSG* p_msg) {
  int i;

  for (i = 0; i < p_msg->multi.num_seps; i++) {
    AVDT_MSG_BLD_SEID(*p, p_msg->multi.seid_list[i]);
  }
}

/*******************************************************************************
 *
 * Function         avdt_msg_bld_security_cmd
 *
 * Description      This message building function builds a security
 *                  command message.
 *
 * Returns          void.
 *
 ******************************************************************************/
static void avdt_msg_bld_security_cmd(uint8_t** p, tAVDT_MSG* p_msg) {
  AVDT_MSG_BLD_SEID(*p, p_msg->security_cmd.hdr.seid);
  memcpy(*p, p_msg->security_cmd.p_data, p_msg->security_cmd.len);
  *p += p_msg->security_cmd.len;
}

/*******************************************************************************
 *
 * Function         avdt_msg_bld_delay_rpt
 *
 * Description      This message building function builds a delay report
 *                  command message.
 *
 * Returns          void.
 *
 ******************************************************************************/
static void avdt_msg_bld_delay_rpt(uint8_t** p, tAVDT_MSG* p_msg) {
  AVDT_MSG_BLD_SEID(*p, p_msg->delay_rpt_cmd.hdr.seid);
  UINT16_TO_BE_STREAM(*p, p_msg->delay_rpt_cmd.delay);
}

/*******************************************************************************
 *
 * Function         avdt_msg_bld_discover_rsp
 *
 * Description      This message building function builds a discover
 *                  response message.
 *
 *
 * Returns          void.
 *
 ******************************************************************************/
static void avdt_msg_bld_discover_rsp(uint8_t** p, tAVDT_MSG* p_msg) {
  int i;

  for (i = 0; i < p_msg->discover_rsp.num_seps; i++) {
    /* build discover rsp info */
    AVDT_MSG_BLD_DISC(*p, p_msg->discover_rsp.p_sep_info[i].seid,
                      p_msg->discover_rsp.p_sep_info[i].in_use,
                      p_msg->discover_rsp.p_sep_info[i].media_type,
                      p_msg->discover_rsp.p_sep_info[i].tsep);
  }
}

/*******************************************************************************
 *
 * Function         avdt_msg_bld_svccap
 *
 * Description      This message building function builds a message containing
 *                  service capabilities parameters.
 *
 *
 * Returns          void.
 *
 ******************************************************************************/
static void avdt_msg_bld_svccap(uint8_t** p, tAVDT_MSG* p_msg) {
  AvdtpSepConfig cfg = *p_msg->svccap.p_cfg;

  // Include only the Basic Capability
  cfg.psc_mask &= AVDT_LEG_PSC;

  avdt_msg_bld_cfg(p, &cfg);
}

/*******************************************************************************
 *
 * Function         avdt_msg_bld_all_svccap
 *
 * Description      This message building function builds a message containing
 *                  service capabilities parameters.
 *
 *
 * Returns          void.
 *
 ******************************************************************************/
static void avdt_msg_bld_all_svccap(uint8_t** p, tAVDT_MSG* p_msg) {
  avdt_msg_bld_cfg(p, p_msg->svccap.p_cfg);
}

/*******************************************************************************
 *
 * Function         avdt_msg_bld_security_rsp
 *
 * Description      This message building function builds a security
 *                  response message.
 *
 *
 * Returns          void.
 *
 ******************************************************************************/
static void avdt_msg_bld_security_rsp(uint8_t** p, tAVDT_MSG* p_msg) {
  memcpy(*p, p_msg->security_rsp.p_data, p_msg->security_rsp.len);
  *p += p_msg->security_rsp.len;
}

/*******************************************************************************
 *
 * Function         avdt_msg_prs_cfg
 *
 * Description      This message parsing function parses the configuration
 *                  parameters field of a message.
 *
 *
 * Returns          Error code or zero if no error, and element that failed
 *                  in p_elem.
 *
 ******************************************************************************/
static uint8_t avdt_msg_prs_cfg(AvdtpSepConfig* p_cfg, uint8_t* p, uint16_t len,
                                uint8_t* p_elem, uint8_t sig_id) {
  uint8_t* p_end;
  uint8_t elem = 0;
  uint8_t elem_len;
  uint8_t tmp;
  uint8_t err = 0;
  uint8_t protect_offset = 0;

  if (!p_cfg) {
    AVDT_TRACE_ERROR("not expecting this cfg");
    return AVDT_ERR_BAD_STATE;
  }

  p_cfg->psc_mask = 0;
  p_cfg->num_codec = 0;
  p_cfg->num_protect = 0;

  /* while there is still data to parse */
  p_end = p + len;
  while ((p < p_end) && (err == 0)) {
    /* verify overall length */
    if ((p_end - p) < AVDT_LEN_CFG_MIN) {
      err = AVDT_ERR_PAYLOAD;
      break;
    }

    /* get and verify info elem id, length */
    elem = *p++;
    elem_len = *p++;

    if ((elem == 0) || (elem > AVDT_CAT_MAX_CUR)) {
      /* this may not be really bad.
       * It may be a service category that is too new for us.
       * allow these to be parsed without reporting an error.
       * If this is a "capability" (as in GetCapRsp & GetConfigRsp), this is
       * filtered out.
       * If this is a Configuration (as in SetConfigCmd & ReconfigCmd),
       *    this will be marked as an error in the caller of this function */
      if ((sig_id == AVDT_SIG_SETCONFIG) || (sig_id == AVDT_SIG_RECONFIG)) {
        /* Cannot accept unknown category. */
        err = AVDT_ERR_CATEGORY;
        break;
      } else /* GETCAP or GET_ALLCAP */
      {
        /* Skip unknown categories. */
        p += elem_len;
        AVDT_TRACE_DEBUG("skipping unknown service category=%d len: %d", elem,
                         elem_len);
        continue;
      }
    }

    if ((elem_len > avdt_msg_ie_len_max[elem]) ||
        (elem_len < avdt_msg_ie_len_min[elem])) {
      err = avdt_msg_ie_err[elem];
      break;
    }

    /* add element to psc mask, but mask out codec or protect */
    p_cfg->psc_mask |= (1 << elem);
    AVDT_TRACE_DEBUG("elem=%d elem_len: %d psc_mask=0x%x", elem, elem_len,
                     p_cfg->psc_mask);

    /* parse individual information elements with additional parameters */
    switch (elem) {
      case AVDT_CAT_RECOV:
        if ((p_end - p) < 3) {
          err = AVDT_ERR_PAYLOAD;
          break;
        }
        p_cfg->recov_type = *p++;
        p_cfg->recov_mrws = *p++;
        p_cfg->recov_mnmp = *p++;
        if (p_cfg->recov_type != AVDT_RECOV_RFC2733) {
          err = AVDT_ERR_RECOV_TYPE;
        } else if ((p_cfg->recov_mrws < AVDT_RECOV_MRWS_MIN) ||
                   (p_cfg->recov_mrws > AVDT_RECOV_MRWS_MAX) ||
                   (p_cfg->recov_mnmp < AVDT_RECOV_MNMP_MIN) ||
                   (p_cfg->recov_mnmp > AVDT_RECOV_MNMP_MAX)) {
          err = AVDT_ERR_RECOV_FMT;
        }
        break;

      case AVDT_CAT_PROTECT:
        p_cfg->psc_mask &= ~AVDT_PSC_PROTECT;
        if (p + elem_len > p_end) {
          err = AVDT_ERR_LENGTH;
          android_errorWriteLog(0x534e4554, "78288378");
          break;
        }
        if ((elem_len + protect_offset) < AVDT_PROTECT_SIZE) {
          p_cfg->num_protect++;
          p_cfg->protect_info[protect_offset] = elem_len;
          protect_offset++;
          memcpy(&p_cfg->protect_info[protect_offset], p, elem_len);
          protect_offset += elem_len;
        }
        p += elem_len;
        break;

      case AVDT_CAT_HDRCMP:
        if ((p_end - p) < 1) {
          err = AVDT_ERR_PAYLOAD;
          break;
        }
        p_cfg->hdrcmp_mask = *p++;
        break;

      case AVDT_CAT_CODEC:
        p_cfg->psc_mask &= ~AVDT_PSC_CODEC;
        tmp = elem_len;
        if (elem_len >= AVDT_CODEC_SIZE) {
          tmp = AVDT_CODEC_SIZE - 1;
        }
        if (p + tmp > p_end) {
          err = AVDT_ERR_LENGTH;
          android_errorWriteLog(0x534e4554, "78288378");
          break;
        }
        p_cfg->num_codec++;
        p_cfg->codec_info[0] = elem_len;
        memcpy(&p_cfg->codec_info[1], p, tmp);
        p += elem_len;
        break;

      case AVDT_CAT_DELAY_RPT:
        AVDT_TRACE_DEBUG("%s: Remote device supports delay reporting",
                         __func__);
        break;

      default:
        p += elem_len;
        break;
    } /* switch */
  }   /* while ! err, !end*/
  *p_elem = elem;
  AVDT_TRACE_DEBUG("err=0x%x, elem:0x%x psc_mask=0x%x", err, elem,
                   p_cfg->psc_mask);

  return err;
}

/*******************************************************************************
 *
 * Function         avdt_msg_prs_none
 *
 * Description      This message parsing function parses a message with no
 *                  parameters.
 *
 *
 * Returns          Error code or zero if no error.
 *
 ******************************************************************************/
static uint8_t avdt_msg_prs_none(UNUSED_ATTR tAVDT_MSG* p_msg,
                                 UNUSED_ATTR uint8_t* p,
                                 UNUSED_ATTR uint16_t len) {
  return 0;
}

/*******************************************************************************
 *
 * Function         avdt_msg_prs_single
 *
 * Description      This message parsing function parses a message with a
 *                  single SEID.
 *
 *
 * Returns          Error code or zero if no error.
 *
 ******************************************************************************/
static uint8_t avdt_msg_prs_single(tAVDT_MSG* p_msg, uint8_t* p, uint16_t len) {
  uint8_t err = 0;

  /* verify len */
  if (len != AVDT_LEN_SINGLE) {
    err = AVDT_ERR_LENGTH;
  } else {
    AVDT_MSG_PRS_SEID(p, p_msg->single.seid);

    if (avdt_scb_by_hdl(p_msg->single.seid) == NULL) {
      err = AVDT_ERR_SEID;
    }
  }
  return err;
}

/*******************************************************************************
 *
 * Function         avdt_msg_prs_setconfig_cmd
 *
 * Description      This message parsing function parses a set configuration
 *                  command message.
 *
 *
 * Returns          Error code or zero if no error.
 *
 ******************************************************************************/
static uint8_t avdt_msg_prs_setconfig_cmd(tAVDT_MSG* p_msg, uint8_t* p,
                                          uint16_t len) {
  uint8_t err = 0;

  p_msg->hdr.err_param = 0;

  /* verify len */
  if (len < AVDT_LEN_SETCONFIG_MIN) {
    err = AVDT_ERR_LENGTH;
  } else {
    /* get seids */
    AVDT_MSG_PRS_SEID(p, p_msg->config_cmd.hdr.seid);
    if (avdt_scb_by_hdl(p_msg->config_cmd.hdr.seid) == NULL) {
      err = AVDT_ERR_SEID;
    }

    AVDT_MSG_PRS_SEID(p, p_msg->config_cmd.int_seid);
    if ((p_msg->config_cmd.int_seid < AVDT_SEID_MIN) ||
        (p_msg->config_cmd.int_seid > AVDT_SEID_MAX)) {
      err = AVDT_ERR_SEID;
    }
  }

  if (!err) {
    /* parse configuration parameters */
    len -= 2;
    err = avdt_msg_prs_cfg(p_msg->config_cmd.p_cfg, p, len,
                           &p_msg->hdr.err_param, AVDT_SIG_SETCONFIG);

    if (!err) {
      /* verify protocol service capabilities are supported */
      if (((p_msg->config_cmd.p_cfg->psc_mask & (~AVDT_PSC)) != 0) ||
          (p_msg->config_cmd.p_cfg->num_codec == 0)) {
        err = AVDT_ERR_INVALID_CAP;
      }
    }
  }

  return err;
}

/*******************************************************************************
 *
 * Function         avdt_msg_prs_reconfig_cmd
 *
 * Description      This message parsing function parses a reconfiguration
 *                  command message.
 *
 *
 * Returns          Error code or zero if no error.
 *
 ******************************************************************************/
static uint8_t avdt_msg_prs_reconfig_cmd(tAVDT_MSG* p_msg, uint8_t* p,
                                         uint16_t len) {
  uint8_t err = 0;

  p_msg->hdr.err_param = 0;

  /* verify len */
  if (len < AVDT_LEN_RECONFIG_MIN) {
    err = AVDT_ERR_LENGTH;
  } else {
    /* get seid */
    AVDT_MSG_PRS_SEID(p, p_msg->reconfig_cmd.hdr.seid);
    if (avdt_scb_by_hdl(p_msg->reconfig_cmd.hdr.seid) == NULL) {
      err = AVDT_ERR_SEID;
    } else {
      /* parse config parameters */
      len--;
      err = avdt_msg_prs_cfg(p_msg->config_cmd.p_cfg, p, len,
                             &p_msg->hdr.err_param, AVDT_SIG_RECONFIG);

      /* verify no protocol service capabilities in parameters */
      if (!err) {
        AVDT_TRACE_DEBUG("avdt_msg_prs_reconfig_cmd psc_mask=0x%x/0x%x",
                         p_msg->config_cmd.p_cfg->psc_mask, AVDT_MSG_PSC_MASK);
        if ((p_msg->config_cmd.p_cfg->psc_mask != 0) ||
            (p_msg->config_cmd.p_cfg->num_codec == 0 &&
             p_msg->config_cmd.p_cfg->num_protect == 0)) {
          err = AVDT_ERR_INVALID_CAP;
        }
      }
    }
  }
  return err;
}

/*******************************************************************************
 *
 * Function         avdt_msg_prs_multi
 *
 * Description      This message parsing function parses a message containing
 *                  multiple SEID's.
 *
 *
 * Returns          Error code or zero if no error.
 *
 ******************************************************************************/
static uint8_t avdt_msg_prs_multi(tAVDT_MSG* p_msg, uint8_t* p, uint16_t len) {
  int i;
  uint8_t err = 0;

  p_msg->hdr.err_param = 0;

  /* verify len */
  if (len < AVDT_LEN_MULTI_MIN || (len > AVDT_NUM_SEPS)) {
    err = AVDT_ERR_LENGTH;
  } else {
    /* get and verify all seps */
    for (i = 0; i < len; i++) {
      AVDT_MSG_PRS_SEID(p, p_msg->multi.seid_list[i]);
      if (avdt_scb_by_hdl(p_msg->multi.seid_list[i]) == NULL) {
        err = AVDT_ERR_SEID;
        p_msg->hdr.err_param = p_msg->multi.seid_list[i];
        break;
      }
    }
    p_msg->multi.num_seps = (uint8_t)i;
  }

  return err;
}

/*******************************************************************************
 *
 * Function         avdt_msg_prs_security_cmd
 *
 * Description      This message parsing function parses a security
 *                  command message.
 *
 *
 * Returns          Error code or zero if no error.
 *
 ******************************************************************************/
static uint8_t avdt_msg_prs_security_cmd(tAVDT_MSG* p_msg, uint8_t* p,
                                         uint16_t len) {
  uint8_t err = 0;

  /* verify len */
  if (len < AVDT_LEN_SECURITY_MIN) {
    err = AVDT_ERR_LENGTH;
  } else {
    /* get seid */
    AVDT_MSG_PRS_SEID(p, p_msg->security_cmd.hdr.seid);
    if (avdt_scb_by_hdl(p_msg->security_cmd.hdr.seid) == NULL) {
      err = AVDT_ERR_SEID;
    } else {
      p_msg->security_cmd.p_data = p;
      p_msg->security_cmd.len = len - 1;
    }
  }
  return err;
}

/*******************************************************************************
 *
 * Function         avdt_msg_prs_discover_rsp
 *
 * Description      This message parsing function parses a discover
 *                  response message.
 *
 *
 * Returns          Error code or zero if no error.
 *
 ******************************************************************************/
static uint8_t avdt_msg_prs_discover_rsp(tAVDT_MSG* p_msg, uint8_t* p,
                                         uint16_t len) {
  int i;
  uint8_t err = 0;

  /* determine number of seps; seps in msg is len/2, but set to minimum
  ** of seps app has supplied memory for and seps in msg
  */
  if (p_msg->discover_rsp.num_seps > (len / 2)) {
    p_msg->discover_rsp.num_seps = (len / 2);
  }

  /* parse out sep info */
  for (i = 0; i < p_msg->discover_rsp.num_seps; i++) {
    /* parse discover rsp info */
    AVDT_MSG_PRS_DISC(p, p_msg->discover_rsp.p_sep_info[i].seid,
                      p_msg->discover_rsp.p_sep_info[i].in_use,
                      p_msg->discover_rsp.p_sep_info[i].media_type,
                      p_msg->discover_rsp.p_sep_info[i].tsep);

    /* verify that seid is valid */
    if ((p_msg->discover_rsp.p_sep_info[i].seid < AVDT_SEID_MIN) ||
        (p_msg->discover_rsp.p_sep_info[i].seid > AVDT_SEID_MAX)) {
      err = AVDT_ERR_SEID;
      break;
    }
  }

  return err;
}

/*******************************************************************************
 *
 * Function         avdt_msg_prs_svccap
 *
 * Description      This message parsing function parses a message containing
 *                  service capabilities parameters.
 *
 *
 * Returns          Error code or zero if no error.
 *
 ******************************************************************************/
static uint8_t avdt_msg_prs_svccap(tAVDT_MSG* p_msg, uint8_t* p, uint16_t len) {
  /* parse parameters */
  uint8_t err = avdt_msg_prs_cfg(p_msg->svccap.p_cfg, p, len,
                                 &p_msg->hdr.err_param, AVDT_SIG_GETCAP);
  if (p_msg->svccap.p_cfg) {
    p_msg->svccap.p_cfg->psc_mask &= AVDT_LEG_PSC;
  }

  return (err);
}

/*******************************************************************************
 *
 * Function         avdt_msg_prs_all_svccap
 *
 * Description      This message parsing function parses a message containing
 *                  service capabilities parameters.
 *
 *
 * Returns          Error code or zero if no error.
 *
 ******************************************************************************/
static uint8_t avdt_msg_prs_all_svccap(tAVDT_MSG* p_msg, uint8_t* p,
                                       uint16_t len) {
  uint8_t err = avdt_msg_prs_cfg(p_msg->svccap.p_cfg, p, len,
                                 &p_msg->hdr.err_param, AVDT_SIG_GET_ALLCAP);
  if (p_msg->svccap.p_cfg) {
    p_msg->svccap.p_cfg->psc_mask &= AVDT_MSG_PSC_MASK;
  }
  return (err);
}

/*******************************************************************************
 *
 * Function         avdt_msg_prs_security_rsp
 *
 * Description      This message parsing function parsing a security
 *                  response message.
 *
 *
 * Returns          Error code or zero if no error.
 *
 ******************************************************************************/
static uint8_t avdt_msg_prs_security_rsp(tAVDT_MSG* p_msg, uint8_t* p,
                                         uint16_t len) {
  p_msg->security_rsp.p_data = p;
  p_msg->security_rsp.len = len;

  return 0;
}

/*******************************************************************************
 *
 * Function         avdt_msg_prs_rej
 *
 * Description
 *
 *
 * Returns          Error code or zero if no error.
 *
 ******************************************************************************/
static uint8_t avdt_msg_prs_rej(tAVDT_MSG* p_msg, uint8_t* p, uint16_t len,
                                uint8_t sig) {
  uint8_t error = 0;

  if (len > 0) {
    if ((sig == AVDT_SIG_SETCONFIG) || (sig == AVDT_SIG_RECONFIG)) {
      p_msg->hdr.err_param = *p++;
      len--;
    } else if ((sig == AVDT_SIG_START) || (sig == AVDT_SIG_SUSPEND)) {
      AVDT_MSG_PRS_SEID(p, p_msg->hdr.err_param);
      len--;
    }
  }

  if (len < 1) {
    char error_info[] = "AVDT rejected response length mismatch";
    android_errorWriteWithInfoLog(0x534e4554, "79702484", -1, error_info,
                                  strlen(error_info));
    error = AVDT_ERR_LENGTH;
  } else {
    p_msg->hdr.err_code = *p;
  }

  return error;
}

/*******************************************************************************
 *
 * Function         avdt_msg_prs_delay_rpt
 *
 * Description      This message parsing function parses a security
 *                  command message.
 *
 *
 * Returns          Error code or zero if no error.
 *
 ******************************************************************************/
static uint8_t avdt_msg_prs_delay_rpt(tAVDT_MSG* p_msg, uint8_t* p,
                                      uint16_t len) {
  uint8_t err = 0;

  /* verify len */
  if (len != AVDT_LEN_DELAY_RPT) {
    AVDT_TRACE_WARNING("avdt_msg_prs_delay_rpt expected len: %u  got: %u",
                       AVDT_LEN_DELAY_RPT, len);
    err = AVDT_ERR_LENGTH;
  } else {
    /* get seid */
    AVDT_MSG_PRS_SEID(p, p_msg->delay_rpt_cmd.hdr.seid);

    if (avdt_scb_by_hdl(p_msg->delay_rpt_cmd.hdr.seid) == NULL) {
      err = AVDT_ERR_SEID;
    } else {
      BE_STREAM_TO_UINT16(p_msg->delay_rpt_cmd.delay, p);
      AVDT_TRACE_DEBUG("avdt_msg_prs_delay_rpt delay: %u",
                       p_msg->delay_rpt_cmd.delay);
    }
  }
  return err;
}

/*******************************************************************************
 *
 * Function         avdt_msg_send
 *
 * Description      Send, and if necessary fragment the next message.
 *
 *
 * Returns          Congested state; true if CCB congested, false if not.
 *
 ******************************************************************************/
bool avdt_msg_send(AvdtpCcb* p_ccb, BT_HDR* p_msg) {
  uint16_t curr_msg_len;
  uint8_t pkt_type;
  uint8_t hdr_len;
  AvdtpTransportChannel* p_tbl;
  BT_HDR* p_buf;
  uint8_t* p;
  uint8_t label;
  uint8_t msg;
  uint8_t sig;
  uint8_t nosp = 0; /* number of subsequent packets */

  /* look up transport channel table entry to get peer mtu */
  p_tbl = avdt_ad_tc_tbl_by_type(AVDT_CHAN_SIG, p_ccb, NULL);

  /* set the current message if there is a message passed in */
  if (p_msg != NULL) {
    p_ccb->p_curr_msg = p_msg;
  }

  /* store copy of curr_msg->len */
  curr_msg_len = p_ccb->p_curr_msg->len;

  /* while not congested and we haven't sent it all */
  while ((!p_ccb->cong) && (p_ccb->p_curr_msg != NULL)) {
    /* check what kind of message we've got here; we are using the offset
    ** to indicate that a message is being fragmented
    */

    /* if message isn't being fragmented and it fits in mtu */
    if ((p_ccb->p_curr_msg->offset == AVDT_MSG_OFFSET) &&
        (p_ccb->p_curr_msg->len <= p_tbl->peer_mtu - AVDT_LEN_TYPE_SINGLE)) {
      pkt_type = AVDT_PKT_TYPE_SINGLE;
      hdr_len = AVDT_LEN_TYPE_SINGLE;
      p_buf = p_ccb->p_curr_msg;
    }
    /* if message isn't being fragmented and it doesn't fit in mtu */
    else if ((p_ccb->p_curr_msg->offset == AVDT_MSG_OFFSET) &&
             (p_ccb->p_curr_msg->len >
              p_tbl->peer_mtu - AVDT_LEN_TYPE_SINGLE)) {
      pkt_type = AVDT_PKT_TYPE_START;
      hdr_len = AVDT_LEN_TYPE_START;
      nosp = (p_ccb->p_curr_msg->len + AVDT_LEN_TYPE_START - p_tbl->peer_mtu) /
                 (p_tbl->peer_mtu - 1) +
             2;

      /* get a new buffer for fragment we are sending */
      p_buf = (BT_HDR*)osi_malloc(AVDT_CMD_BUF_SIZE);

      /* copy portion of data from current message to new buffer */
      p_buf->offset = L2CAP_MIN_OFFSET + hdr_len;
      p_buf->len = p_tbl->peer_mtu - hdr_len;
      memcpy((uint8_t*)(p_buf + 1) + p_buf->offset,
             (uint8_t*)(p_ccb->p_curr_msg + 1) + p_ccb->p_curr_msg->offset,
             p_buf->len);
    }
    /* if message is being fragmented and remaining bytes don't fit in mtu */
    else if ((p_ccb->p_curr_msg->offset > AVDT_MSG_OFFSET) &&
             (p_ccb->p_curr_msg->len >
              (p_tbl->peer_mtu - AVDT_LEN_TYPE_CONT))) {
      pkt_type = AVDT_PKT_TYPE_CONT;
      hdr_len = AVDT_LEN_TYPE_CONT;

      /* get a new buffer for fragment we are sending */
      p_buf = (BT_HDR*)osi_malloc(AVDT_CMD_BUF_SIZE);

      /* copy portion of data from current message to new buffer */
      p_buf->offset = L2CAP_MIN_OFFSET + hdr_len;
      p_buf->len = p_tbl->peer_mtu - hdr_len;
      memcpy((uint8_t*)(p_buf + 1) + p_buf->offset,
             (uint8_t*)(p_ccb->p_curr_msg + 1) + p_ccb->p_curr_msg->offset,
             p_buf->len);
    }
    /* if message is being fragmented and remaining bytes do fit in mtu */
    else {
      pkt_type = AVDT_PKT_TYPE_END;
      hdr_len = AVDT_LEN_TYPE_END;
      p_buf = p_ccb->p_curr_msg;
    }

    /* label, sig id, msg type are in hdr of p_curr_msg */
    label = AVDT_LAYERSPEC_LABEL(p_ccb->p_curr_msg->layer_specific);
    msg = AVDT_LAYERSPEC_MSG(p_ccb->p_curr_msg->layer_specific);
    sig = (uint8_t)p_ccb->p_curr_msg->event;
    AVDT_TRACE_DEBUG("avdt_msg_send label:%d, msg:%d, sig:%d", label, msg, sig);

    /* keep track of how much of msg we've sent */
    curr_msg_len -= p_buf->len;
    if (curr_msg_len == 0) {
      /* entire message sent; mark as finished */
      p_ccb->p_curr_msg = NULL;

      /* start timer here for commands */
      if (msg == AVDT_MSG_TYPE_CMD) {
        /* if retransmit timeout set to zero, sig doesn't use retransmit */
        if ((sig == AVDT_SIG_DISCOVER) || (sig == AVDT_SIG_GETCAP) ||
            (sig == AVDT_SIG_SECURITY) || (avdtp_cb.rcb.ret_tout == 0)) {
          alarm_cancel(p_ccb->idle_ccb_timer);
          alarm_cancel(p_ccb->ret_ccb_timer);
          uint64_t interval_ms = avdtp_cb.rcb.sig_tout * 1000;
          alarm_set_on_mloop(p_ccb->rsp_ccb_timer, interval_ms,
                             avdt_ccb_rsp_ccb_timer_timeout, p_ccb);
        } else if (sig != AVDT_SIG_DELAY_RPT) {
          alarm_cancel(p_ccb->idle_ccb_timer);
          alarm_cancel(p_ccb->rsp_ccb_timer);
          uint64_t interval_ms = avdtp_cb.rcb.ret_tout * 1000;
          alarm_set_on_mloop(p_ccb->ret_ccb_timer, interval_ms,
                             avdt_ccb_ret_ccb_timer_timeout, p_ccb);
        }
      }
    } else {
      /* message being fragmented and not completely sent */
      p_ccb->p_curr_msg->len -= p_buf->len;
      p_ccb->p_curr_msg->offset += p_buf->len;
    }

    /* set up to build header */
    p_buf->len += hdr_len;
    p_buf->offset -= hdr_len;
    p = (uint8_t*)(p_buf + 1) + p_buf->offset;

    /* build header */
    AVDT_MSG_BLD_HDR(p, label, pkt_type, msg);
    if (pkt_type == AVDT_PKT_TYPE_START) {
      AVDT_MSG_BLD_NOSP(p, nosp);
    }
    if ((pkt_type == AVDT_PKT_TYPE_START) ||
        (pkt_type == AVDT_PKT_TYPE_SINGLE)) {
      AVDT_MSG_BLD_SIG(p, sig);
    }

    /* send msg buffer down */
    avdt_ad_write_req(AVDT_CHAN_SIG, p_ccb, NULL, p_buf);
  }
  return (p_ccb->cong);
}

/*******************************************************************************
 *
 * Function         avdt_msg_asmbl
 *
 * Description      Reassemble incoming message.
 *
 *
 * Returns          Pointer to reassembled message;  NULL if no message
 *                  available.
 *
 ******************************************************************************/
BT_HDR* avdt_msg_asmbl(AvdtpCcb* p_ccb, BT_HDR* p_buf) {
  uint8_t* p;
  uint8_t pkt_type;
  BT_HDR* p_ret;

  /* parse the message header */
  p = (uint8_t*)(p_buf + 1) + p_buf->offset;

  /* Check if is valid length */
  if (p_buf->len < 1) {
    android_errorWriteLog(0x534e4554, "78287084");
    osi_free(p_buf);
    p_ret = NULL;
    return p_ret;
  }
  AVDT_MSG_PRS_PKT_TYPE(p, pkt_type);

  /* quick sanity check on length */
  if (p_buf->len < avdt_msg_pkt_type_len[pkt_type]) {
    osi_free(p_buf);
    AVDT_TRACE_WARNING("Bad length during reassembly");
    p_ret = NULL;
  }
  /* single packet */
  else if (pkt_type == AVDT_PKT_TYPE_SINGLE) {
    /* if reassembly in progress drop message and process new single */
    if (p_ccb->p_rx_msg != NULL)
      AVDT_TRACE_WARNING("Got single during reassembly");

    osi_free_and_reset((void**)&p_ccb->p_rx_msg);

    p_ret = p_buf;
  }
  /* start packet */
  else if (pkt_type == AVDT_PKT_TYPE_START) {
    /* if reassembly in progress drop message and process new single */
    if (p_ccb->p_rx_msg != NULL)
      AVDT_TRACE_WARNING("Got start during reassembly");

    osi_free_and_reset((void**)&p_ccb->p_rx_msg);

    /*
     * Allocate bigger buffer for reassembly. As lower layers are
     * not aware of possible packet size after reassembly, they
     * would have allocated smaller buffer.
     */
    if (sizeof(BT_HDR) + p_buf->offset + p_buf->len > BT_DEFAULT_BUFFER_SIZE) {
      android_errorWriteLog(0x534e4554, "232023771");
      osi_free(p_buf);
      p_ret = NULL;
      return p_ret;
    }
    p_ccb->p_rx_msg = (BT_HDR*)osi_malloc(BT_DEFAULT_BUFFER_SIZE);
    memcpy(p_ccb->p_rx_msg, p_buf, sizeof(BT_HDR) + p_buf->offset + p_buf->len);

    /* Free original buffer */
    osi_free(p_buf);

    /* update p to point to new buffer */
    p = (uint8_t*)(p_ccb->p_rx_msg + 1) + p_ccb->p_rx_msg->offset;

    /* copy first header byte over nosp */
    *(p + 1) = *p;

    /* set offset to point to where to copy next */
    p_ccb->p_rx_msg->offset += p_ccb->p_rx_msg->len;

    /* adjust length for packet header */
    p_ccb->p_rx_msg->len -= 1;

    p_ret = NULL;
  }
  /* continue or end */
  else {
    /* if no reassembly in progress drop message */
    if (p_ccb->p_rx_msg == NULL) {
      osi_free(p_buf);
      AVDT_TRACE_WARNING("Pkt type=%d out of order", pkt_type);
      p_ret = NULL;
    } else {
      /* get size of buffer holding assembled message */
      /*
       * NOTE: The buffer is allocated above at the beginning of the
       * reassembly, and is always of size BT_DEFAULT_BUFFER_SIZE.
       */
      size_t buf_len = BT_DEFAULT_BUFFER_SIZE - sizeof(BT_HDR);

      /* adjust offset and len of fragment for header byte */
      p_buf->offset += AVDT_LEN_TYPE_CONT;
      p_buf->len -= AVDT_LEN_TYPE_CONT;

      /* verify length */
      if (((size_t) p_ccb->p_rx_msg->offset + (size_t) p_buf->len) > buf_len) {
        /* won't fit; free everything */
        AVDT_TRACE_WARNING("%s: Fragmented message too big!", __func__);
        osi_free_and_reset((void**)&p_ccb->p_rx_msg);
        osi_free(p_buf);
        p_ret = NULL;
      } else {
        /* copy contents of p_buf to p_rx_msg */
        memcpy((uint8_t*)(p_ccb->p_rx_msg + 1) + p_ccb->p_rx_msg->offset,
               (uint8_t*)(p_buf + 1) + p_buf->offset, p_buf->len);

        if (pkt_type == AVDT_PKT_TYPE_END) {
          p_ccb->p_rx_msg->offset -= p_ccb->p_rx_msg->len;
          p_ccb->p_rx_msg->len += p_buf->len;
          p_ret = p_ccb->p_rx_msg;
          p_ccb->p_rx_msg = NULL;
        } else {
          p_ccb->p_rx_msg->offset += p_buf->len;
          p_ccb->p_rx_msg->len += p_buf->len;
          p_ret = NULL;
        }
        osi_free(p_buf);
      }
    }
  }
  return p_ret;
}

/*******************************************************************************
 *
 * Function         avdt_msg_send_cmd
 *
 * Description      This function is called to send a command message.  The
 *                  sig_id parameter indicates the message type, p_params
 *                  points to the message parameters, if any.  It gets a buffer
 *                  from the AVDTP command pool, executes the message building
 *                  function for this message type.  It then queues the message
 *                  in the command queue for this CCB.
 *
 *
 * Returns          Nothing.
 *
 ******************************************************************************/
void avdt_msg_send_cmd(AvdtpCcb* p_ccb, void* p_scb, uint8_t sig_id,
                       tAVDT_MSG* p_params) {
  uint8_t* p;
  uint8_t* p_start;
  BT_HDR* p_buf = (BT_HDR*)osi_malloc(AVDT_CMD_BUF_SIZE);

  /* set up buf pointer and offset */
  p_buf->offset = AVDT_MSG_OFFSET;
  p_start = p = (uint8_t*)(p_buf + 1) + p_buf->offset;

  /* execute parameter building function to build message */
  (*avdt_msg_bld_cmd[sig_id - 1])(&p, p_params);

  /* set len */
  p_buf->len = (uint16_t)(p - p_start);

  /* now store scb hdls, if any, in buf */
  if (p_scb != NULL) {
    p = (uint8_t*)(p_buf + 1);

    /* for start and suspend, p_scb points to array of handles */
    if ((sig_id == AVDT_SIG_START) || (sig_id == AVDT_SIG_SUSPEND)) {
      memcpy(p, (uint8_t*)p_scb, p_buf->len);
    }
    /* for all others, p_scb points to scb as usual */
    else {
      *p = avdt_scb_to_hdl((AvdtpScb*)p_scb);
    }
  }

  /* stash sig, label, and message type in buf */
  p_buf->event = sig_id;
  AVDT_BLD_LAYERSPEC(p_buf->layer_specific, AVDT_MSG_TYPE_CMD, p_ccb->label);

  /* increment label */
  p_ccb->label = (p_ccb->label + 1) % 16;

  /* queue message and trigger ccb to send it */
  fixed_queue_enqueue(p_ccb->cmd_q, p_buf);
  avdt_ccb_event(p_ccb, AVDT_CCB_SENDMSG_EVT, NULL);
}

/*******************************************************************************
 *
 * Function         avdt_msg_send_rsp
 *
 * Description      This function is called to send a response message.  The
 *                  sig_id parameter indicates the message type, p_params
 *                  points to the message parameters, if any.  It gets a buffer
 *                  from the AVDTP command pool, executes the message building
 *                  function for this message type.  It then queues the message
 *                  in the response queue for this CCB.
 *
 *
 * Returns          Nothing.
 *
 ******************************************************************************/
void avdt_msg_send_rsp(AvdtpCcb* p_ccb, uint8_t sig_id, tAVDT_MSG* p_params) {
  uint8_t* p;
  uint8_t* p_start;
  BT_HDR* p_buf = (BT_HDR*)osi_malloc(AVDT_CMD_BUF_SIZE);

  /* set up buf pointer and offset */
  p_buf->offset = AVDT_MSG_OFFSET;
  p_start = p = (uint8_t*)(p_buf + 1) + p_buf->offset;

  /* execute parameter building function to build message */
  (*avdt_msg_bld_rsp[sig_id - 1])(&p, p_params);

  /* set length */
  p_buf->len = (uint16_t)(p - p_start);

  /* stash sig, label, and message type in buf */
  p_buf->event = sig_id;
  AVDT_BLD_LAYERSPEC(p_buf->layer_specific, AVDT_MSG_TYPE_RSP,
                     p_params->hdr.label);

  /* queue message and trigger ccb to send it */
  fixed_queue_enqueue(p_ccb->rsp_q, p_buf);
  avdt_ccb_event(p_ccb, AVDT_CCB_SENDMSG_EVT, NULL);
}

/*******************************************************************************
 *
 * Function         avdt_msg_send_rej
 *
 * Description      This function is called to send a reject message.  The
 *                  sig_id parameter indicates the message type.  It gets
 *                  a buffer from the AVDTP command pool and builds the
 *                  message based on the message type and the error code.
 *                  It then queues the message in the response queue for
 *                  this CCB.
 *
 *
 * Returns          Nothing.
 *
 ******************************************************************************/
void avdt_msg_send_rej(AvdtpCcb* p_ccb, uint8_t sig_id, tAVDT_MSG* p_params) {
  uint8_t* p;
  uint8_t* p_start;
  BT_HDR* p_buf = (BT_HDR*)osi_malloc(AVDT_CMD_BUF_SIZE);

  /* set up buf pointer and offset */
  p_buf->offset = AVDT_MSG_OFFSET;
  p_start = p = (uint8_t*)(p_buf + 1) + p_buf->offset;

  /* if sig id included, build into message */
  if (sig_id != AVDT_SIG_NONE) {
    /* if this sig has a parameter, add the parameter */
    if ((sig_id == AVDT_SIG_SETCONFIG) || (sig_id == AVDT_SIG_RECONFIG)) {
      AVDT_MSG_BLD_PARAM(p, p_params->hdr.err_param);
    } else if ((sig_id == AVDT_SIG_START) || (sig_id == AVDT_SIG_SUSPEND)) {
      AVDT_MSG_BLD_SEID(p, p_params->hdr.err_param);
    }

    /* add the error code */
    AVDT_MSG_BLD_ERR(p, p_params->hdr.err_code);
  }
  AVDT_TRACE_DEBUG("avdt_msg_send_rej");

  /* calculate length */
  p_buf->len = (uint16_t)(p - p_start);

  /* stash sig, label, and message type in buf */
  p_buf->event = sig_id;
  AVDT_BLD_LAYERSPEC(p_buf->layer_specific, AVDT_MSG_TYPE_REJ,
                     p_params->hdr.label);

  /* queue message and trigger ccb to send it */
  fixed_queue_enqueue(p_ccb->rsp_q, p_buf);
  avdt_ccb_event(p_ccb, AVDT_CCB_SENDMSG_EVT, NULL);
}

/*******************************************************************************
 *
 * Function         avdt_msg_send_grej
 *
 * Description      This function is called to send a general reject message.
 *                  The sig_id parameter indicates the message type.  It gets
 *                  a buffer from the AVDTP command pool and builds the
 *                  message based on the message type and the error code.
 *                  It then queues the message in the response queue for
 *                  this CCB.
 *
 *
 * Returns          Nothing.
 *
 ******************************************************************************/
void avdt_msg_send_grej(AvdtpCcb* p_ccb, uint8_t sig_id, tAVDT_MSG* p_params) {
  uint8_t* p;
  uint8_t* p_start;
  BT_HDR* p_buf = (BT_HDR*)osi_malloc(AVDT_CMD_BUF_SIZE);

  /* set up buf pointer and offset */
  p_buf->offset = AVDT_MSG_OFFSET;
  p_start = p = (uint8_t*)(p_buf + 1) + p_buf->offset;

  /* calculate length */
  p_buf->len = (uint16_t)(p - p_start);

  /* stash sig, label, and message type in buf */
  p_buf->event = sig_id;
  AVDT_BLD_LAYERSPEC(p_buf->layer_specific, AVDT_MSG_TYPE_GRJ,
                     p_params->hdr.label);
  AVDT_TRACE_DEBUG(__func__);

  /* queue message and trigger ccb to send it */
  fixed_queue_enqueue(p_ccb->rsp_q, p_buf);
  avdt_ccb_event(p_ccb, AVDT_CCB_SENDMSG_EVT, NULL);
}

/*******************************************************************************
 *
 * Function         avdt_msg_ind
 *
 * Description      This function is called by the adaption layer when an
 *                  incoming message is received on the signaling channel.
 *                  It parses the message and sends an event to the appropriate
 *                  SCB or CCB for the message.
 *
 *
 * Returns          Nothing.
 *
 ******************************************************************************/
void avdt_msg_ind(AvdtpCcb* p_ccb, BT_HDR* p_buf) {
  AvdtpScb* p_scb;
  uint8_t* p;
  bool ok = true;
  bool handle_rsp = false;
  bool gen_rej = false;
  uint8_t label;
  uint8_t pkt_type;
  uint8_t msg_type;
  uint8_t sig = 0;
  tAVDT_MSG msg{};
  AvdtpSepConfig cfg{};
  uint8_t err;
  uint8_t evt = 0;
  uint8_t scb_hdl;

  /* reassemble message; if no message available (we received a fragment) return
   */
  p_buf = avdt_msg_asmbl(p_ccb, p_buf);
  if (p_buf == NULL) {
    return;
  }

  p = (uint8_t*)(p_buf + 1) + p_buf->offset;

  /* parse the message header */
  AVDT_MSG_PRS_HDR(p, label, pkt_type, msg_type);

  AVDT_TRACE_DEBUG("msg_type=%d, sig=%d", msg_type, sig);
  /* set up label and ccb_idx in message hdr */
  msg.hdr.label = label;
  msg.hdr.ccb_idx = avdt_ccb_to_idx(p_ccb);

  /* verify msg type */
  if (msg_type == AVDT_MSG_TYPE_GRJ) {
    AVDT_TRACE_WARNING("Dropping msg msg_type=%d", msg_type);
    ok = false;
  }
  /* check for general reject */
  else if ((msg_type == AVDT_MSG_TYPE_REJ) &&
           (p_buf->len == AVDT_LEN_GEN_REJ)) {
    gen_rej = true;
    if (p_ccb->p_curr_cmd != NULL) {
      msg.hdr.sig_id = sig = (uint8_t)p_ccb->p_curr_cmd->event;
      evt = avdt_msg_rej_2_evt[sig - 1];
      msg.hdr.err_code = AVDT_ERR_NSC;
      msg.hdr.err_param = 0;
    }
  } else /* not a general reject */
  {
    /* get and verify signal */
    AVDT_MSG_PRS_SIG(p, sig);
    msg.hdr.sig_id = sig;
    if ((sig == 0) || (sig > AVDT_SIG_MAX)) {
      AVDT_TRACE_WARNING("Dropping msg sig=%d msg_type:%d", sig, msg_type);
      ok = false;

      /* send a general reject */
      if (msg_type == AVDT_MSG_TYPE_CMD) {
        avdt_msg_send_grej(p_ccb, sig, &msg);
      }
    }

    /* validate reject/response against cached sig */
    if (((msg_type == AVDT_MSG_TYPE_RSP) || (msg_type == AVDT_MSG_TYPE_REJ)) &&
        (p_ccb->p_curr_cmd == nullptr || p_ccb->p_curr_cmd->event != sig)) {
      AVDT_TRACE_WARNING(
          "Dropping msg with mismatched sig; sig=%d", sig);
      ok = false;
    }
  }

  if (ok && !gen_rej) {
    /* skip over header (msg length already verified during reassembly) */
    p_buf->len -= AVDT_LEN_TYPE_SINGLE;

    /* set up to parse message */
    if ((msg_type == AVDT_MSG_TYPE_RSP) && (sig == AVDT_SIG_DISCOVER)) {
      /* parse discover rsp message to struct supplied by app */
      msg.discover_rsp.p_sep_info = (tAVDT_SEP_INFO*)p_ccb->p_proc_data;
      msg.discover_rsp.num_seps = p_ccb->proc_param;
    } else if ((msg_type == AVDT_MSG_TYPE_RSP) &&
               ((sig == AVDT_SIG_GETCAP) || (sig == AVDT_SIG_GET_ALLCAP))) {
      /* parse discover rsp message to struct supplied by app */
      msg.svccap.p_cfg = (AvdtpSepConfig*)p_ccb->p_proc_data;
    } else if ((msg_type == AVDT_MSG_TYPE_RSP) && (sig == AVDT_SIG_GETCONFIG)) {
      /* parse get config rsp message to struct allocated locally */
      msg.svccap.p_cfg = &cfg;
    } else if ((msg_type == AVDT_MSG_TYPE_CMD) && (sig == AVDT_SIG_SETCONFIG)) {
      /* parse config cmd message to struct allocated locally */
      msg.config_cmd.p_cfg = &cfg;
    } else if ((msg_type == AVDT_MSG_TYPE_CMD) && (sig == AVDT_SIG_RECONFIG)) {
      /* parse reconfig cmd message to struct allocated locally */
      msg.reconfig_cmd.p_cfg = &cfg;
    }

    /* parse message; while we're at it map message sig to event */
    if (msg_type == AVDT_MSG_TYPE_CMD) {
      msg.hdr.err_code = err =
          (*avdt_msg_prs_cmd[sig - 1])(&msg, p, p_buf->len);
      evt = avdt_msg_cmd_2_evt[sig - 1];
    } else if (msg_type == AVDT_MSG_TYPE_RSP) {
      msg.hdr.err_code = err =
          (*avdt_msg_prs_rsp[sig - 1])(&msg, p, p_buf->len);
      evt = avdt_msg_rsp_2_evt[sig - 1];
    } else /* msg_type == AVDT_MSG_TYPE_REJ */
    {
      err = avdt_msg_prs_rej(&msg, p, p_buf->len, sig);
      evt = avdt_msg_rej_2_evt[sig - 1];
    }

    /* if parsing failed */
    if (err != 0) {
      AVDT_TRACE_WARNING("Parsing failed sig=%d err=0x%x", sig, err);

      /* if its a rsp or rej, drop it; if its a cmd, send a rej;
      ** note special case for abort; never send abort reject
      */
      ok = false;
      if ((msg_type == AVDT_MSG_TYPE_CMD) && (sig != AVDT_SIG_ABORT)) {
        avdt_msg_send_rej(p_ccb, sig, &msg);
      }
    }
  }

  /* if its a rsp or rej, check sent cmd to see if we're waiting for
  ** the rsp or rej.  If we didn't send a cmd for it, drop it.  If
  ** it does match a cmd, stop timer for the cmd.
  */
  if (ok) {
    if ((msg_type == AVDT_MSG_TYPE_RSP) || (msg_type == AVDT_MSG_TYPE_REJ)) {
      if ((p_ccb->p_curr_cmd != NULL) && (p_ccb->p_curr_cmd->event == sig) &&
          (AVDT_LAYERSPEC_LABEL(p_ccb->p_curr_cmd->layer_specific) == label)) {
        /* stop timer */
        alarm_cancel(p_ccb->idle_ccb_timer);
        alarm_cancel(p_ccb->ret_ccb_timer);
        alarm_cancel(p_ccb->rsp_ccb_timer);

        /* clear retransmission count */
        p_ccb->ret_count = 0;

        /* later in this function handle ccb event */
        handle_rsp = true;
      } else {
        ok = false;
        AVDT_TRACE_WARNING("Cmd not found for rsp sig=%d label=%d", sig, label);
      }
    }
  }

  if (ok) {
    /* if it's a ccb event send to ccb */
    if (evt & AVDT_CCB_MKR) {
      tAVDT_CCB_EVT avdt_ccb_evt;
      avdt_ccb_evt.msg = msg;
      avdt_ccb_event(p_ccb, (uint8_t)(evt & ~AVDT_CCB_MKR), &avdt_ccb_evt);
    }
    /* if it's a scb event */
    else {
      /* Scb events always have a single seid.  For cmd, get seid from
      ** message.  For rej and rsp, get seid from p_curr_cmd.
      */
      if (msg_type == AVDT_MSG_TYPE_CMD) {
        scb_hdl = msg.single.seid;
      } else {
        scb_hdl = *((uint8_t*)(p_ccb->p_curr_cmd + 1));
      }

      /* Map seid to the scb and send it the event.  For cmd, seid has
      ** already been verified by parsing function.
      */
      if (evt) {
        p_scb = avdt_scb_by_hdl(scb_hdl);
        if (p_scb != NULL) {
          tAVDT_SCB_EVT avdt_scb_evt;
          avdt_scb_evt.msg = msg;
          avdt_scb_event(p_scb, evt, &avdt_scb_evt);
        }
      }
    }
  }

  /* free message buffer */
  osi_free(p_buf);

  /* if its a rsp or rej, send event to ccb to free associated
  ** cmd msg buffer and handle cmd queue
  */
  if (handle_rsp) {
    avdt_ccb_event(p_ccb, AVDT_CCB_RCVRSP_EVT, NULL);
  }
}
