/*-
 *   BSD LICENSE
 *
 *   Copyright(c) 2010-2015 Intel Corporation. All rights reserved.
 *   All rights reserved.
 *
 *   Redistribution and use in source and binary forms, with or without
 *   modification, are permitted provided that the following conditions
 *   are met:
 *
 *     * Redistributions of source code must retain the above copyright
 *       notice, this list of conditions and the following disclaimer.
 *     * Redistributions in binary form must reproduce the above copyright
 *       notice, this list of conditions and the following disclaimer in
 *       the documentation and/or other materials provided with the
 *       distribution.
 *     * Neither the name of Intel Corporation nor the names of its
 *       contributors may be used to endorse or promote products derived
 *       from this software without specific prior written permission.
 *
 *   THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 *   "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 *   LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
 *   A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
 *   OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
 *   SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
 *   LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
 *   DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
 *   THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 *   (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 *   OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#include <sys/queue.h>
#include <stdio.h>
#include <errno.h>
#include <stdint.h>
#include <string.h>
#include <unistd.h>
#include <stdarg.h>
#include <inttypes.h>
#include <rte_byteorder.h>
#include <rte_common.h>
#include <rte_cycles.h>

#include <rte_interrupts.h>
#include <rte_log.h>
#include <rte_debug.h>
#include <rte_pci.h>
#include <rte_atomic.h>
#include <rte_branch_prediction.h>
#include <rte_memory.h>
#include <rte_memzone.h>
#include <rte_eal.h>
#include <rte_alarm.h>
#include <rte_ether.h>
#include <rte_ethdev.h>
#include <rte_atomic.h>
#include <rte_malloc.h>
#include <rte_dev.h>

#include "i40e_logs.h"
#include "base/i40e_prototype.h"
#include "base/i40e_adminq_cmd.h"
#include "base/i40e_type.h"

#include "i40e_rxtx.h"
#include "i40e_ethdev.h"
#include "i40e_pf.h"
#define I40EVF_VSI_DEFAULT_MSIX_INTR     1
#define I40EVF_VSI_DEFAULT_MSIX_INTR_LNX 0

/* busy wait delay in msec */
#define I40EVF_BUSY_WAIT_DELAY 10
#define I40EVF_BUSY_WAIT_COUNT 50
#define MAX_RESET_WAIT_CNT     20
/*ITR index for NOITR*/
#define I40E_QINT_RQCTL_MSIX_INDX_NOITR     3

struct i40evf_arq_msg_info {
	enum i40e_virtchnl_ops ops;
	enum i40e_status_code result;
	uint16_t buf_len;
	uint16_t msg_len;
	uint8_t *msg;
};

struct vf_cmd_info {
	enum i40e_virtchnl_ops ops;
	uint8_t *in_args;
	uint32_t in_args_size;
	uint8_t *out_buffer;
	/* Input & output type. pass in buffer size and pass out
	 * actual return result
	 */
	uint32_t out_size;
};

enum i40evf_aq_result {
	I40EVF_MSG_ERR = -1, /* Meet error when accessing admin queue */
	I40EVF_MSG_NON,      /* Read nothing from admin queue */
	I40EVF_MSG_SYS,      /* Read system msg from admin queue */
	I40EVF_MSG_CMD,      /* Read async command result */
};

/* A share buffer to store the command result from PF driver */
static uint8_t cmd_result_buffer[I40E_AQ_BUF_SZ];

static int i40evf_dev_configure(struct rte_eth_dev *dev);
static int i40evf_dev_start(struct rte_eth_dev *dev);
static void i40evf_dev_stop(struct rte_eth_dev *dev);
static void i40evf_dev_info_get(struct rte_eth_dev *dev,
				struct rte_eth_dev_info *dev_info);
static int i40evf_dev_link_update(struct rte_eth_dev *dev,
				  __rte_unused int wait_to_complete);
static void i40evf_dev_stats_get(struct rte_eth_dev *dev,
				struct rte_eth_stats *stats);
static int i40evf_dev_xstats_get(struct rte_eth_dev *dev,
				 struct rte_eth_xstats *xstats, unsigned n);
static void i40evf_dev_xstats_reset(struct rte_eth_dev *dev);
static int i40evf_vlan_filter_set(struct rte_eth_dev *dev,
				  uint16_t vlan_id, int on);
static void i40evf_vlan_offload_set(struct rte_eth_dev *dev, int mask);
static int i40evf_vlan_pvid_set(struct rte_eth_dev *dev, uint16_t pvid,
				int on);
static void i40evf_dev_close(struct rte_eth_dev *dev);
static void i40evf_dev_promiscuous_enable(struct rte_eth_dev *dev);
static void i40evf_dev_promiscuous_disable(struct rte_eth_dev *dev);
static void i40evf_dev_allmulticast_enable(struct rte_eth_dev *dev);
static void i40evf_dev_allmulticast_disable(struct rte_eth_dev *dev);
static int i40evf_get_link_status(struct rte_eth_dev *dev,
				  struct rte_eth_link *link);
static int i40evf_init_vlan(struct rte_eth_dev *dev);
static int i40evf_dev_rx_queue_start(struct rte_eth_dev *dev,
				     uint16_t rx_queue_id);
static int i40evf_dev_rx_queue_stop(struct rte_eth_dev *dev,
				    uint16_t rx_queue_id);
static int i40evf_dev_tx_queue_start(struct rte_eth_dev *dev,
				     uint16_t tx_queue_id);
static int i40evf_dev_tx_queue_stop(struct rte_eth_dev *dev,
				    uint16_t tx_queue_id);
static void i40evf_add_mac_addr(struct rte_eth_dev *dev,
				struct ether_addr *addr,
				uint32_t index,
				uint32_t pool);
static void i40evf_del_mac_addr(struct rte_eth_dev *dev, uint32_t index);
static int i40evf_dev_rss_reta_update(struct rte_eth_dev *dev,
			struct rte_eth_rss_reta_entry64 *reta_conf,
			uint16_t reta_size);
static int i40evf_dev_rss_reta_query(struct rte_eth_dev *dev,
			struct rte_eth_rss_reta_entry64 *reta_conf,
			uint16_t reta_size);
static int i40evf_config_rss(struct i40e_vf *vf);
static int i40evf_dev_rss_hash_update(struct rte_eth_dev *dev,
				      struct rte_eth_rss_conf *rss_conf);
static int i40evf_dev_rss_hash_conf_get(struct rte_eth_dev *dev,
					struct rte_eth_rss_conf *rss_conf);
static int
i40evf_dev_rx_queue_intr_enable(struct rte_eth_dev *dev, uint16_t queue_id);
static int
i40evf_dev_rx_queue_intr_disable(struct rte_eth_dev *dev, uint16_t queue_id);

/* Default hash key buffer for RSS */
static uint32_t rss_key_default[I40E_VFQF_HKEY_MAX_INDEX + 1];

struct rte_i40evf_xstats_name_off {
	char name[RTE_ETH_XSTATS_NAME_SIZE];
	unsigned offset;
};

static const struct rte_i40evf_xstats_name_off rte_i40evf_stats_strings[] = {
	{"rx_bytes", offsetof(struct i40e_eth_stats, rx_bytes)},
	{"rx_unicast_packets", offsetof(struct i40e_eth_stats, rx_unicast)},
	{"rx_multicast_packets", offsetof(struct i40e_eth_stats, rx_multicast)},
	{"rx_broadcast_packets", offsetof(struct i40e_eth_stats, rx_broadcast)},
	{"rx_dropped_packets", offsetof(struct i40e_eth_stats, rx_discards)},
	{"rx_unknown_protocol_packets", offsetof(struct i40e_eth_stats,
		rx_unknown_protocol)},
	{"tx_bytes", offsetof(struct i40e_eth_stats, tx_bytes)},
	{"tx_unicast_packets", offsetof(struct i40e_eth_stats, tx_bytes)},
	{"tx_multicast_packets", offsetof(struct i40e_eth_stats, tx_bytes)},
	{"tx_broadcast_packets", offsetof(struct i40e_eth_stats, tx_bytes)},
	{"tx_dropped_packets", offsetof(struct i40e_eth_stats, tx_bytes)},
	{"tx_error_packets", offsetof(struct i40e_eth_stats, tx_bytes)},
};

#define I40EVF_NB_XSTATS (sizeof(rte_i40evf_stats_strings) / \
		sizeof(rte_i40evf_stats_strings[0]))

static const struct eth_dev_ops i40evf_eth_dev_ops = {
	.dev_configure        = i40evf_dev_configure,
	.dev_start            = i40evf_dev_start,
	.dev_stop             = i40evf_dev_stop,
	.promiscuous_enable   = i40evf_dev_promiscuous_enable,
	.promiscuous_disable  = i40evf_dev_promiscuous_disable,
	.allmulticast_enable  = i40evf_dev_allmulticast_enable,
	.allmulticast_disable = i40evf_dev_allmulticast_disable,
	.link_update          = i40evf_dev_link_update,
	.stats_get            = i40evf_dev_stats_get,
	.xstats_get           = i40evf_dev_xstats_get,
	.xstats_reset         = i40evf_dev_xstats_reset,
	.dev_close            = i40evf_dev_close,
	.dev_infos_get        = i40evf_dev_info_get,
	.vlan_filter_set      = i40evf_vlan_filter_set,
	.vlan_offload_set     = i40evf_vlan_offload_set,
	.vlan_pvid_set        = i40evf_vlan_pvid_set,
	.rx_queue_start       = i40evf_dev_rx_queue_start,
	.rx_queue_stop        = i40evf_dev_rx_queue_stop,
	.tx_queue_start       = i40evf_dev_tx_queue_start,
	.tx_queue_stop        = i40evf_dev_tx_queue_stop,
	.rx_queue_setup       = i40e_dev_rx_queue_setup,
	.rx_queue_release     = i40e_dev_rx_queue_release,
	.rx_queue_intr_enable = i40evf_dev_rx_queue_intr_enable,
	.rx_queue_intr_disable = i40evf_dev_rx_queue_intr_disable,
	.rx_descriptor_done   = i40e_dev_rx_descriptor_done,
	.tx_queue_setup       = i40e_dev_tx_queue_setup,
	.tx_queue_release     = i40e_dev_tx_queue_release,
	.mac_addr_add	      = i40evf_add_mac_addr,
	.mac_addr_remove      = i40evf_del_mac_addr,
	.reta_update          = i40evf_dev_rss_reta_update,
	.reta_query           = i40evf_dev_rss_reta_query,
	.rss_hash_update      = i40evf_dev_rss_hash_update,
	.rss_hash_conf_get    = i40evf_dev_rss_hash_conf_get,
};

/*
 * Parse admin queue message.
 *
 * return value:
 *  < 0: meet error
 *  0: read sys msg
 *  > 0: read cmd result
 */
static enum i40evf_aq_result
i40evf_parse_pfmsg(struct i40e_vf *vf,
		   struct i40e_arq_event_info *event,
		   struct i40evf_arq_msg_info *data)
{
	enum i40e_virtchnl_ops opcode = (enum i40e_virtchnl_ops)\
			rte_le_to_cpu_32(event->desc.cookie_high);
	enum i40e_status_code retval = (enum i40e_status_code)\
			rte_le_to_cpu_32(event->desc.cookie_low);
	enum i40evf_aq_result ret = I40EVF_MSG_CMD;

	/* pf sys event */
	if (opcode == I40E_VIRTCHNL_OP_EVENT) {
		struct i40e_virtchnl_pf_event *vpe =
			(struct i40e_virtchnl_pf_event *)event->msg_buf;

		/* Initialize ret to sys event */
		ret = I40EVF_MSG_SYS;
		switch (vpe->event) {
		case I40E_VIRTCHNL_EVENT_LINK_CHANGE:
			vf->link_up =
				vpe->event_data.link_event.link_status;
			vf->pend_msg |= PFMSG_LINK_CHANGE;
			PMD_DRV_LOG(INFO, "Link status update:%s",
				    vf->link_up ? "up" : "down");
			break;
		case I40E_VIRTCHNL_EVENT_RESET_IMPENDING:
			vf->vf_reset = true;
			vf->pend_msg |= PFMSG_RESET_IMPENDING;
			PMD_DRV_LOG(INFO, "vf is reseting");
			break;
		case I40E_VIRTCHNL_EVENT_PF_DRIVER_CLOSE:
			vf->dev_closed = true;
			vf->pend_msg |= PFMSG_DRIVER_CLOSE;
			PMD_DRV_LOG(INFO, "PF driver closed");
			break;
		default:
			PMD_DRV_LOG(ERR, "%s: Unknown event %d from pf",
				    __func__, vpe->event);
		}
	} else {
		/* async reply msg on command issued by vf previously */
		ret = I40EVF_MSG_CMD;
		/* Actual data length read from PF */
		data->msg_len = event->msg_len;
	}
	/* fill the ops and result to notify VF */
	data->result = retval;
	data->ops = opcode;

	return ret;
}

/*
 * Read data in admin queue to get msg from pf driver
 */
static enum i40evf_aq_result
i40evf_read_pfmsg(struct rte_eth_dev *dev, struct i40evf_arq_msg_info *data)
{
	struct i40e_hw *hw = I40E_DEV_PRIVATE_TO_HW(dev->data->dev_private);
	struct i40e_vf *vf = I40EVF_DEV_PRIVATE_TO_VF(dev->data->dev_private);
	struct i40e_arq_event_info event;
	int ret;
	enum i40evf_aq_result result = I40EVF_MSG_NON;

	event.buf_len = data->buf_len;
	event.msg_buf = data->msg;
	ret = i40e_clean_arq_element(hw, &event, NULL);
	/* Can't read any msg from adminQ */
	if (ret) {
		if (ret == I40E_ERR_ADMIN_QUEUE_NO_WORK)
			result = I40EVF_MSG_NON;
		else
			result = I40EVF_MSG_ERR;
		return result;
	}

	/* Parse the event */
	result = i40evf_parse_pfmsg(vf, &event, data);

	return result;
}

/*
 * Polling read until command result return from pf driver or meet error.
 */
static int
i40evf_wait_cmd_done(struct rte_eth_dev *dev,
		     struct i40evf_arq_msg_info *data)
{
	int i = 0;
	enum i40evf_aq_result ret;

#define MAX_TRY_TIMES 20
#define ASQ_DELAY_MS  100
	do {
		/* Delay some time first */
		rte_delay_ms(ASQ_DELAY_MS);
		ret = i40evf_read_pfmsg(dev, data);
		if (ret == I40EVF_MSG_CMD)
			return 0;
		else if (ret == I40EVF_MSG_ERR)
			return -1;

		/* If don't read msg or read sys event, continue */
	} while(i++ < MAX_TRY_TIMES);

	return -1;
}

/**
 * clear current command. Only call in case execute
 * _atomic_set_cmd successfully.
 */
static inline void
_clear_cmd(struct i40e_vf *vf)
{
	rte_wmb();
	vf->pend_cmd = I40E_VIRTCHNL_OP_UNKNOWN;
}

/*
 * Check there is pending cmd in execution. If none, set new command.
 */
static inline int
_atomic_set_cmd(struct i40e_vf *vf, enum i40e_virtchnl_ops ops)
{
	int ret = rte_atomic32_cmpset(&vf->pend_cmd,
			I40E_VIRTCHNL_OP_UNKNOWN, ops);

	if (!ret)
		PMD_DRV_LOG(ERR, "There is incomplete cmd %d", vf->pend_cmd);

	return !ret;
}

static int
i40evf_execute_vf_cmd(struct rte_eth_dev *dev, struct vf_cmd_info *args)
{
	struct i40e_hw *hw = I40E_DEV_PRIVATE_TO_HW(dev->data->dev_private);
	struct i40e_vf *vf = I40EVF_DEV_PRIVATE_TO_VF(dev->data->dev_private);
	int err = -1;
	struct i40evf_arq_msg_info info;

	if (_atomic_set_cmd(vf, args->ops))
		return -1;

	info.msg = args->out_buffer;
	info.buf_len = args->out_size;
	info.ops = I40E_VIRTCHNL_OP_UNKNOWN;
	info.result = I40E_SUCCESS;

	err = i40e_aq_send_msg_to_pf(hw, args->ops, I40E_SUCCESS,
		     args->in_args, args->in_args_size, NULL);
	if (err) {
		PMD_DRV_LOG(ERR, "fail to send cmd %d", args->ops);
		_clear_cmd(vf);
		return err;
	}

	err = i40evf_wait_cmd_done(dev, &info);
	/* read message and it's expected one */
	if (!err && args->ops == info.ops)
		_clear_cmd(vf);
	else if (err) {
		PMD_DRV_LOG(ERR, "Failed to read message from AdminQ");
		_clear_cmd(vf);
	}
	else if (args->ops != info.ops)
		PMD_DRV_LOG(ERR, "command mismatch, expect %u, get %u",
			    args->ops, info.ops);

	return err | info.result;
}

/*
 * Check API version with sync wait until version read or fail from admin queue
 */
static int
i40evf_check_api_version(struct rte_eth_dev *dev)
{
	struct i40e_virtchnl_version_info version, *pver;
	int err;
	struct vf_cmd_info args;
	struct i40e_vf *vf = I40EVF_DEV_PRIVATE_TO_VF(dev->data->dev_private);

	version.major = I40E_VIRTCHNL_VERSION_MAJOR;
	version.minor = I40E_VIRTCHNL_VERSION_MINOR;

	args.ops = I40E_VIRTCHNL_OP_VERSION;
	args.in_args = (uint8_t *)&version;
	args.in_args_size = sizeof(version);
	args.out_buffer = cmd_result_buffer;
	args.out_size = I40E_AQ_BUF_SZ;

	err = i40evf_execute_vf_cmd(dev, &args);
	if (err) {
		PMD_INIT_LOG(ERR, "fail to execute command OP_VERSION");
		return err;
	}

	pver = (struct i40e_virtchnl_version_info *)args.out_buffer;
	vf->version_major = pver->major;
	vf->version_minor = pver->minor;
	if (vf->version_major == I40E_DPDK_VERSION_MAJOR)
		PMD_DRV_LOG(INFO, "Peer is DPDK PF host");
	else if ((vf->version_major == I40E_VIRTCHNL_VERSION_MAJOR) &&
		(vf->version_minor <= I40E_VIRTCHNL_VERSION_MINOR))
		PMD_DRV_LOG(INFO, "Peer is Linux PF host");
	else {
		PMD_INIT_LOG(ERR, "PF/VF API version mismatch:(%u.%u)-(%u.%u)",
					vf->version_major, vf->version_minor,
						I40E_VIRTCHNL_VERSION_MAJOR,
						I40E_VIRTCHNL_VERSION_MINOR);
		return -1;
	}

	return 0;
}

static int
i40evf_get_vf_resource(struct rte_eth_dev *dev)
{
	struct i40e_hw *hw = I40E_DEV_PRIVATE_TO_HW(dev->data->dev_private);
	struct i40e_vf *vf = I40EVF_DEV_PRIVATE_TO_VF(dev->data->dev_private);
	int err;
	struct vf_cmd_info args;
	uint32_t caps, len;

	args.ops = I40E_VIRTCHNL_OP_GET_VF_RESOURCES;
	args.out_buffer = cmd_result_buffer;
	args.out_size = I40E_AQ_BUF_SZ;
	if (PF_IS_V11(vf)) {
		caps = I40E_VIRTCHNL_VF_OFFLOAD_L2 |
		       I40E_VIRTCHNL_VF_OFFLOAD_RSS_AQ |
		       I40E_VIRTCHNL_VF_OFFLOAD_RSS_REG |
		       I40E_VIRTCHNL_VF_OFFLOAD_VLAN |
		       I40E_VIRTCHNL_VF_OFFLOAD_RX_POLLING;
		args.in_args = (uint8_t *)&caps;
		args.in_args_size = sizeof(caps);
	} else {
		args.in_args = NULL;
		args.in_args_size = 0;
	}
	err = i40evf_execute_vf_cmd(dev, &args);

	if (err) {
		PMD_DRV_LOG(ERR, "fail to execute command OP_GET_VF_RESOURCE");
		return err;
	}

	len =  sizeof(struct i40e_virtchnl_vf_resource) +
		I40E_MAX_VF_VSI * sizeof(struct i40e_virtchnl_vsi_resource);

	(void)rte_memcpy(vf->vf_res, args.out_buffer,
			RTE_MIN(args.out_size, len));
	i40e_vf_parse_hw_config(hw, vf->vf_res);

	return 0;
}

static int
i40evf_config_promisc(struct rte_eth_dev *dev,
		      bool enable_unicast,
		      bool enable_multicast)
{
	struct i40e_vf *vf = I40EVF_DEV_PRIVATE_TO_VF(dev->data->dev_private);
	int err;
	struct vf_cmd_info args;
	struct i40e_virtchnl_promisc_info promisc;

	promisc.flags = 0;
	promisc.vsi_id = vf->vsi_res->vsi_id;

	if (enable_unicast)
		promisc.flags |= I40E_FLAG_VF_UNICAST_PROMISC;

	if (enable_multicast)
		promisc.flags |= I40E_FLAG_VF_MULTICAST_PROMISC;

	args.ops = I40E_VIRTCHNL_OP_CONFIG_PROMISCUOUS_MODE;
	args.in_args = (uint8_t *)&promisc;
	args.in_args_size = sizeof(promisc);
	args.out_buffer = cmd_result_buffer;
	args.out_size = I40E_AQ_BUF_SZ;

	err = i40evf_execute_vf_cmd(dev, &args);

	if (err)
		PMD_DRV_LOG(ERR, "fail to execute command "
			    "CONFIG_PROMISCUOUS_MODE");
	return err;
}

/* Configure vlan and double vlan offload. Use flag to specify which part to configure */
static int
i40evf_config_vlan_offload(struct rte_eth_dev *dev,
				bool enable_vlan_strip)
{
	struct i40e_vf *vf = I40EVF_DEV_PRIVATE_TO_VF(dev->data->dev_private);
	int err;
	struct vf_cmd_info args;
	struct i40e_virtchnl_vlan_offload_info offload;

	offload.vsi_id = vf->vsi_res->vsi_id;
	offload.enable_vlan_strip = enable_vlan_strip;

	args.ops = (enum i40e_virtchnl_ops)I40E_VIRTCHNL_OP_CFG_VLAN_OFFLOAD;
	args.in_args = (uint8_t *)&offload;
	args.in_args_size = sizeof(offload);
	args.out_buffer = cmd_result_buffer;
	args.out_size = I40E_AQ_BUF_SZ;

	err = i40evf_execute_vf_cmd(dev, &args);
	if (err)
		PMD_DRV_LOG(ERR, "fail to execute command CFG_VLAN_OFFLOAD");

	return err;
}

static int
i40evf_config_vlan_pvid(struct rte_eth_dev *dev,
				struct i40e_vsi_vlan_pvid_info *info)
{
	struct i40e_vf *vf = I40EVF_DEV_PRIVATE_TO_VF(dev->data->dev_private);
	int err;
	struct vf_cmd_info args;
	struct i40e_virtchnl_pvid_info tpid_info;

	if (dev == NULL || info == NULL) {
		PMD_DRV_LOG(ERR, "invalid parameters");
		return I40E_ERR_PARAM;
	}

	memset(&tpid_info, 0, sizeof(tpid_info));
	tpid_info.vsi_id = vf->vsi_res->vsi_id;
	(void)rte_memcpy(&tpid_info.info, info, sizeof(*info));

	args.ops = (enum i40e_virtchnl_ops)I40E_VIRTCHNL_OP_CFG_VLAN_PVID;
	args.in_args = (uint8_t *)&tpid_info;
	args.in_args_size = sizeof(tpid_info);
	args.out_buffer = cmd_result_buffer;
	args.out_size = I40E_AQ_BUF_SZ;

	err = i40evf_execute_vf_cmd(dev, &args);
	if (err)
		PMD_DRV_LOG(ERR, "fail to execute command CFG_VLAN_PVID");

	return err;
}

static void
i40evf_fill_virtchnl_vsi_txq_info(struct i40e_virtchnl_txq_info *txq_info,
				  uint16_t vsi_id,
				  uint16_t queue_id,
				  uint16_t nb_txq,
				  struct i40e_tx_queue *txq)
{
	txq_info->vsi_id = vsi_id;
	txq_info->queue_id = queue_id;
	if (queue_id < nb_txq) {
		txq_info->ring_len = txq->nb_tx_desc;
		txq_info->dma_ring_addr = txq->tx_ring_phys_addr;
	}
}

static void
i40evf_fill_virtchnl_vsi_rxq_info(struct i40e_virtchnl_rxq_info *rxq_info,
				  uint16_t vsi_id,
				  uint16_t queue_id,
				  uint16_t nb_rxq,
				  uint32_t max_pkt_size,
				  struct i40e_rx_queue *rxq)
{
	rxq_info->vsi_id = vsi_id;
	rxq_info->queue_id = queue_id;
	rxq_info->max_pkt_size = max_pkt_size;
	if (queue_id < nb_rxq) {
		rxq_info->ring_len = rxq->nb_rx_desc;
		rxq_info->dma_ring_addr = rxq->rx_ring_phys_addr;
		rxq_info->databuffer_size =
			(rte_pktmbuf_data_room_size(rxq->mp) -
				RTE_PKTMBUF_HEADROOM);
	}
}

/* It configures VSI queues to co-work with Linux PF host */
static int
i40evf_configure_vsi_queues(struct rte_eth_dev *dev)
{
	struct i40e_vf *vf = I40EVF_DEV_PRIVATE_TO_VF(dev->data->dev_private);
	struct i40e_rx_queue **rxq =
		(struct i40e_rx_queue **)dev->data->rx_queues;
	struct i40e_tx_queue **txq =
		(struct i40e_tx_queue **)dev->data->tx_queues;
	struct i40e_virtchnl_vsi_queue_config_info *vc_vqci;
	struct i40e_virtchnl_queue_pair_info *vc_qpi;
	struct vf_cmd_info args;
	uint16_t i, nb_qp = vf->num_queue_pairs;
	const uint32_t size =
		I40E_VIRTCHNL_CONFIG_VSI_QUEUES_SIZE(vc_vqci, nb_qp);
	uint8_t buff[size];
	int ret;

	memset(buff, 0, sizeof(buff));
	vc_vqci = (struct i40e_virtchnl_vsi_queue_config_info *)buff;
	vc_vqci->vsi_id = vf->vsi_res->vsi_id;
	vc_vqci->num_queue_pairs = nb_qp;

	for (i = 0, vc_qpi = vc_vqci->qpair; i < nb_qp; i++, vc_qpi++) {
		i40evf_fill_virtchnl_vsi_txq_info(&vc_qpi->txq,
			vc_vqci->vsi_id, i, dev->data->nb_tx_queues, txq[i]);
		i40evf_fill_virtchnl_vsi_rxq_info(&vc_qpi->rxq,
			vc_vqci->vsi_id, i, dev->data->nb_rx_queues,
					vf->max_pkt_len, rxq[i]);
	}
	memset(&args, 0, sizeof(args));
	args.ops = I40E_VIRTCHNL_OP_CONFIG_VSI_QUEUES;
	args.in_args = (uint8_t *)vc_vqci;
	args.in_args_size = size;
	args.out_buffer = cmd_result_buffer;
	args.out_size = I40E_AQ_BUF_SZ;
	ret = i40evf_execute_vf_cmd(dev, &args);
	if (ret)
		PMD_DRV_LOG(ERR, "Failed to execute command of "
			"I40E_VIRTCHNL_OP_CONFIG_VSI_QUEUES\n");

	return ret;
}

/* It configures VSI queues to co-work with DPDK PF host */
static int
i40evf_configure_vsi_queues_ext(struct rte_eth_dev *dev)
{
	struct i40e_vf *vf = I40EVF_DEV_PRIVATE_TO_VF(dev->data->dev_private);
	struct i40e_rx_queue **rxq =
		(struct i40e_rx_queue **)dev->data->rx_queues;
	struct i40e_tx_queue **txq =
		(struct i40e_tx_queue **)dev->data->tx_queues;
	struct i40e_virtchnl_vsi_queue_config_ext_info *vc_vqcei;
	struct i40e_virtchnl_queue_pair_ext_info *vc_qpei;
	struct vf_cmd_info args;
	uint16_t i, nb_qp = vf->num_queue_pairs;
	const uint32_t size =
		I40E_VIRTCHNL_CONFIG_VSI_QUEUES_SIZE(vc_vqcei, nb_qp);
	uint8_t buff[size];
	int ret;

	memset(buff, 0, sizeof(buff));
	vc_vqcei = (struct i40e_virtchnl_vsi_queue_config_ext_info *)buff;
	vc_vqcei->vsi_id = vf->vsi_res->vsi_id;
	vc_vqcei->num_queue_pairs = nb_qp;
	vc_qpei = vc_vqcei->qpair;
	for (i = 0; i < nb_qp; i++, vc_qpei++) {
		i40evf_fill_virtchnl_vsi_txq_info(&vc_qpei->txq,
			vc_vqcei->vsi_id, i, dev->data->nb_tx_queues, txq[i]);
		i40evf_fill_virtchnl_vsi_rxq_info(&vc_qpei->rxq,
			vc_vqcei->vsi_id, i, dev->data->nb_rx_queues,
					vf->max_pkt_len, rxq[i]);
		if (i < dev->data->nb_rx_queues)
			/*
			 * It adds extra info for configuring VSI queues, which
			 * is needed to enable the configurable crc stripping
			 * in VF.
			 */
			vc_qpei->rxq_ext.crcstrip =
				dev->data->dev_conf.rxmode.hw_strip_crc;
	}
	memset(&args, 0, sizeof(args));
	args.ops =
		(enum i40e_virtchnl_ops)I40E_VIRTCHNL_OP_CONFIG_VSI_QUEUES_EXT;
	args.in_args = (uint8_t *)vc_vqcei;
	args.in_args_size = size;
	args.out_buffer = cmd_result_buffer;
	args.out_size = I40E_AQ_BUF_SZ;
	ret = i40evf_execute_vf_cmd(dev, &args);
	if (ret)
		PMD_DRV_LOG(ERR, "Failed to execute command of "
			"I40E_VIRTCHNL_OP_CONFIG_VSI_QUEUES_EXT\n");

	return ret;
}

static int
i40evf_configure_queues(struct rte_eth_dev *dev)
{
	struct i40e_vf *vf = I40EVF_DEV_PRIVATE_TO_VF(dev->data->dev_private);

	if (vf->version_major == I40E_DPDK_VERSION_MAJOR)
		/* To support DPDK PF host */
		return i40evf_configure_vsi_queues_ext(dev);
	else
		/* To support Linux PF host */
		return i40evf_configure_vsi_queues(dev);
}

static int
i40evf_config_irq_map(struct rte_eth_dev *dev)
{
	struct i40e_vf *vf = I40EVF_DEV_PRIVATE_TO_VF(dev->data->dev_private);
	struct vf_cmd_info args;
	uint8_t cmd_buffer[sizeof(struct i40e_virtchnl_irq_map_info) + \
		sizeof(struct i40e_virtchnl_vector_map)];
	struct i40e_virtchnl_irq_map_info *map_info;
	struct rte_intr_handle *intr_handle = &dev->pci_dev->intr_handle;
	uint32_t vector_id;
	int i, err;

	if (rte_intr_allow_others(intr_handle)) {
		if (vf->version_major == I40E_DPDK_VERSION_MAJOR)
			vector_id = I40EVF_VSI_DEFAULT_MSIX_INTR;
		else
			vector_id = I40EVF_VSI_DEFAULT_MSIX_INTR_LNX;
	} else {
		vector_id = I40E_MISC_VEC_ID;
	}

	map_info = (struct i40e_virtchnl_irq_map_info *)cmd_buffer;
	map_info->num_vectors = 1;
	map_info->vecmap[0].rxitr_idx = I40E_QINT_RQCTL_MSIX_INDX_NOITR;
	map_info->vecmap[0].vsi_id = vf->vsi_res->vsi_id;
	/* Alway use default dynamic MSIX interrupt */
	map_info->vecmap[0].vector_id = vector_id;
	/* Don't map any tx queue */
	map_info->vecmap[0].txq_map = 0;
	map_info->vecmap[0].rxq_map = 0;
	for (i = 0; i < dev->data->nb_rx_queues; i++) {
		map_info->vecmap[0].rxq_map |= 1 << i;
		if (rte_intr_dp_is_en(intr_handle))
			intr_handle->intr_vec[i] = vector_id;
	}

	args.ops = I40E_VIRTCHNL_OP_CONFIG_IRQ_MAP;
	args.in_args = (u8 *)cmd_buffer;
	args.in_args_size = sizeof(cmd_buffer);
	args.out_buffer = cmd_result_buffer;
	args.out_size = I40E_AQ_BUF_SZ;
	err = i40evf_execute_vf_cmd(dev, &args);
	if (err)
		PMD_DRV_LOG(ERR, "fail to execute command OP_ENABLE_QUEUES");

	return err;
}

static int
i40evf_switch_queue(struct rte_eth_dev *dev, bool isrx, uint16_t qid,
				bool on)
{
	struct i40e_vf *vf = I40EVF_DEV_PRIVATE_TO_VF(dev->data->dev_private);
	struct i40e_virtchnl_queue_select queue_select;
	int err;
	struct vf_cmd_info args;
	memset(&queue_select, 0, sizeof(queue_select));
	queue_select.vsi_id = vf->vsi_res->vsi_id;

	if (isrx)
		queue_select.rx_queues |= 1 << qid;
	else
		queue_select.tx_queues |= 1 << qid;

	if (on)
		args.ops = I40E_VIRTCHNL_OP_ENABLE_QUEUES;
	else
		args.ops = I40E_VIRTCHNL_OP_DISABLE_QUEUES;
	args.in_args = (u8 *)&queue_select;
	args.in_args_size = sizeof(queue_select);
	args.out_buffer = cmd_result_buffer;
	args.out_size = I40E_AQ_BUF_SZ;
	err = i40evf_execute_vf_cmd(dev, &args);
	if (err)
		PMD_DRV_LOG(ERR, "fail to switch %s %u %s",
			    isrx ? "RX" : "TX", qid, on ? "on" : "off");

	return err;
}

static int
i40evf_start_queues(struct rte_eth_dev *dev)
{
	struct rte_eth_dev_data *dev_data = dev->data;
	int i;
	struct i40e_rx_queue *rxq;
	struct i40e_tx_queue *txq;

	for (i = 0; i < dev->data->nb_rx_queues; i++) {
		rxq = dev_data->rx_queues[i];
		if (rxq->rx_deferred_start)
			continue;
		if (i40evf_dev_rx_queue_start(dev, i) != 0) {
			PMD_DRV_LOG(ERR, "Fail to start queue %u", i);
			return -1;
		}
	}

	for (i = 0; i < dev->data->nb_tx_queues; i++) {
		txq = dev_data->tx_queues[i];
		if (txq->tx_deferred_start)
			continue;
		if (i40evf_dev_tx_queue_start(dev, i) != 0) {
			PMD_DRV_LOG(ERR, "Fail to start queue %u", i);
			return -1;
		}
	}

	return 0;
}

static int
i40evf_stop_queues(struct rte_eth_dev *dev)
{
	int i;

	/* Stop TX queues first */
	for (i = 0; i < dev->data->nb_tx_queues; i++) {
		if (i40evf_dev_tx_queue_stop(dev, i) != 0) {
			PMD_DRV_LOG(ERR, "Fail to stop queue %u", i);
			return -1;
		}
	}

	/* Then stop RX queues */
	for (i = 0; i < dev->data->nb_rx_queues; i++) {
		if (i40evf_dev_rx_queue_stop(dev, i) != 0) {
			PMD_DRV_LOG(ERR, "Fail to stop queue %u", i);
			return -1;
		}
	}

	return 0;
}

static void
i40evf_add_mac_addr(struct rte_eth_dev *dev,
		    struct ether_addr *addr,
		    __rte_unused uint32_t index,
		    __rte_unused uint32_t pool)
{
	struct i40e_virtchnl_ether_addr_list *list;
	struct i40e_vf *vf = I40EVF_DEV_PRIVATE_TO_VF(dev->data->dev_private);
	uint8_t cmd_buffer[sizeof(struct i40e_virtchnl_ether_addr_list) + \
			sizeof(struct i40e_virtchnl_ether_addr)];
	int err;
	struct vf_cmd_info args;

	if (i40e_validate_mac_addr(addr->addr_bytes) != I40E_SUCCESS) {
		PMD_DRV_LOG(ERR, "Invalid mac:%x:%x:%x:%x:%x:%x",
			    addr->addr_bytes[0], addr->addr_bytes[1],
			    addr->addr_bytes[2], addr->addr_bytes[3],
			    addr->addr_bytes[4], addr->addr_bytes[5]);
		return;
	}

	list = (struct i40e_virtchnl_ether_addr_list *)cmd_buffer;
	list->vsi_id = vf->vsi_res->vsi_id;
	list->num_elements = 1;
	(void)rte_memcpy(list->list[0].addr, addr->addr_bytes,
					sizeof(addr->addr_bytes));

	args.ops = I40E_VIRTCHNL_OP_ADD_ETHER_ADDRESS;
	args.in_args = cmd_buffer;
	args.in_args_size = sizeof(cmd_buffer);
	args.out_buffer = cmd_result_buffer;
	args.out_size = I40E_AQ_BUF_SZ;
	err = i40evf_execute_vf_cmd(dev, &args);
	if (err)
		PMD_DRV_LOG(ERR, "fail to execute command "
			    "OP_ADD_ETHER_ADDRESS");

	return;
}

static void
i40evf_del_mac_addr(struct rte_eth_dev *dev, uint32_t index)
{
	struct i40e_virtchnl_ether_addr_list *list;
	struct i40e_vf *vf = I40EVF_DEV_PRIVATE_TO_VF(dev->data->dev_private);
	struct rte_eth_dev_data *data = dev->data;
	struct ether_addr *addr;
	uint8_t cmd_buffer[sizeof(struct i40e_virtchnl_ether_addr_list) + \
			sizeof(struct i40e_virtchnl_ether_addr)];
	int err;
	struct vf_cmd_info args;

	addr = &(data->mac_addrs[index]);

	if (i40e_validate_mac_addr(addr->addr_bytes) != I40E_SUCCESS) {
		PMD_DRV_LOG(ERR, "Invalid mac:%x-%x-%x-%x-%x-%x",
			    addr->addr_bytes[0], addr->addr_bytes[1],
			    addr->addr_bytes[2], addr->addr_bytes[3],
			    addr->addr_bytes[4], addr->addr_bytes[5]);
		return;
	}

	list = (struct i40e_virtchnl_ether_addr_list *)cmd_buffer;
	list->vsi_id = vf->vsi_res->vsi_id;
	list->num_elements = 1;
	(void)rte_memcpy(list->list[0].addr, addr->addr_bytes,
			sizeof(addr->addr_bytes));

	args.ops = I40E_VIRTCHNL_OP_DEL_ETHER_ADDRESS;
	args.in_args = cmd_buffer;
	args.in_args_size = sizeof(cmd_buffer);
	args.out_buffer = cmd_result_buffer;
	args.out_size = I40E_AQ_BUF_SZ;
	err = i40evf_execute_vf_cmd(dev, &args);
	if (err)
		PMD_DRV_LOG(ERR, "fail to execute command "
			    "OP_DEL_ETHER_ADDRESS");
	return;
}

static int
i40evf_update_stats(struct rte_eth_dev *dev, struct i40e_eth_stats **pstats)
{
	struct i40e_vf *vf = I40EVF_DEV_PRIVATE_TO_VF(dev->data->dev_private);
	struct i40e_virtchnl_queue_select q_stats;
	int err;
	struct vf_cmd_info args;

	memset(&q_stats, 0, sizeof(q_stats));
	q_stats.vsi_id = vf->vsi_res->vsi_id;
	args.ops = I40E_VIRTCHNL_OP_GET_STATS;
	args.in_args = (u8 *)&q_stats;
	args.in_args_size = sizeof(q_stats);
	args.out_buffer = cmd_result_buffer;
	args.out_size = I40E_AQ_BUF_SZ;

	err = i40evf_execute_vf_cmd(dev, &args);
	if (err) {
		PMD_DRV_LOG(ERR, "fail to execute command OP_GET_STATS");
		*pstats = NULL;
		return err;
	}
	*pstats = (struct i40e_eth_stats *)args.out_buffer;
	return 0;
}

static int
i40evf_get_statics(struct rte_eth_dev *dev, struct rte_eth_stats *stats)
{
	int ret;
	struct i40e_eth_stats *pstats = NULL;

	ret = i40evf_update_stats(dev, &pstats);
	if (ret != 0)
		return 0;

	stats->ipackets = pstats->rx_unicast + pstats->rx_multicast +
						pstats->rx_broadcast;
	stats->opackets = pstats->tx_broadcast + pstats->tx_multicast +
						pstats->tx_unicast;
	stats->ierrors = pstats->rx_discards;
	stats->oerrors = pstats->tx_errors + pstats->tx_discards;
	stats->ibytes = pstats->rx_bytes;
	stats->obytes = pstats->tx_bytes;

	return 0;
}

static void
i40evf_dev_xstats_reset(struct rte_eth_dev *dev)
{
	struct i40e_vf *vf = I40EVF_DEV_PRIVATE_TO_VF(dev->data->dev_private);
	struct i40e_eth_stats *pstats = NULL;

	/* read stat values to clear hardware registers */
	i40evf_update_stats(dev, &pstats);

	/* set stats offset base on current values */
	vf->vsi.eth_stats_offset = vf->vsi.eth_stats;
}

static int i40evf_dev_xstats_get(struct rte_eth_dev *dev,
				 struct rte_eth_xstats *xstats, unsigned n)
{
	int ret;
	unsigned i;
	struct i40e_eth_stats *pstats = NULL;

	if (n < I40EVF_NB_XSTATS)
		return I40EVF_NB_XSTATS;

	ret = i40evf_update_stats(dev, &pstats);
	if (ret != 0)
		return 0;

	if (!xstats)
		return 0;

	/* loop over xstats array and values from pstats */
	for (i = 0; i < I40EVF_NB_XSTATS; i++) {
		snprintf(xstats[i].name, sizeof(xstats[i].name),
			 "%s", rte_i40evf_stats_strings[i].name);
		xstats[i].value = *(uint64_t *)(((char *)pstats) +
			rte_i40evf_stats_strings[i].offset);
	}

	return I40EVF_NB_XSTATS;
}

static int
i40evf_add_vlan(struct rte_eth_dev *dev, uint16_t vlanid)
{
	struct i40e_vf *vf = I40EVF_DEV_PRIVATE_TO_VF(dev->data->dev_private);
	struct i40e_virtchnl_vlan_filter_list *vlan_list;
	uint8_t cmd_buffer[sizeof(struct i40e_virtchnl_vlan_filter_list) +
							sizeof(uint16_t)];
	int err;
	struct vf_cmd_info args;

	vlan_list = (struct i40e_virtchnl_vlan_filter_list *)cmd_buffer;
	vlan_list->vsi_id = vf->vsi_res->vsi_id;
	vlan_list->num_elements = 1;
	vlan_list->vlan_id[0] = vlanid;

	args.ops = I40E_VIRTCHNL_OP_ADD_VLAN;
	args.in_args = (u8 *)&cmd_buffer;
	args.in_args_size = sizeof(cmd_buffer);
	args.out_buffer = cmd_result_buffer;
	args.out_size = I40E_AQ_BUF_SZ;
	err = i40evf_execute_vf_cmd(dev, &args);
	if (err)
		PMD_DRV_LOG(ERR, "fail to execute command OP_ADD_VLAN");

	return err;
}

static int
i40evf_del_vlan(struct rte_eth_dev *dev, uint16_t vlanid)
{
	struct i40e_vf *vf = I40EVF_DEV_PRIVATE_TO_VF(dev->data->dev_private);
	struct i40e_virtchnl_vlan_filter_list *vlan_list;
	uint8_t cmd_buffer[sizeof(struct i40e_virtchnl_vlan_filter_list) +
							sizeof(uint16_t)];
	int err;
	struct vf_cmd_info args;

	vlan_list = (struct i40e_virtchnl_vlan_filter_list *)cmd_buffer;
	vlan_list->vsi_id = vf->vsi_res->vsi_id;
	vlan_list->num_elements = 1;
	vlan_list->vlan_id[0] = vlanid;

	args.ops = I40E_VIRTCHNL_OP_DEL_VLAN;
	args.in_args = (u8 *)&cmd_buffer;
	args.in_args_size = sizeof(cmd_buffer);
	args.out_buffer = cmd_result_buffer;
	args.out_size = I40E_AQ_BUF_SZ;
	err = i40evf_execute_vf_cmd(dev, &args);
	if (err)
		PMD_DRV_LOG(ERR, "fail to execute command OP_DEL_VLAN");

	return err;
}

static int
i40evf_get_link_status(struct rte_eth_dev *dev, struct rte_eth_link *link)
{
	int err;
	struct vf_cmd_info args;
	struct rte_eth_link *new_link;

	args.ops = (enum i40e_virtchnl_ops)I40E_VIRTCHNL_OP_GET_LINK_STAT;
	args.in_args = NULL;
	args.in_args_size = 0;
	args.out_buffer = cmd_result_buffer;
	args.out_size = I40E_AQ_BUF_SZ;
	err = i40evf_execute_vf_cmd(dev, &args);
	if (err) {
		PMD_DRV_LOG(ERR, "fail to execute command OP_GET_LINK_STAT");
		return err;
	}

	new_link = (struct rte_eth_link *)args.out_buffer;
	(void)rte_memcpy(link, new_link, sizeof(*link));

	return 0;
}

static const struct rte_pci_id pci_id_i40evf_map[] = {
#define RTE_PCI_DEV_ID_DECL_I40EVF(vend, dev) {RTE_PCI_DEVICE(vend, dev)},
#include "rte_pci_dev_ids.h"
{ .vendor_id = 0, /* sentinel */ },
};

static inline int
i40evf_dev_atomic_write_link_status(struct rte_eth_dev *dev,
				    struct rte_eth_link *link)
{
	struct rte_eth_link *dst = &(dev->data->dev_link);
	struct rte_eth_link *src = link;

	if (rte_atomic64_cmpset((uint64_t *)dst, *(uint64_t *)dst,
					*(uint64_t *)src) == 0)
		return -1;

	return 0;
}

static int
i40evf_reset_vf(struct i40e_hw *hw)
{
	int i, reset;

	if (i40e_vf_reset(hw) != I40E_SUCCESS) {
		PMD_INIT_LOG(ERR, "Reset VF NIC failed");
		return -1;
	}
	/**
	  * After issuing vf reset command to pf, pf won't necessarily
	  * reset vf, it depends on what state it exactly is. If it's not
	  * initialized yet, it won't have vf reset since it's in a certain
	  * state. If not, it will try to reset. Even vf is reset, pf will
	  * set I40E_VFGEN_RSTAT to COMPLETE first, then wait 10ms and set
	  * it to ACTIVE. In this duration, vf may not catch the moment that
	  * COMPLETE is set. So, for vf, we'll try to wait a long time.
	  */
	rte_delay_ms(200);

	for (i = 0; i < MAX_RESET_WAIT_CNT; i++) {
		reset = rd32(hw, I40E_VFGEN_RSTAT) &
			I40E_VFGEN_RSTAT_VFR_STATE_MASK;
		reset = reset >> I40E_VFGEN_RSTAT_VFR_STATE_SHIFT;
		if (I40E_VFR_COMPLETED == reset || I40E_VFR_VFACTIVE == reset)
			break;
		else
			rte_delay_ms(50);
	}

	if (i >= MAX_RESET_WAIT_CNT) {
		PMD_INIT_LOG(ERR, "Reset VF NIC failed");
		return -1;
	}

	return 0;
}

static int
i40evf_init_vf(struct rte_eth_dev *dev)
{
	int i, err, bufsz;
	struct i40e_hw *hw = I40E_DEV_PRIVATE_TO_HW(dev->data->dev_private);
	struct i40e_vf *vf = I40EVF_DEV_PRIVATE_TO_VF(dev->data->dev_private);
	struct ether_addr *p_mac_addr;

	vf->adapter = I40E_DEV_PRIVATE_TO_ADAPTER(dev->data->dev_private);
	vf->dev_data = dev->data;
	err = i40e_set_mac_type(hw);
	if (err) {
		PMD_INIT_LOG(ERR, "set_mac_type failed: %d", err);
		goto err;
	}

	i40e_init_adminq_parameter(hw);
	err = i40e_init_adminq(hw);
	if (err) {
		PMD_INIT_LOG(ERR, "init_adminq failed: %d", err);
		goto err;
	}


	/* Reset VF and wait until it's complete */
	if (i40evf_reset_vf(hw)) {
		PMD_INIT_LOG(ERR, "reset NIC failed");
		goto err_aq;
	}

	/* VF reset, shutdown admin queue and initialize again */
	if (i40e_shutdown_adminq(hw) != I40E_SUCCESS) {
		PMD_INIT_LOG(ERR, "i40e_shutdown_adminq failed");
		return -1;
	}

	i40e_init_adminq_parameter(hw);
	if (i40e_init_adminq(hw) != I40E_SUCCESS) {
		PMD_INIT_LOG(ERR, "init_adminq failed");
		return -1;
	}
	if (i40evf_check_api_version(dev) != 0) {
		PMD_INIT_LOG(ERR, "check_api version failed");
		goto err_aq;
	}
	bufsz = sizeof(struct i40e_virtchnl_vf_resource) +
		(I40E_MAX_VF_VSI * sizeof(struct i40e_virtchnl_vsi_resource));
	vf->vf_res = rte_zmalloc("vf_res", bufsz, 0);
	if (!vf->vf_res) {
		PMD_INIT_LOG(ERR, "unable to allocate vf_res memory");
			goto err_aq;
	}

	if (i40evf_get_vf_resource(dev) != 0) {
		PMD_INIT_LOG(ERR, "i40evf_get_vf_config failed");
		goto err_alloc;
	}

	/* got VF config message back from PF, now we can parse it */
	for (i = 0; i < vf->vf_res->num_vsis; i++) {
		if (vf->vf_res->vsi_res[i].vsi_type == I40E_VSI_SRIOV)
			vf->vsi_res = &vf->vf_res->vsi_res[i];
	}

	if (!vf->vsi_res) {
		PMD_INIT_LOG(ERR, "no LAN VSI found");
		goto err_alloc;
	}

	if (hw->mac.type == I40E_MAC_X722_VF)
		vf->flags = I40E_FLAG_RSS_AQ_CAPABLE;
	vf->vsi.vsi_id = vf->vsi_res->vsi_id;
	vf->vsi.type = vf->vsi_res->vsi_type;
	vf->vsi.nb_qps = vf->vsi_res->num_queue_pairs;
	vf->vsi.adapter = I40E_DEV_PRIVATE_TO_ADAPTER(dev->data->dev_private);

	/* Store the MAC address configured by host, or generate random one */
	p_mac_addr = (struct ether_addr *)(vf->vsi_res->default_mac_addr);
	if (is_valid_assigned_ether_addr(p_mac_addr)) /* Configured by host */
		ether_addr_copy(p_mac_addr, (struct ether_addr *)hw->mac.addr);
	else
		eth_random_addr(hw->mac.addr); /* Generate a random one */

	return 0;

err_alloc:
	rte_free(vf->vf_res);
err_aq:
	i40e_shutdown_adminq(hw); /* ignore error */
err:
	return -1;
}

static int
i40evf_uninit_vf(struct rte_eth_dev *dev)
{
	struct i40e_vf *vf = I40EVF_DEV_PRIVATE_TO_VF(dev->data->dev_private);
	struct i40e_hw *hw = I40E_DEV_PRIVATE_TO_HW(dev->data->dev_private);

	PMD_INIT_FUNC_TRACE();

	if (hw->adapter_stopped == 0)
		i40evf_dev_close(dev);
	rte_free(vf->vf_res);
	vf->vf_res = NULL;

	return 0;
}

static int
i40evf_dev_init(struct rte_eth_dev *eth_dev)
{
	struct i40e_hw *hw = I40E_DEV_PRIVATE_TO_HW(\
			eth_dev->data->dev_private);

	PMD_INIT_FUNC_TRACE();

	/* assign ops func pointer */
	eth_dev->dev_ops = &i40evf_eth_dev_ops;
	eth_dev->rx_pkt_burst = &i40e_recv_pkts;
	eth_dev->tx_pkt_burst = &i40e_xmit_pkts;

	/*
	 * For secondary processes, we don't initialise any further as primary
	 * has already done this work.
	 */
	if (rte_eal_process_type() != RTE_PROC_PRIMARY){
		i40e_set_rx_function(eth_dev);
		i40e_set_tx_function(eth_dev);
		return 0;
	}

	rte_eth_copy_pci_info(eth_dev, eth_dev->pci_dev);

	hw->vendor_id = eth_dev->pci_dev->id.vendor_id;
	hw->device_id = eth_dev->pci_dev->id.device_id;
	hw->subsystem_vendor_id = eth_dev->pci_dev->id.subsystem_vendor_id;
	hw->subsystem_device_id = eth_dev->pci_dev->id.subsystem_device_id;
	hw->bus.device = eth_dev->pci_dev->addr.devid;
	hw->bus.func = eth_dev->pci_dev->addr.function;
	hw->hw_addr = (void *)eth_dev->pci_dev->mem_resource[0].addr;
	hw->adapter_stopped = 0;

	if(i40evf_init_vf(eth_dev) != 0) {
		PMD_INIT_LOG(ERR, "Init vf failed");
		return -1;
	}

	/* allocate memory for mac addr storage */
	eth_dev->data->mac_addrs = rte_zmalloc("i40evf_mac",
					ETHER_ADDR_LEN * I40E_NUM_MACADDR_MAX,
					0);
	if (eth_dev->data->mac_addrs == NULL) {
		PMD_INIT_LOG(ERR, "Failed to allocate %d bytes needed to"
				" store MAC addresses",
				ETHER_ADDR_LEN * I40E_NUM_MACADDR_MAX);
		return -ENOMEM;
	}
	ether_addr_copy((struct ether_addr *)hw->mac.addr,
			&eth_dev->data->mac_addrs[0]);

	return 0;
}

static int
i40evf_dev_uninit(struct rte_eth_dev *eth_dev)
{
	PMD_INIT_FUNC_TRACE();

	if (rte_eal_process_type() != RTE_PROC_PRIMARY)
		return -EPERM;

	eth_dev->dev_ops = NULL;
	eth_dev->rx_pkt_burst = NULL;
	eth_dev->tx_pkt_burst = NULL;

	if (i40evf_uninit_vf(eth_dev) != 0) {
		PMD_INIT_LOG(ERR, "i40evf_uninit_vf failed");
		return -1;
	}

	rte_free(eth_dev->data->mac_addrs);
	eth_dev->data->mac_addrs = NULL;

	return 0;
}
/*
 * virtual function driver struct
 */
static struct eth_driver rte_i40evf_pmd = {
	.pci_drv = {
		.name = "rte_i40evf_pmd",
		.id_table = pci_id_i40evf_map,
		.drv_flags = RTE_PCI_DRV_NEED_MAPPING | RTE_PCI_DRV_DETACHABLE,
	},
	.eth_dev_init = i40evf_dev_init,
	.eth_dev_uninit = i40evf_dev_uninit,
	.dev_private_size = sizeof(struct i40e_adapter),
};

/*
 * VF Driver initialization routine.
 * Invoked one at EAL init time.
 * Register itself as the [Virtual Poll Mode] Driver of PCI Fortville devices.
 */
static int
rte_i40evf_pmd_init(const char *name __rte_unused,
		    const char *params __rte_unused)
{
	PMD_INIT_FUNC_TRACE();

	rte_eth_driver_register(&rte_i40evf_pmd);

	return 0;
}

static struct rte_driver rte_i40evf_driver = {
	.type = PMD_PDEV,
	.init = rte_i40evf_pmd_init,
};

PMD_REGISTER_DRIVER(rte_i40evf_driver);

static int
i40evf_dev_configure(struct rte_eth_dev *dev)
{
	struct i40e_adapter *ad =
		I40E_DEV_PRIVATE_TO_ADAPTER(dev->data->dev_private);

	/* Initialize to TRUE. If any of Rx queues doesn't meet the bulk
	 * allocation or vector Rx preconditions we will reset it.
	 */
	ad->rx_bulk_alloc_allowed = true;
	ad->rx_vec_allowed = true;
	ad->tx_simple_allowed = true;
	ad->tx_vec_allowed = true;

	return i40evf_init_vlan(dev);
}

static int
i40evf_init_vlan(struct rte_eth_dev *dev)
{
	struct rte_eth_dev_data *data = dev->data;
	int ret;

	/* Apply vlan offload setting */
	i40evf_vlan_offload_set(dev, ETH_VLAN_STRIP_MASK);

	/* Apply pvid setting */
	ret = i40evf_vlan_pvid_set(dev, data->dev_conf.txmode.pvid,
				data->dev_conf.txmode.hw_vlan_insert_pvid);
	return ret;
}

static void
i40evf_vlan_offload_set(struct rte_eth_dev *dev, int mask)
{
	bool enable_vlan_strip = 0;
	struct rte_eth_conf *dev_conf = &dev->data->dev_conf;
	struct i40e_vf *vf = I40EVF_DEV_PRIVATE_TO_VF(dev->data->dev_private);

	/* Linux pf host doesn't support vlan offload yet */
	if (vf->version_major == I40E_DPDK_VERSION_MAJOR) {
		/* Vlan stripping setting */
		if (mask & ETH_VLAN_STRIP_MASK) {
			/* Enable or disable VLAN stripping */
			if (dev_conf->rxmode.hw_vlan_strip)
				enable_vlan_strip = 1;
			else
				enable_vlan_strip = 0;

			i40evf_config_vlan_offload(dev, enable_vlan_strip);
		}
	}
}

static int
i40evf_vlan_pvid_set(struct rte_eth_dev *dev, uint16_t pvid, int on)
{
	struct rte_eth_conf *dev_conf = &dev->data->dev_conf;
	struct i40e_vsi_vlan_pvid_info info;
	struct i40e_vf *vf = I40EVF_DEV_PRIVATE_TO_VF(dev->data->dev_private);

	memset(&info, 0, sizeof(info));
	info.on = on;

	/* Linux pf host don't support vlan offload yet */
	if (vf->version_major == I40E_DPDK_VERSION_MAJOR) {
		if (info.on)
			info.config.pvid = pvid;
		else {
			info.config.reject.tagged =
				dev_conf->txmode.hw_vlan_reject_tagged;
			info.config.reject.untagged =
				dev_conf->txmode.hw_vlan_reject_untagged;
		}
		return i40evf_config_vlan_pvid(dev, &info);
	}

	return 0;
}

static int
i40evf_dev_rx_queue_start(struct rte_eth_dev *dev, uint16_t rx_queue_id)
{
	struct i40e_rx_queue *rxq;
	int err = 0;
	struct i40e_hw *hw = I40E_DEV_PRIVATE_TO_HW(dev->data->dev_private);

	PMD_INIT_FUNC_TRACE();

	if (rx_queue_id < dev->data->nb_rx_queues) {
		rxq = dev->data->rx_queues[rx_queue_id];

		err = i40e_alloc_rx_queue_mbufs(rxq);
		if (err) {
			PMD_DRV_LOG(ERR, "Failed to allocate RX queue mbuf");
			return err;
		}

		rte_wmb();

		/* Init the RX tail register. */
		I40E_PCI_REG_WRITE(rxq->qrx_tail, rxq->nb_rx_desc - 1);
		I40EVF_WRITE_FLUSH(hw);

		/* Ready to switch the queue on */
		err = i40evf_switch_queue(dev, TRUE, rx_queue_id, TRUE);

		if (err)
			PMD_DRV_LOG(ERR, "Failed to switch RX queue %u on",
				    rx_queue_id);
		else
			dev->data->rx_queue_state[rx_queue_id] = RTE_ETH_QUEUE_STATE_STARTED;
	}

	return err;
}

static int
i40evf_dev_rx_queue_stop(struct rte_eth_dev *dev, uint16_t rx_queue_id)
{
	struct i40e_rx_queue *rxq;
	int err;

	if (rx_queue_id < dev->data->nb_rx_queues) {
		rxq = dev->data->rx_queues[rx_queue_id];

		err = i40evf_switch_queue(dev, TRUE, rx_queue_id, FALSE);

		if (err) {
			PMD_DRV_LOG(ERR, "Failed to switch RX queue %u off",
				    rx_queue_id);
			return err;
		}

		i40e_rx_queue_release_mbufs(rxq);
		i40e_reset_rx_queue(rxq);
		dev->data->rx_queue_state[rx_queue_id] = RTE_ETH_QUEUE_STATE_STOPPED;
	}

	return 0;
}

static int
i40evf_dev_tx_queue_start(struct rte_eth_dev *dev, uint16_t tx_queue_id)
{
	int err = 0;

	PMD_INIT_FUNC_TRACE();

	if (tx_queue_id < dev->data->nb_tx_queues) {

		/* Ready to switch the queue on */
		err = i40evf_switch_queue(dev, FALSE, tx_queue_id, TRUE);

		if (err)
			PMD_DRV_LOG(ERR, "Failed to switch TX queue %u on",
				    tx_queue_id);
		else
			dev->data->tx_queue_state[tx_queue_id] = RTE_ETH_QUEUE_STATE_STARTED;
	}

	return err;
}

static int
i40evf_dev_tx_queue_stop(struct rte_eth_dev *dev, uint16_t tx_queue_id)
{
	struct i40e_tx_queue *txq;
	int err;

	if (tx_queue_id < dev->data->nb_tx_queues) {
		txq = dev->data->tx_queues[tx_queue_id];

		err = i40evf_switch_queue(dev, FALSE, tx_queue_id, FALSE);

		if (err) {
			PMD_DRV_LOG(ERR, "Failed to switch TX queue %u off",
				    tx_queue_id);
			return err;
		}

		i40e_tx_queue_release_mbufs(txq);
		i40e_reset_tx_queue(txq);
		dev->data->tx_queue_state[tx_queue_id] = RTE_ETH_QUEUE_STATE_STOPPED;
	}

	return 0;
}

static int
i40evf_vlan_filter_set(struct rte_eth_dev *dev, uint16_t vlan_id, int on)
{
	int ret;

	if (on)
		ret = i40evf_add_vlan(dev, vlan_id);
	else
		ret = i40evf_del_vlan(dev,vlan_id);

	return ret;
}

static int
i40evf_rxq_init(struct rte_eth_dev *dev, struct i40e_rx_queue *rxq)
{
	struct i40e_hw *hw = I40E_DEV_PRIVATE_TO_HW(dev->data->dev_private);
	struct rte_eth_dev_data *dev_data = dev->data;
	struct rte_pktmbuf_pool_private *mbp_priv;
	uint16_t buf_size, len;

	rxq->qrx_tail = hw->hw_addr + I40E_QRX_TAIL1(rxq->queue_id);
	I40E_PCI_REG_WRITE(rxq->qrx_tail, rxq->nb_rx_desc - 1);
	I40EVF_WRITE_FLUSH(hw);

	/* Calculate the maximum packet length allowed */
	mbp_priv = rte_mempool_get_priv(rxq->mp);
	buf_size = (uint16_t)(mbp_priv->mbuf_data_room_size -
					RTE_PKTMBUF_HEADROOM);
	rxq->hs_mode = i40e_header_split_none;
	rxq->rx_hdr_len = 0;
	rxq->rx_buf_len = RTE_ALIGN(buf_size, (1 << I40E_RXQ_CTX_DBUFF_SHIFT));
	len = rxq->rx_buf_len * I40E_MAX_CHAINED_RX_BUFFERS;
	rxq->max_pkt_len = RTE_MIN(len,
		dev_data->dev_conf.rxmode.max_rx_pkt_len);

	/**
	 * Check if the jumbo frame and maximum packet length are set correctly
	 */
	if (dev_data->dev_conf.rxmode.jumbo_frame == 1) {
		if (rxq->max_pkt_len <= ETHER_MAX_LEN ||
		    rxq->max_pkt_len > I40E_FRAME_SIZE_MAX) {
			PMD_DRV_LOG(ERR, "maximum packet length must be "
				"larger than %u and smaller than %u, as jumbo "
				"frame is enabled", (uint32_t)ETHER_MAX_LEN,
					(uint32_t)I40E_FRAME_SIZE_MAX);
			return I40E_ERR_CONFIG;
		}
	} else {
		if (rxq->max_pkt_len < ETHER_MIN_LEN ||
		    rxq->max_pkt_len > ETHER_MAX_LEN) {
			PMD_DRV_LOG(ERR, "maximum packet length must be "
				"larger than %u and smaller than %u, as jumbo "
				"frame is disabled", (uint32_t)ETHER_MIN_LEN,
						(uint32_t)ETHER_MAX_LEN);
			return I40E_ERR_CONFIG;
		}
	}

	if (dev_data->dev_conf.rxmode.enable_scatter ||
	    (rxq->max_pkt_len + 2 * I40E_VLAN_TAG_SIZE) > buf_size) {
		dev_data->scattered_rx = 1;
	}

	return 0;
}

static int
i40evf_rx_init(struct rte_eth_dev *dev)
{
	struct i40e_vf *vf = I40EVF_DEV_PRIVATE_TO_VF(dev->data->dev_private);
	uint16_t i;
	int ret = I40E_SUCCESS;
	struct i40e_rx_queue **rxq =
		(struct i40e_rx_queue **)dev->data->rx_queues;

	i40evf_config_rss(vf);
	for (i = 0; i < dev->data->nb_rx_queues; i++) {
		if (!rxq[i] || !rxq[i]->q_set)
			continue;
		ret = i40evf_rxq_init(dev, rxq[i]);
		if (ret != I40E_SUCCESS)
			break;
	}
	if (ret == I40E_SUCCESS)
		i40e_set_rx_function(dev);

	return ret;
}

static void
i40evf_tx_init(struct rte_eth_dev *dev)
{
	uint16_t i;
	struct i40e_tx_queue **txq =
		(struct i40e_tx_queue **)dev->data->tx_queues;
	struct i40e_hw *hw = I40E_DEV_PRIVATE_TO_HW(dev->data->dev_private);

	for (i = 0; i < dev->data->nb_tx_queues; i++)
		txq[i]->qtx_tail = hw->hw_addr + I40E_QTX_TAIL1(i);

	i40e_set_tx_function(dev);
}

static inline void
i40evf_enable_queues_intr(struct rte_eth_dev *dev)
{
	struct i40e_vf *vf = I40EVF_DEV_PRIVATE_TO_VF(dev->data->dev_private);
	struct i40e_hw *hw = I40E_DEV_PRIVATE_TO_HW(dev->data->dev_private);
	struct rte_intr_handle *intr_handle = &dev->pci_dev->intr_handle;

	if (!rte_intr_allow_others(intr_handle)) {
		I40E_WRITE_REG(hw,
			       I40E_VFINT_DYN_CTL01,
			       I40E_VFINT_DYN_CTL01_INTENA_MASK |
			       I40E_VFINT_DYN_CTL01_CLEARPBA_MASK);
		I40EVF_WRITE_FLUSH(hw);
		return;
	}

	if (vf->version_major == I40E_DPDK_VERSION_MAJOR)
		/* To support DPDK PF host */
		I40E_WRITE_REG(hw,
			I40E_VFINT_DYN_CTLN1(I40EVF_VSI_DEFAULT_MSIX_INTR - 1),
			I40E_VFINT_DYN_CTLN1_INTENA_MASK |
			I40E_VFINT_DYN_CTLN_CLEARPBA_MASK);
	else
		/* To support Linux PF host */
		I40E_WRITE_REG(hw, I40E_VFINT_DYN_CTL01,
				I40E_VFINT_DYN_CTL01_INTENA_MASK |
				I40E_VFINT_DYN_CTL01_CLEARPBA_MASK);

	I40EVF_WRITE_FLUSH(hw);
}

static inline void
i40evf_disable_queues_intr(struct rte_eth_dev *dev)
{
	struct i40e_vf *vf = I40EVF_DEV_PRIVATE_TO_VF(dev->data->dev_private);
	struct i40e_hw *hw = I40E_DEV_PRIVATE_TO_HW(dev->data->dev_private);
	struct rte_intr_handle *intr_handle = &dev->pci_dev->intr_handle;

	if (!rte_intr_allow_others(intr_handle)) {
		I40E_WRITE_REG(hw, I40E_VFINT_DYN_CTL01, 0);
		I40EVF_WRITE_FLUSH(hw);
		return;
	}

	if (vf->version_major == I40E_DPDK_VERSION_MAJOR)
		I40E_WRITE_REG(hw,
			       I40E_VFINT_DYN_CTLN1(I40EVF_VSI_DEFAULT_MSIX_INTR
						    - 1),
			       0);
	else
		I40E_WRITE_REG(hw, I40E_VFINT_DYN_CTL01, 0);

	I40EVF_WRITE_FLUSH(hw);
}

static int
i40evf_dev_rx_queue_intr_enable(struct rte_eth_dev *dev, uint16_t queue_id)
{
	struct rte_intr_handle *intr_handle = &dev->pci_dev->intr_handle;
	struct i40e_hw *hw = I40E_DEV_PRIVATE_TO_HW(dev->data->dev_private);
	uint16_t interval =
		i40e_calc_itr_interval(RTE_LIBRTE_I40E_ITR_INTERVAL);
	uint16_t msix_intr;

	msix_intr = intr_handle->intr_vec[queue_id];
	if (msix_intr == I40E_MISC_VEC_ID)
		I40E_WRITE_REG(hw, I40E_VFINT_DYN_CTL01,
			       I40E_VFINT_DYN_CTL01_INTENA_MASK |
			       I40E_VFINT_DYN_CTL01_CLEARPBA_MASK |
			       (0 << I40E_VFINT_DYN_CTL01_ITR_INDX_SHIFT) |
			       (interval <<
				I40E_VFINT_DYN_CTL01_INTERVAL_SHIFT));
	else
		I40E_WRITE_REG(hw,
			       I40E_VFINT_DYN_CTLN1(msix_intr -
						    I40E_RX_VEC_START),
			       I40E_VFINT_DYN_CTLN1_INTENA_MASK |
			       I40E_VFINT_DYN_CTLN1_CLEARPBA_MASK |
			       (0 << I40E_VFINT_DYN_CTLN1_ITR_INDX_SHIFT) |
			       (interval <<
				I40E_VFINT_DYN_CTLN1_INTERVAL_SHIFT));

	I40EVF_WRITE_FLUSH(hw);

	rte_intr_enable(&dev->pci_dev->intr_handle);

	return 0;
}

static int
i40evf_dev_rx_queue_intr_disable(struct rte_eth_dev *dev, uint16_t queue_id)
{
	struct rte_intr_handle *intr_handle = &dev->pci_dev->intr_handle;
	struct i40e_hw *hw = I40E_DEV_PRIVATE_TO_HW(dev->data->dev_private);
	uint16_t msix_intr;

	msix_intr = intr_handle->intr_vec[queue_id];
	if (msix_intr == I40E_MISC_VEC_ID)
		I40E_WRITE_REG(hw, I40E_VFINT_DYN_CTL01, 0);
	else
		I40E_WRITE_REG(hw,
			       I40E_VFINT_DYN_CTLN1(msix_intr -
						    I40E_RX_VEC_START),
			       0);

	I40EVF_WRITE_FLUSH(hw);

	return 0;
}

static void
i40evf_add_del_all_mac_addr(struct rte_eth_dev *dev, bool add)
{
	struct i40e_virtchnl_ether_addr_list *list;
	struct i40e_vf *vf = I40EVF_DEV_PRIVATE_TO_VF(dev->data->dev_private);
	int err, i, j;
	int next_begin = 0;
	int begin = 0;
	uint32_t len;
	struct ether_addr *addr;
	struct vf_cmd_info args;

	do {
		j = 0;
		len = sizeof(struct i40e_virtchnl_ether_addr_list);
		for (i = begin; i < I40E_NUM_MACADDR_MAX; i++, next_begin++) {
			if (is_zero_ether_addr(&dev->data->mac_addrs[i]))
				continue;
			len += sizeof(struct i40e_virtchnl_ether_addr);
			if (len >= I40E_AQ_BUF_SZ) {
				next_begin = i + 1;
				break;
			}
		}

		list = rte_zmalloc("i40evf_del_mac_buffer", len, 0);

		for (i = begin; i < next_begin; i++) {
			addr = &dev->data->mac_addrs[i];
			if (is_zero_ether_addr(addr))
				continue;
			(void)rte_memcpy(list->list[j].addr, addr->addr_bytes,
					 sizeof(addr->addr_bytes));
			PMD_DRV_LOG(DEBUG, "add/rm mac:%x:%x:%x:%x:%x:%x",
				    addr->addr_bytes[0], addr->addr_bytes[1],
				    addr->addr_bytes[2], addr->addr_bytes[3],
				    addr->addr_bytes[4], addr->addr_bytes[5]);
			j++;
		}
		list->vsi_id = vf->vsi_res->vsi_id;
		list->num_elements = j;
		args.ops = add ? I40E_VIRTCHNL_OP_ADD_ETHER_ADDRESS :
			   I40E_VIRTCHNL_OP_DEL_ETHER_ADDRESS;
		args.in_args = (uint8_t *)list;
		args.in_args_size = len;
		args.out_buffer = cmd_result_buffer;
		args.out_size = I40E_AQ_BUF_SZ;
		err = i40evf_execute_vf_cmd(dev, &args);
		if (err)
			PMD_DRV_LOG(ERR, "fail to execute command %s",
				    add ? "OP_ADD_ETHER_ADDRESS" :
				    "OP_DEL_ETHER_ADDRESS");
		rte_free(list);
		begin = next_begin;
	} while (begin < I40E_NUM_MACADDR_MAX);
}

static int
i40evf_dev_start(struct rte_eth_dev *dev)
{
	struct i40e_vf *vf = I40EVF_DEV_PRIVATE_TO_VF(dev->data->dev_private);
	struct i40e_hw *hw = I40E_DEV_PRIVATE_TO_HW(dev->data->dev_private);
	struct rte_intr_handle *intr_handle = &dev->pci_dev->intr_handle;
	uint32_t intr_vector = 0;

	PMD_INIT_FUNC_TRACE();

	hw->adapter_stopped = 0;

	vf->max_pkt_len = dev->data->dev_conf.rxmode.max_rx_pkt_len;
	vf->num_queue_pairs = RTE_MAX(dev->data->nb_rx_queues,
					dev->data->nb_tx_queues);

	/* check and configure queue intr-vector mapping */
	if (dev->data->dev_conf.intr_conf.rxq != 0) {
		intr_vector = dev->data->nb_rx_queues;
		if (rte_intr_efd_enable(intr_handle, intr_vector))
			return -1;
	}

	if (rte_intr_dp_is_en(intr_handle) && !intr_handle->intr_vec) {
		intr_handle->intr_vec =
			rte_zmalloc("intr_vec",
				    dev->data->nb_rx_queues * sizeof(int), 0);
		if (!intr_handle->intr_vec) {
			PMD_INIT_LOG(ERR, "Failed to allocate %d rx_queues"
				     " intr_vec\n", dev->data->nb_rx_queues);
			return -ENOMEM;
		}
	}

	if (i40evf_rx_init(dev) != 0){
		PMD_DRV_LOG(ERR, "failed to do RX init");
		return -1;
	}

	i40evf_tx_init(dev);

	if (i40evf_configure_queues(dev) != 0) {
		PMD_DRV_LOG(ERR, "configure queues failed");
		goto err_queue;
	}
	if (i40evf_config_irq_map(dev)) {
		PMD_DRV_LOG(ERR, "config_irq_map failed");
		goto err_queue;
	}

	/* Set all mac addrs */
	i40evf_add_del_all_mac_addr(dev, TRUE);

	if (i40evf_start_queues(dev) != 0) {
		PMD_DRV_LOG(ERR, "enable queues failed");
		goto err_mac;
	}

	/* vf don't allow intr except for rxq intr */
	if (dev->data->dev_conf.intr_conf.rxq != 0)
		rte_intr_enable(intr_handle);

	i40evf_enable_queues_intr(dev);
	return 0;

err_mac:
	i40evf_add_del_all_mac_addr(dev, FALSE);
err_queue:
	return -1;
}

static void
i40evf_dev_stop(struct rte_eth_dev *dev)
{
	struct rte_intr_handle *intr_handle = &dev->pci_dev->intr_handle;

	PMD_INIT_FUNC_TRACE();

	i40evf_stop_queues(dev);
	i40evf_disable_queues_intr(dev);
	i40e_dev_clear_queues(dev);

	/* Clean datapath event and queue/vec mapping */
	rte_intr_efd_disable(intr_handle);
	if (intr_handle->intr_vec) {
		rte_free(intr_handle->intr_vec);
		intr_handle->intr_vec = NULL;
	}
	/* remove all mac addrs */
	i40evf_add_del_all_mac_addr(dev, FALSE);

}

static int
i40evf_dev_link_update(struct rte_eth_dev *dev,
		       __rte_unused int wait_to_complete)
{
	struct rte_eth_link new_link;
	struct i40e_vf *vf = I40EVF_DEV_PRIVATE_TO_VF(dev->data->dev_private);
	/*
	 * DPDK pf host provide interfacet to acquire link status
	 * while Linux driver does not
	 */
	if (vf->version_major == I40E_DPDK_VERSION_MAJOR)
		i40evf_get_link_status(dev, &new_link);
	else {
		/* Always assume it's up, for Linux driver PF host */
		new_link.link_duplex = ETH_LINK_AUTONEG_DUPLEX;
		new_link.link_speed  = ETH_LINK_SPEED_10000;
		new_link.link_status = 1;
	}
	i40evf_dev_atomic_write_link_status(dev, &new_link);

	return 0;
}

static void
i40evf_dev_promiscuous_enable(struct rte_eth_dev *dev)
{
	struct i40e_vf *vf = I40EVF_DEV_PRIVATE_TO_VF(dev->data->dev_private);
	int ret;

	/* If enabled, just return */
	if (vf->promisc_unicast_enabled)
		return;

	ret = i40evf_config_promisc(dev, 1, vf->promisc_multicast_enabled);
	if (ret == 0)
		vf->promisc_unicast_enabled = TRUE;
}

static void
i40evf_dev_promiscuous_disable(struct rte_eth_dev *dev)
{
	struct i40e_vf *vf = I40EVF_DEV_PRIVATE_TO_VF(dev->data->dev_private);
	int ret;

	/* If disabled, just return */
	if (!vf->promisc_unicast_enabled)
		return;

	ret = i40evf_config_promisc(dev, 0, vf->promisc_multicast_enabled);
	if (ret == 0)
		vf->promisc_unicast_enabled = FALSE;
}

static void
i40evf_dev_allmulticast_enable(struct rte_eth_dev *dev)
{
	struct i40e_vf *vf = I40EVF_DEV_PRIVATE_TO_VF(dev->data->dev_private);
	int ret;

	/* If enabled, just return */
	if (vf->promisc_multicast_enabled)
		return;

	ret = i40evf_config_promisc(dev, vf->promisc_unicast_enabled, 1);
	if (ret == 0)
		vf->promisc_multicast_enabled = TRUE;
}

static void
i40evf_dev_allmulticast_disable(struct rte_eth_dev *dev)
{
	struct i40e_vf *vf = I40EVF_DEV_PRIVATE_TO_VF(dev->data->dev_private);
	int ret;

	/* If enabled, just return */
	if (!vf->promisc_multicast_enabled)
		return;

	ret = i40evf_config_promisc(dev, vf->promisc_unicast_enabled, 0);
	if (ret == 0)
		vf->promisc_multicast_enabled = FALSE;
}

static void
i40evf_dev_info_get(struct rte_eth_dev *dev, struct rte_eth_dev_info *dev_info)
{
	struct i40e_vf *vf = I40EVF_DEV_PRIVATE_TO_VF(dev->data->dev_private);

	memset(dev_info, 0, sizeof(*dev_info));
	dev_info->max_rx_queues = vf->vsi_res->num_queue_pairs;
	dev_info->max_tx_queues = vf->vsi_res->num_queue_pairs;
	dev_info->min_rx_bufsize = I40E_BUF_SIZE_MIN;
	dev_info->max_rx_pktlen = I40E_FRAME_SIZE_MAX;
	dev_info->hash_key_size = (I40E_VFQF_HKEY_MAX_INDEX + 1) * sizeof(uint32_t);
	dev_info->reta_size = ETH_RSS_RETA_SIZE_64;
	dev_info->flow_type_rss_offloads = I40E_RSS_OFFLOAD_ALL;
	dev_info->max_mac_addrs = I40E_NUM_MACADDR_MAX;
	dev_info->rx_offload_capa =
		DEV_RX_OFFLOAD_VLAN_STRIP |
		DEV_RX_OFFLOAD_QINQ_STRIP |
		DEV_RX_OFFLOAD_IPV4_CKSUM |
		DEV_RX_OFFLOAD_UDP_CKSUM |
		DEV_RX_OFFLOAD_TCP_CKSUM;
	dev_info->tx_offload_capa =
		DEV_TX_OFFLOAD_VLAN_INSERT |
		DEV_TX_OFFLOAD_QINQ_INSERT |
		DEV_TX_OFFLOAD_IPV4_CKSUM |
		DEV_TX_OFFLOAD_UDP_CKSUM |
		DEV_TX_OFFLOAD_TCP_CKSUM |
		DEV_TX_OFFLOAD_SCTP_CKSUM;

	dev_info->default_rxconf = (struct rte_eth_rxconf) {
		.rx_thresh = {
			.pthresh = I40E_DEFAULT_RX_PTHRESH,
			.hthresh = I40E_DEFAULT_RX_HTHRESH,
			.wthresh = I40E_DEFAULT_RX_WTHRESH,
		},
		.rx_free_thresh = I40E_DEFAULT_RX_FREE_THRESH,
		.rx_drop_en = 0,
	};

	dev_info->default_txconf = (struct rte_eth_txconf) {
		.tx_thresh = {
			.pthresh = I40E_DEFAULT_TX_PTHRESH,
			.hthresh = I40E_DEFAULT_TX_HTHRESH,
			.wthresh = I40E_DEFAULT_TX_WTHRESH,
		},
		.tx_free_thresh = I40E_DEFAULT_TX_FREE_THRESH,
		.tx_rs_thresh = I40E_DEFAULT_TX_RSBIT_THRESH,
		.txq_flags = ETH_TXQ_FLAGS_NOMULTSEGS |
				ETH_TXQ_FLAGS_NOOFFLOADS,
	};

	dev_info->rx_desc_lim = (struct rte_eth_desc_lim) {
		.nb_max = I40E_MAX_RING_DESC,
		.nb_min = I40E_MIN_RING_DESC,
		.nb_align = I40E_ALIGN_RING_DESC,
	};

	dev_info->tx_desc_lim = (struct rte_eth_desc_lim) {
		.nb_max = I40E_MAX_RING_DESC,
		.nb_min = I40E_MIN_RING_DESC,
		.nb_align = I40E_ALIGN_RING_DESC,
	};
}

static void
i40evf_dev_stats_get(struct rte_eth_dev *dev, struct rte_eth_stats *stats)
{
	if (i40evf_get_statics(dev, stats))
		PMD_DRV_LOG(ERR, "Get statics failed");
}

static void
i40evf_dev_close(struct rte_eth_dev *dev)
{
	struct i40e_hw *hw = I40E_DEV_PRIVATE_TO_HW(dev->data->dev_private);

	i40evf_dev_stop(dev);
	hw->adapter_stopped = 1;
	i40e_dev_free_queues(dev);
	i40evf_reset_vf(hw);
	i40e_shutdown_adminq(hw);
}

static int
i40evf_get_rss_lut(struct i40e_vsi *vsi, uint8_t *lut, uint16_t lut_size)
{
	struct i40e_vf *vf = I40E_VSI_TO_VF(vsi);
	struct i40e_hw *hw = I40E_VSI_TO_HW(vsi);
	int ret;

	if (!lut)
		return -EINVAL;

	if (vf->flags & I40E_FLAG_RSS_AQ_CAPABLE) {
		ret = i40e_aq_get_rss_lut(hw, vsi->vsi_id, FALSE,
					  lut, lut_size);
		if (ret) {
			PMD_DRV_LOG(ERR, "Failed to get RSS lookup table");
			return ret;
		}
	} else {
		uint32_t *lut_dw = (uint32_t *)lut;
		uint16_t i, lut_size_dw = lut_size / 4;

		for (i = 0; i < lut_size_dw; i++)
			lut_dw[i] = I40E_READ_REG(hw, I40E_VFQF_HLUT(i));
	}

	return 0;
}

static int
i40evf_set_rss_lut(struct i40e_vsi *vsi, uint8_t *lut, uint16_t lut_size)
{
	struct i40e_vf *vf = I40E_VSI_TO_VF(vsi);
	struct i40e_hw *hw = I40E_VSI_TO_HW(vsi);
	int ret;

	if (!vsi || !lut)
		return -EINVAL;

	if (vf->flags & I40E_FLAG_RSS_AQ_CAPABLE) {
		ret = i40e_aq_set_rss_lut(hw, vsi->vsi_id, FALSE,
					  lut, lut_size);
		if (ret) {
			PMD_DRV_LOG(ERR, "Failed to set RSS lookup table");
			return ret;
		}
	} else {
		uint32_t *lut_dw = (uint32_t *)lut;
		uint16_t i, lut_size_dw = lut_size / 4;

		for (i = 0; i < lut_size_dw; i++)
			I40E_WRITE_REG(hw, I40E_VFQF_HLUT(i), lut_dw[i]);
		I40EVF_WRITE_FLUSH(hw);
	}

	return 0;
}

static int
i40evf_dev_rss_reta_update(struct rte_eth_dev *dev,
			   struct rte_eth_rss_reta_entry64 *reta_conf,
			   uint16_t reta_size)
{
	struct i40e_vf *vf = I40EVF_DEV_PRIVATE_TO_VF(dev->data->dev_private);
	uint8_t *lut;
	uint16_t i, idx, shift;
	int ret;

	if (reta_size != ETH_RSS_RETA_SIZE_64) {
		PMD_DRV_LOG(ERR, "The size of hash lookup table configured "
			"(%d) doesn't match the number of hardware can "
			"support (%d)\n", reta_size, ETH_RSS_RETA_SIZE_64);
		return -EINVAL;
	}

	lut = rte_zmalloc("i40e_rss_lut", reta_size, 0);
	if (!lut) {
		PMD_DRV_LOG(ERR, "No memory can be allocated");
		return -ENOMEM;
	}
	ret = i40evf_get_rss_lut(&vf->vsi, lut, reta_size);
	if (ret)
		goto out;
	for (i = 0; i < reta_size; i++) {
		idx = i / RTE_RETA_GROUP_SIZE;
		shift = i % RTE_RETA_GROUP_SIZE;
		if (reta_conf[idx].mask & (1ULL << shift))
			lut[i] = reta_conf[idx].reta[shift];
	}
	ret = i40evf_set_rss_lut(&vf->vsi, lut, reta_size);

out:
	rte_free(lut);

	return ret;
}

static int
i40evf_dev_rss_reta_query(struct rte_eth_dev *dev,
			  struct rte_eth_rss_reta_entry64 *reta_conf,
			  uint16_t reta_size)
{
	struct i40e_vf *vf = I40EVF_DEV_PRIVATE_TO_VF(dev->data->dev_private);
	uint16_t i, idx, shift;
	uint8_t *lut;
	int ret;

	if (reta_size != ETH_RSS_RETA_SIZE_64) {
		PMD_DRV_LOG(ERR, "The size of hash lookup table configured "
			"(%d) doesn't match the number of hardware can "
			"support (%d)\n", reta_size, ETH_RSS_RETA_SIZE_64);
		return -EINVAL;
	}

	lut = rte_zmalloc("i40e_rss_lut", reta_size, 0);
	if (!lut) {
		PMD_DRV_LOG(ERR, "No memory can be allocated");
		return -ENOMEM;
	}

	ret = i40evf_get_rss_lut(&vf->vsi, lut, reta_size);
	if (ret)
		goto out;
	for (i = 0; i < reta_size; i++) {
		idx = i / RTE_RETA_GROUP_SIZE;
		shift = i % RTE_RETA_GROUP_SIZE;
		if (reta_conf[idx].mask & (1ULL << shift))
			reta_conf[idx].reta[shift] = lut[i];
	}

out:
	rte_free(lut);

	return ret;
}

static int
i40evf_set_rss_key(struct i40e_vsi *vsi, uint8_t *key, uint8_t key_len)
{
	struct i40e_vf *vf = I40E_VSI_TO_VF(vsi);
	struct i40e_hw *hw = I40E_VSI_TO_HW(vsi);
	int ret = 0;

	if (!key || key_len == 0) {
		PMD_DRV_LOG(DEBUG, "No key to be configured");
		return 0;
	} else if (key_len != (I40E_VFQF_HKEY_MAX_INDEX + 1) *
		sizeof(uint32_t)) {
		PMD_DRV_LOG(ERR, "Invalid key length %u", key_len);
		return -EINVAL;
	}

	if (vf->flags & I40E_FLAG_RSS_AQ_CAPABLE) {
		struct i40e_aqc_get_set_rss_key_data *key_dw =
			(struct i40e_aqc_get_set_rss_key_data *)key;

		ret = i40e_aq_set_rss_key(hw, vsi->vsi_id, key_dw);
		if (ret)
			PMD_INIT_LOG(ERR, "Failed to configure RSS key "
				     "via AQ");
	} else {
		uint32_t *hash_key = (uint32_t *)key;
		uint16_t i;

		for (i = 0; i <= I40E_VFQF_HKEY_MAX_INDEX; i++)
			i40e_write_rx_ctl(hw, I40E_VFQF_HKEY(i), hash_key[i]);
		I40EVF_WRITE_FLUSH(hw);
	}

	return ret;
}

static int
i40evf_get_rss_key(struct i40e_vsi *vsi, uint8_t *key, uint8_t *key_len)
{
	struct i40e_vf *vf = I40E_VSI_TO_VF(vsi);
	struct i40e_hw *hw = I40E_VSI_TO_HW(vsi);
	int ret;

	if (!key || !key_len)
		return -EINVAL;

	if (vf->flags & I40E_FLAG_RSS_AQ_CAPABLE) {
		ret = i40e_aq_get_rss_key(hw, vsi->vsi_id,
			(struct i40e_aqc_get_set_rss_key_data *)key);
		if (ret) {
			PMD_INIT_LOG(ERR, "Failed to get RSS key via AQ");
			return ret;
		}
	} else {
		uint32_t *key_dw = (uint32_t *)key;
		uint16_t i;

		for (i = 0; i <= I40E_VFQF_HKEY_MAX_INDEX; i++)
			key_dw[i] = i40e_read_rx_ctl(hw, I40E_VFQF_HKEY(i));
	}
	*key_len = (I40E_VFQF_HKEY_MAX_INDEX + 1) * sizeof(uint32_t);

	return 0;
}

static int
i40evf_hw_rss_hash_set(struct i40e_vf *vf, struct rte_eth_rss_conf *rss_conf)
{
	struct i40e_hw *hw = I40E_VF_TO_HW(vf);
	uint64_t rss_hf, hena;
	int ret;

	ret = i40evf_set_rss_key(&vf->vsi, rss_conf->rss_key,
				 rss_conf->rss_key_len);
	if (ret)
		return ret;

	rss_hf = rss_conf->rss_hf;
	hena = (uint64_t)i40e_read_rx_ctl(hw, I40E_VFQF_HENA(0));
	hena |= ((uint64_t)i40e_read_rx_ctl(hw, I40E_VFQF_HENA(1))) << 32;
	hena &= ~I40E_RSS_HENA_ALL;
	hena |= i40e_config_hena(rss_hf);
	i40e_write_rx_ctl(hw, I40E_VFQF_HENA(0), (uint32_t)hena);
	i40e_write_rx_ctl(hw, I40E_VFQF_HENA(1), (uint32_t)(hena >> 32));
	I40EVF_WRITE_FLUSH(hw);

	return 0;
}

static void
i40evf_disable_rss(struct i40e_vf *vf)
{
	struct i40e_hw *hw = I40E_VF_TO_HW(vf);
	uint64_t hena;

	hena = (uint64_t)i40e_read_rx_ctl(hw, I40E_VFQF_HENA(0));
	hena |= ((uint64_t)i40e_read_rx_ctl(hw, I40E_VFQF_HENA(1))) << 32;
	hena &= ~I40E_RSS_HENA_ALL;
	i40e_write_rx_ctl(hw, I40E_VFQF_HENA(0), (uint32_t)hena);
	i40e_write_rx_ctl(hw, I40E_VFQF_HENA(1), (uint32_t)(hena >> 32));
	I40EVF_WRITE_FLUSH(hw);
}

static int
i40evf_config_rss(struct i40e_vf *vf)
{
	struct i40e_hw *hw = I40E_VF_TO_HW(vf);
	struct rte_eth_rss_conf rss_conf;
	uint32_t i, j, lut = 0, nb_q = (I40E_VFQF_HLUT_MAX_INDEX + 1) * 4;
	uint16_t num;

	if (vf->dev_data->dev_conf.rxmode.mq_mode != ETH_MQ_RX_RSS) {
		i40evf_disable_rss(vf);
		PMD_DRV_LOG(DEBUG, "RSS not configured\n");
		return 0;
	}

	num = RTE_MIN(vf->dev_data->nb_rx_queues, I40E_MAX_QP_NUM_PER_VF);
	/* Fill out the look up table */
	for (i = 0, j = 0; i < nb_q; i++, j++) {
		if (j >= num)
			j = 0;
		lut = (lut << 8) | j;
		if ((i & 3) == 3)
			I40E_WRITE_REG(hw, I40E_VFQF_HLUT(i >> 2), lut);
	}

	rss_conf = vf->dev_data->dev_conf.rx_adv_conf.rss_conf;
	if ((rss_conf.rss_hf & I40E_RSS_OFFLOAD_ALL) == 0) {
		i40evf_disable_rss(vf);
		PMD_DRV_LOG(DEBUG, "No hash flag is set\n");
		return 0;
	}

	if (rss_conf.rss_key == NULL || rss_conf.rss_key_len <
		(I40E_VFQF_HKEY_MAX_INDEX + 1) * sizeof(uint32_t)) {
		/* Calculate the default hash key */
		for (i = 0; i <= I40E_VFQF_HKEY_MAX_INDEX; i++)
			rss_key_default[i] = (uint32_t)rte_rand();
		rss_conf.rss_key = (uint8_t *)rss_key_default;
		rss_conf.rss_key_len = (I40E_VFQF_HKEY_MAX_INDEX + 1) *
			sizeof(uint32_t);
	}

	return i40evf_hw_rss_hash_set(vf, &rss_conf);
}

static int
i40evf_dev_rss_hash_update(struct rte_eth_dev *dev,
			   struct rte_eth_rss_conf *rss_conf)
{
	struct i40e_vf *vf = I40EVF_DEV_PRIVATE_TO_VF(dev->data->dev_private);
	struct i40e_hw *hw = I40E_DEV_PRIVATE_TO_HW(dev->data->dev_private);
	uint64_t rss_hf = rss_conf->rss_hf & I40E_RSS_OFFLOAD_ALL;
	uint64_t hena;

	hena = (uint64_t)i40e_read_rx_ctl(hw, I40E_VFQF_HENA(0));
	hena |= ((uint64_t)i40e_read_rx_ctl(hw, I40E_VFQF_HENA(1))) << 32;
	if (!(hena & I40E_RSS_HENA_ALL)) { /* RSS disabled */
		if (rss_hf != 0) /* Enable RSS */
			return -EINVAL;
		return 0;
	}

	/* RSS enabled */
	if (rss_hf == 0) /* Disable RSS */
		return -EINVAL;

	return i40evf_hw_rss_hash_set(vf, rss_conf);
}

static int
i40evf_dev_rss_hash_conf_get(struct rte_eth_dev *dev,
			     struct rte_eth_rss_conf *rss_conf)
{
	struct i40e_vf *vf = I40EVF_DEV_PRIVATE_TO_VF(dev->data->dev_private);
	struct i40e_hw *hw = I40E_DEV_PRIVATE_TO_HW(dev->data->dev_private);
	uint64_t hena;

	i40evf_get_rss_key(&vf->vsi, rss_conf->rss_key,
			   &rss_conf->rss_key_len);

	hena = (uint64_t)i40e_read_rx_ctl(hw, I40E_VFQF_HENA(0));
	hena |= ((uint64_t)i40e_read_rx_ctl(hw, I40E_VFQF_HENA(1))) << 32;
	rss_conf->rss_hf = i40e_parse_hena(hena);

	return 0;
}
