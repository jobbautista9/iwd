/*
 *
 *  Wireless daemon for Linux
 *
 *  Copyright (C) 2013-2014  Intel Corporation. All rights reserved.
 *
 *  This library is free software; you can redistribute it and/or
 *  modify it under the terms of the GNU Lesser General Public
 *  License as published by the Free Software Foundation; either
 *  version 2.1 of the License, or (at your option) any later version.
 *
 *  This library is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 *  Lesser General Public License for more details.
 *
 *  You should have received a copy of the GNU Lesser General Public
 *  License along with this library; if not, write to the Free Software
 *  Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA  02110-1301  USA
 *
 */

#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

#include <stdlib.h>
#include <linux/rtnetlink.h>
#include <net/if_arp.h>
#include <linux/if.h>
#include <linux/if_packet.h>
#include <linux/if_ether.h>
#include <sys/socket.h>
#include <errno.h>
#include <fnmatch.h>

#include <ell/ell.h>

#include "linux/nl80211.h"

#include "src/iwd.h"
#include "src/wiphy.h"
#include "src/ie.h"
#include "src/mpdu.h"
#include "src/eapol.h"
#include "src/handshake.h"
#include "src/crypto.h"
#include "src/device.h"
#include "src/scan.h"
#include "src/netdev.h"
#include "src/wscutil.h"
#include "src/ftutil.h"

struct netdev {
	uint32_t index;
	char name[IFNAMSIZ];
	uint32_t type;
	uint8_t addr[ETH_ALEN];
	struct device *device;
	struct wiphy *wiphy;
	unsigned int ifi_flags;
	uint32_t frequency;

	netdev_event_func_t event_filter;
	netdev_connect_cb_t connect_cb;
	netdev_disconnect_cb_t disconnect_cb;
	netdev_neighbor_report_cb_t neighbor_report_cb;
	void *user_data;
	struct eapol_sm *sm;
	struct handshake_state *handshake;
	uint32_t pairwise_new_key_cmd_id;
	uint32_t pairwise_set_key_cmd_id;
	uint32_t group_new_key_cmd_id;
	uint32_t group_management_new_key_cmd_id;
	uint32_t connect_cmd_id;
	uint32_t disconnect_cmd_id;
	enum netdev_result result;
	struct l_timeout *neighbor_report_timeout;
	uint8_t prev_bssid[ETH_ALEN];

	struct l_queue *watches;
	uint32_t next_watch_id;

	bool connected : 1;
	bool operational : 1;
	bool rekey_offload_support : 1;
	bool in_ft : 1;
};

struct netdev_watch {
	uint32_t id;
	netdev_watch_func_t callback;
	void *user_data;
};

static struct l_netlink *rtnl = NULL;
static struct l_genl_family *nl80211;
static struct l_queue *netdev_list;
static char **whitelist_filter;
static char **blacklist_filter;

static void do_debug(const char *str, void *user_data)
{
	const char *prefix = user_data;

	l_info("%s%s", prefix, str);
}

struct cb_data {
	netdev_command_func_t callback;
	void *user_data;
};

static void netlink_result(int error, uint16_t type, const void *data,
			uint32_t len, void *user_data)
{
	struct cb_data *cb_data = user_data;

	if (!cb_data)
		return;

	cb_data->callback(error < 0 ? false : true, cb_data->user_data);
}

static size_t rta_add_u8(void *rta_buf, unsigned short type, uint8_t value)
{
	struct rtattr *rta = rta_buf;

	rta->rta_len = RTA_LENGTH(sizeof(uint8_t));
	rta->rta_type = type;
	*((uint8_t *) RTA_DATA(rta)) = value;

	return RTA_SPACE(sizeof(uint8_t));
}

static void netdev_set_linkmode_and_operstate(uint32_t ifindex,
				uint8_t linkmode, uint8_t operstate,
				netdev_command_func_t callback, void *user_data)
{
	struct ifinfomsg *rtmmsg;
	void *rta_buf;
	size_t bufsize;
	struct cb_data *cb_data = NULL;

	bufsize = NLMSG_ALIGN(sizeof(struct ifinfomsg)) +
		RTA_SPACE(sizeof(uint8_t)) + RTA_SPACE(sizeof(uint8_t));

	rtmmsg = l_malloc(bufsize);
	memset(rtmmsg, 0, bufsize);

	rtmmsg->ifi_family = AF_UNSPEC;
	rtmmsg->ifi_index = ifindex;

	rta_buf = (void *) rtmmsg + NLMSG_ALIGN(sizeof(struct ifinfomsg));

	rta_buf += rta_add_u8(rta_buf, IFLA_LINKMODE, linkmode);
	rta_buf += rta_add_u8(rta_buf, IFLA_OPERSTATE, operstate);

	if (callback) {
		cb_data = l_new(struct cb_data, 1);
		cb_data->callback = callback;
		cb_data->user_data = user_data;
	}

	l_netlink_send(rtnl, RTM_SETLINK, 0, rtmmsg,
					rta_buf - (void *) rtmmsg,
					netlink_result, cb_data, l_free);

	l_free(rtmmsg);
}

const uint8_t *netdev_get_address(struct netdev *netdev)
{
	return netdev->addr;
}

uint32_t netdev_get_ifindex(struct netdev *netdev)
{
	return netdev->index;
}

uint32_t netdev_get_iftype(struct netdev *netdev)
{
	return netdev->type;
}

const char *netdev_get_name(struct netdev *netdev)
{
	return netdev->name;
}

bool netdev_get_is_up(struct netdev *netdev)
{
	return (netdev->ifi_flags & IFF_UP) != 0;
}

struct handshake_state *netdev_get_handshake(struct netdev *netdev)
{
	return netdev->handshake;
}

struct set_powered_cb_data {
	struct netdev *netdev;
	netdev_set_powered_cb_t callback;
	void *user_data;
	l_netlink_destroy_func_t destroy;
};

static void netdev_set_powered_result(int error, uint16_t type,
					const void *data,
					uint32_t len, void *user_data)
{
	struct set_powered_cb_data *cb_data = user_data;

	if (!cb_data)
		return;

	cb_data->callback(cb_data->netdev, -error, cb_data->user_data);
}

static void netdev_set_powered_destroy(void *user_data)
{
	struct set_powered_cb_data *cb_data = user_data;

	if (!cb_data)
		return;

	if (cb_data->destroy)
		cb_data->destroy(cb_data->user_data);

	l_free(cb_data);
}

int netdev_set_powered(struct netdev *netdev, bool powered,
			netdev_set_powered_cb_t callback, void *user_data,
			netdev_destroy_func_t destroy)
{
	struct ifinfomsg *rtmmsg;
	size_t bufsize;
	struct set_powered_cb_data *cb_data = NULL;

	bufsize = NLMSG_ALIGN(sizeof(struct ifinfomsg));

	rtmmsg = l_malloc(bufsize);
	memset(rtmmsg, 0, bufsize);

	rtmmsg->ifi_family = AF_UNSPEC;
	rtmmsg->ifi_index = netdev->index;
	rtmmsg->ifi_change = 0xffffffff;
	rtmmsg->ifi_flags = powered ? (netdev->ifi_flags | IFF_UP) :
		(netdev->ifi_flags & ~IFF_UP);

	if (callback) {
		cb_data = l_new(struct set_powered_cb_data, 1);
		cb_data->netdev = netdev;
		cb_data->callback = callback;
		cb_data->user_data = user_data;
		cb_data->destroy = destroy;
	}

	l_netlink_send(rtnl, RTM_SETLINK, 0, rtmmsg, bufsize,
			netdev_set_powered_result, cb_data,
			netdev_set_powered_destroy);

	l_free(rtmmsg);

	return 0;
}

static void netdev_operstate_dormant_cb(bool success, void *user_data)
{
	struct netdev *netdev = user_data;

	l_debug("netdev: %d, success: %d", netdev->index, success);
}

static void netdev_operstate_down_cb(bool success, void *user_data)
{
	uint32_t index = L_PTR_TO_UINT(user_data);

	l_debug("netdev: %d, success: %d", index, success);
}

static void netdev_connect_free(struct netdev *netdev)
{
	if (netdev->sm) {
		eapol_sm_free(netdev->sm);
		netdev->sm = NULL;
	}

	if (netdev->handshake) {
		handshake_state_free(netdev->handshake);
		netdev->handshake = NULL;
	}

	if (netdev->neighbor_report_cb) {
		netdev->neighbor_report_cb(netdev, -ENOTCONN, NULL, 0,
						netdev->user_data);
		netdev->neighbor_report_cb = NULL;
		l_timeout_remove(netdev->neighbor_report_timeout);
	}

	netdev->operational = false;
	netdev->connected = false;
	netdev->connect_cb = NULL;
	netdev->event_filter = NULL;
	netdev->user_data = NULL;
	netdev->result = NETDEV_RESULT_OK;
	netdev->in_ft = false;

	if (netdev->pairwise_new_key_cmd_id) {
		l_genl_family_cancel(nl80211, netdev->pairwise_new_key_cmd_id);
		netdev->pairwise_new_key_cmd_id = 0;
	}

	if (netdev->pairwise_set_key_cmd_id) {
		l_genl_family_cancel(nl80211, netdev->pairwise_set_key_cmd_id);
		netdev->pairwise_set_key_cmd_id = 0;
	}

	if (netdev->group_new_key_cmd_id) {
		l_genl_family_cancel(nl80211, netdev->group_new_key_cmd_id);
		netdev->group_new_key_cmd_id = 0;
	}

	if (netdev->group_management_new_key_cmd_id) {
		l_genl_family_cancel(nl80211,
			netdev->group_management_new_key_cmd_id);
		netdev->group_management_new_key_cmd_id = 0;
	}

	if (netdev->connect_cmd_id) {
		l_genl_family_cancel(nl80211, netdev->connect_cmd_id);
		netdev->connect_cmd_id = 0;
	} else if (netdev->disconnect_cmd_id) {
		l_genl_family_cancel(nl80211, netdev->disconnect_cmd_id);
		netdev->disconnect_cmd_id = 0;
	}
}

static void netdev_connect_failed(struct l_genl_msg *msg, void *user_data)
{
	struct netdev *netdev = user_data;
	netdev_connect_cb_t connect_cb = netdev->connect_cb;
	netdev_event_func_t event_filter = netdev->event_filter;
	void *connect_data = netdev->user_data;
	enum netdev_result result = netdev->result;

	netdev->disconnect_cmd_id = 0;

	/* Done this way to allow re-entrant netdev_connect calls */
	netdev_connect_free(netdev);

	if (connect_cb)
		connect_cb(netdev, result, connect_data);
	else if (event_filter)
		event_filter(netdev, NETDEV_EVENT_DISCONNECT_BY_SME,
				connect_data);
}

static void netdev_free(void *data)
{
	struct netdev *netdev = data;

	l_debug("Freeing netdev %s[%d]", netdev->name, netdev->index);

	if (netdev->neighbor_report_cb) {
		netdev->neighbor_report_cb(netdev, -ENODEV, NULL, 0,
						netdev->user_data);
		netdev->neighbor_report_cb = NULL;
		l_timeout_remove(netdev->neighbor_report_timeout);
	}

	if (netdev->connected)
		netdev_connect_free(netdev);
	else if (netdev->disconnect_cmd_id) {
		l_genl_family_cancel(nl80211, netdev->disconnect_cmd_id);
		netdev->disconnect_cmd_id = 0;

		if (netdev->disconnect_cb)
			netdev->disconnect_cb(netdev, true, netdev->user_data);

		netdev->disconnect_cb = NULL;
		netdev->user_data = NULL;
	}

	device_remove(netdev->device);

	l_queue_destroy(netdev->watches, l_free);

	l_free(netdev);
}

static void netdev_shutdown_one(void *data, void *user_data)
{
	struct netdev *netdev = data;

	netdev_set_linkmode_and_operstate(netdev->index, 0, IF_OPER_DOWN,
						netdev_operstate_down_cb,
						L_UINT_TO_PTR(netdev->index));

	if (netdev_get_is_up(netdev))
		netdev_set_powered(netdev, false, NULL, NULL, NULL);
}

static bool netdev_match(const void *a, const void *b)
{
	const struct netdev *netdev = a;
	uint32_t ifindex = L_PTR_TO_UINT(b);

	return (netdev->index == ifindex);
}

struct netdev *netdev_find(int ifindex)
{
	return l_queue_find(netdev_list, netdev_match, L_UINT_TO_PTR(ifindex));
}

static void netdev_lost_beacon(struct netdev *netdev)
{
	if (!netdev->connected)
		return;

	if (netdev->event_filter)
		netdev->event_filter(netdev, NETDEV_EVENT_LOST_BEACON,
							netdev->user_data);

	netdev_connect_free(netdev);
}

static void netdev_rssi_threshold(struct netdev *netdev, uint32_t rssi_event)
{
	if (!netdev->connected)
		return;

	if (rssi_event != NL80211_CQM_RSSI_THRESHOLD_EVENT_LOW &&
			rssi_event != NL80211_CQM_RSSI_THRESHOLD_EVENT_HIGH)
		return;

	if (netdev->event_filter) {
		int event;

		event = (rssi_event == NL80211_CQM_RSSI_THRESHOLD_EVENT_LOW) ?
			NETDEV_EVENT_RSSI_THRESHOLD_LOW :
			NETDEV_EVENT_RSSI_THRESHOLD_HIGH;

		netdev->event_filter(netdev, event, netdev->user_data);
	}
}

static void netdev_cqm_event(struct l_genl_msg *msg, struct netdev *netdev)
{
	struct l_genl_attr attr;
	struct l_genl_attr nested;
	uint16_t type, len;
	const void *data;

	if (!l_genl_attr_init(&attr, msg))
		return;

	while (l_genl_attr_next(&attr, &type, &len, &data)) {
		switch (type) {
		case NL80211_ATTR_CQM:
			if (!l_genl_attr_recurse(&attr, &nested))
				return;

			while (l_genl_attr_next(&nested, &type, &len, &data)) {
				switch (type) {
				case NL80211_ATTR_CQM_BEACON_LOSS_EVENT:
					netdev_lost_beacon(netdev);
					break;

				case NL80211_ATTR_CQM_RSSI_THRESHOLD_EVENT:
					if (len != 4)
						continue;

					netdev_rssi_threshold(netdev,
							*(uint32_t *) data);
					break;
				}
			}

			break;
		}
	}
}

static void netdev_rekey_offload_event(struct l_genl_msg *msg,
					struct netdev *netdev)
{
	struct l_genl_attr attr;
	struct l_genl_attr nested;
	uint16_t type, len;
	const void *data;
	uint64_t replay_ctr;

	if (!l_genl_attr_init(&attr, msg))
		return;

	while (l_genl_attr_next(&attr, &type, &len, &data)) {
		if (type != NL80211_ATTR_REKEY_DATA)
			continue;

		if (!l_genl_attr_recurse(&attr, &nested))
			return;

		while (l_genl_attr_next(&nested, &type, &len, &data)) {
			if (type != NL80211_REKEY_DATA_REPLAY_CTR)
				continue;

			if (len != sizeof(uint64_t)) {
				l_warn("Invalid replay_ctr");
				return;
			}

			replay_ctr = *((uint64_t *) data);
			__eapol_update_replay_counter(netdev->index,
							netdev->addr,
							netdev->handshake->aa,
							replay_ctr);
			return;
		}
	}
}

static void netdev_disconnect_event(struct l_genl_msg *msg,
							struct netdev *netdev)
{
	struct l_genl_attr attr;
	uint16_t type, len;
	const void *data;
	uint16_t reason_code = 0;
	bool disconnect_by_ap = false;
	netdev_event_func_t event_filter;
	void *event_data;

	l_debug("");

	if (!netdev->connected || netdev->disconnect_cmd_id > 0 ||
			netdev->in_ft)
		return;

	if (!l_genl_attr_init(&attr, msg)) {
		l_error("attr init failed");
		return;
	}

	while (l_genl_attr_next(&attr, &type, &len, &data)) {
		switch (type) {
		case NL80211_ATTR_REASON_CODE:
			if (len != sizeof(uint16_t))
				l_warn("Invalid reason code attribute");
			else
				reason_code = *((uint16_t *) data);

			break;

		case NL80211_ATTR_DISCONNECTED_BY_AP:
			disconnect_by_ap = true;
			break;
		}
	}

	l_info("Received Deauthentication event, reason: %hu, from_ap: %s",
			reason_code, disconnect_by_ap ? "true" : "false");

	event_filter = netdev->event_filter;
	event_data = netdev->user_data;
	netdev_connect_free(netdev);

	if (disconnect_by_ap && event_filter)
		event_filter(netdev, NETDEV_EVENT_DISCONNECT_BY_AP,
							event_data);
}

static void netdev_deauthenticate_event(struct l_genl_msg *msg,
							struct netdev *netdev)
{
	l_debug("");
}

static void netdev_cmd_deauthenticate_cb(struct l_genl_msg *msg,
								void *user_data)
{
	struct netdev *netdev = user_data;
	void *disconnect_data;
	netdev_disconnect_cb_t disconnect_cb;
	bool r;

	netdev->disconnect_cmd_id = 0;

	if (!netdev->disconnect_cb) {
		netdev->user_data = NULL;
		return;
	}

	disconnect_data = netdev->user_data;
	disconnect_cb = netdev->disconnect_cb;
	netdev->user_data = NULL;
	netdev->disconnect_cb = NULL;

	if (l_genl_msg_get_error(msg) < 0)
		r = false;
	else
		r = true;

	disconnect_cb(netdev, r, disconnect_data);
}

static struct l_genl_msg *netdev_build_cmd_deauthenticate(struct netdev *netdev,
							uint16_t reason_code)
{
	struct l_genl_msg *msg;

	msg = l_genl_msg_new_sized(NL80211_CMD_DEAUTHENTICATE, 128);
	l_genl_msg_append_attr(msg, NL80211_ATTR_IFINDEX, 4, &netdev->index);
	l_genl_msg_append_attr(msg, NL80211_ATTR_REASON_CODE, 2, &reason_code);
	l_genl_msg_append_attr(msg, NL80211_ATTR_MAC, ETH_ALEN,
							netdev->handshake->aa);

	return msg;
}

static void netdev_operstate_cb(bool success, void *user_data)
{
	struct netdev *netdev = user_data;

	if (!netdev->connected)
		return;

	if (!success) {
		struct l_genl_msg *msg;

		l_error("Setting LinkMode and OperState failed for ifindex: %d",
				netdev->index);

		netdev->result = NETDEV_RESULT_KEY_SETTING_FAILED;
		msg = netdev_build_cmd_deauthenticate(netdev,
						MPDU_REASON_CODE_UNSPECIFIED);
		netdev->disconnect_cmd_id = l_genl_family_send(nl80211, msg,
							netdev_connect_failed,
							netdev, NULL);
		return;
	}

	netdev->operational = true;

	if (netdev->connect_cb) {
		netdev->connect_cb(netdev, NETDEV_RESULT_OK, netdev->user_data);
		netdev->connect_cb = NULL;
	}
}

static void netdev_setting_keys_failed(struct netdev *netdev,
							uint16_t reason_code)
{
	struct l_genl_msg *msg;

	/*
	 * Something went wrong with our new_key, set_key, new_key,
	 * set_station
	 *
	 * Cancel all pending commands, then de-authenticate
	 */
	l_genl_family_cancel(nl80211, netdev->pairwise_new_key_cmd_id);
	netdev->pairwise_new_key_cmd_id = 0;

	l_genl_family_cancel(nl80211, netdev->pairwise_set_key_cmd_id);
	netdev->pairwise_set_key_cmd_id = 0;

	l_genl_family_cancel(nl80211, netdev->group_new_key_cmd_id);
	netdev->group_new_key_cmd_id = 0;

	l_genl_family_cancel(nl80211,
		netdev->group_management_new_key_cmd_id);
	netdev->group_management_new_key_cmd_id = 0;

	netdev->result = NETDEV_RESULT_KEY_SETTING_FAILED;
	msg = netdev_build_cmd_deauthenticate(netdev,
						MPDU_REASON_CODE_UNSPECIFIED);
	netdev->disconnect_cmd_id = l_genl_family_send(nl80211, msg,
							netdev_connect_failed,
							netdev, NULL);
}

static void netdev_set_station_cb(struct l_genl_msg *msg, void *user_data)
{
	struct netdev *netdev = user_data;

	if (!netdev->connected)
		return;

	if (l_genl_msg_get_error(msg) < 0) {
		l_error("Set Station failed for ifindex %d", netdev->index);
		netdev_setting_keys_failed(netdev,
						MPDU_REASON_CODE_UNSPECIFIED);
		return;
	}

	netdev_set_linkmode_and_operstate(netdev->index, 1, IF_OPER_UP,
						netdev_operstate_cb, netdev);
}

static struct l_genl_msg *netdev_build_cmd_set_station(struct netdev *netdev)
{
	struct l_genl_msg *msg;
	struct nl80211_sta_flag_update flags;

	flags.mask = 1 << NL80211_STA_FLAG_AUTHORIZED;
	flags.set = flags.mask;

	msg = l_genl_msg_new_sized(NL80211_CMD_SET_STATION, 512);

	l_genl_msg_append_attr(msg, NL80211_ATTR_IFINDEX, 4, &netdev->index);
	l_genl_msg_append_attr(msg, NL80211_ATTR_MAC, ETH_ALEN,
						netdev->handshake->aa);
	l_genl_msg_append_attr(msg, NL80211_ATTR_STA_FLAGS2,
				sizeof(struct nl80211_sta_flag_update), &flags);

	return msg;
}

static void netdev_new_group_key_cb(struct l_genl_msg *msg, void *data)
{
	struct netdev *netdev = data;

	netdev->group_new_key_cmd_id = 0;

	if (l_genl_msg_get_error(msg) < 0) {
		l_error("New Key for Group Key failed for ifindex: %d",
				netdev->index);
		goto error;
	}

	msg = netdev_build_cmd_set_station(netdev);

	if (l_genl_family_send(nl80211, msg, netdev_set_station_cb,
							netdev, NULL) > 0)
		return;

error:
	netdev_setting_keys_failed(netdev, MPDU_REASON_CODE_UNSPECIFIED);
}

static void netdev_new_group_management_key_cb(struct l_genl_msg *msg,
					void *data)
{
	struct netdev *netdev = data;

	netdev->group_management_new_key_cmd_id = 0;

	if (l_genl_msg_get_error(msg) < 0) {
		l_error("New Key for Group Mgmt failed for ifindex: %d",
				netdev->index);
		netdev_setting_keys_failed(netdev,
			MPDU_REASON_CODE_UNSPECIFIED);
	}
}

static struct l_genl_msg *netdev_build_cmd_new_key_group(struct netdev *netdev,
					uint32_t cipher, uint8_t key_id,
					const uint8_t *key, size_t key_len,
					const uint8_t *ctr, size_t ctr_len)
{
	struct l_genl_msg *msg;

	msg = l_genl_msg_new_sized(NL80211_CMD_NEW_KEY, 512);

	l_genl_msg_append_attr(msg, NL80211_ATTR_KEY_DATA, key_len, key);
	l_genl_msg_append_attr(msg, NL80211_ATTR_KEY_CIPHER, 4, &cipher);
	l_genl_msg_append_attr(msg, NL80211_ATTR_KEY_SEQ, ctr_len, ctr);

	l_genl_msg_enter_nested(msg, NL80211_ATTR_KEY_DEFAULT_TYPES);
	l_genl_msg_append_attr(msg, NL80211_KEY_DEFAULT_TYPE_MULTICAST,
				0, NULL);
	l_genl_msg_leave_nested(msg);

	l_genl_msg_append_attr(msg, NL80211_ATTR_KEY_IDX, 1, &key_id);
	l_genl_msg_append_attr(msg, NL80211_ATTR_IFINDEX, 4, &netdev->index);

	return msg;
}

static void netdev_set_gtk(uint32_t ifindex, uint8_t key_index,
				const uint8_t *gtk, uint8_t gtk_len,
				const uint8_t *rsc, uint8_t rsc_len,
				uint32_t cipher, void *user_data)
{
	uint8_t gtk_buf[32];
	struct netdev *netdev;
	struct l_genl_msg *msg;

	netdev = netdev_find(ifindex);

	l_debug("%d", netdev->index);

	if (crypto_cipher_key_len(cipher) != gtk_len) {
		l_error("Unexpected key length: %d", gtk_len);
		netdev_setting_keys_failed(netdev,
					MPDU_REASON_CODE_INVALID_GROUP_CIPHER);
		return;
	}

	switch (cipher) {
	case CRYPTO_CIPHER_CCMP:
		memcpy(gtk_buf, gtk, 16);
		break;
	case CRYPTO_CIPHER_TKIP:
		/*
		 * Swap the TX and RX MIC key portions for supplicant.
		 * WPA_80211_v3_1_090922 doc's 3.3.4:
		 *   The MIC key used on the Client for transmit (TX) is in
		 *   bytes 24-31, and the MIC key used on the Client for
		 *   receive (RX) is in bytes 16-23 of the PTK.  That is,
		 *   assume that TX MIC and RX MIC referred to in Clause 8.7
		 *   are referenced to the Authenticator. Similarly, on the AP,
		 *   the MIC used for TX is in bytes 16-23, and the MIC key
		 *   used for RX is in bytes 24-31 of the PTK.
		 *
		 * Here apply this to the GTK instead of the PTK.
		 */
		memcpy(gtk_buf, gtk, 16);
		memcpy(gtk_buf + 16, gtk + 24, 8);
		memcpy(gtk_buf + 24, gtk + 16, 8);
		break;
	default:
		l_error("Unexpected cipher: %x", cipher);
		netdev_setting_keys_failed(netdev,
					MPDU_REASON_CODE_INVALID_GROUP_CIPHER);
		return;
	}

	msg = netdev_build_cmd_new_key_group(netdev, cipher, key_index,
						gtk_buf, gtk_len,
						rsc, rsc_len);
	netdev->group_new_key_cmd_id =
		l_genl_family_send(nl80211, msg, netdev_new_group_key_cb,
						netdev, NULL);

	if (netdev->group_new_key_cmd_id > 0)
		return;

	l_genl_msg_unref(msg);
	netdev_setting_keys_failed(netdev, MPDU_REASON_CODE_UNSPECIFIED);
}

static void netdev_set_igtk(uint32_t ifindex, uint8_t key_index,
				const uint8_t *igtk, uint8_t igtk_len,
				const uint8_t *ipn, uint8_t ipn_len,
				uint32_t cipher, void *user_data)
{
	uint8_t igtk_buf[16];
	struct netdev *netdev;
	struct l_genl_msg *msg;

	netdev = netdev_find(ifindex);

	l_debug("%d", netdev->index);

	if (crypto_cipher_key_len(cipher) != igtk_len) {
		l_error("Unexpected key length: %d", igtk_len);
		netdev_setting_keys_failed(netdev,
					MPDU_REASON_CODE_INVALID_GROUP_CIPHER);
		return;
	}

	switch (cipher) {
	case CRYPTO_CIPHER_BIP:
		memcpy(igtk_buf, igtk, 16);
		break;
	default:
		l_error("Unexpected cipher: %x", cipher);
		netdev_setting_keys_failed(netdev,
					MPDU_REASON_CODE_INVALID_GROUP_CIPHER);
		return;
	}

	msg = netdev_build_cmd_new_key_group(netdev, cipher, key_index,
						igtk_buf, igtk_len,
						ipn, ipn_len);
	netdev->group_management_new_key_cmd_id =
			l_genl_family_send(nl80211, msg,
				netdev_new_group_management_key_cb,
				netdev, NULL);

	if (netdev->group_management_new_key_cmd_id > 0)
		return;

	l_genl_msg_unref(msg);
	netdev_setting_keys_failed(netdev, MPDU_REASON_CODE_UNSPECIFIED);
}

static void netdev_set_pairwise_key_cb(struct l_genl_msg *msg, void *data)
{
	struct netdev *netdev = data;

	netdev->pairwise_set_key_cmd_id = 0;

	if (l_genl_msg_get_error(msg) >= 0)
		return;

	l_error("Set Key for Pairwise Key failed for ifindex: %d",
								netdev->index);
	netdev_setting_keys_failed(netdev, MPDU_REASON_CODE_UNSPECIFIED);
}

static struct l_genl_msg *netdev_build_cmd_set_key_pairwise(
							struct netdev *netdev)
{
	uint8_t key_id = 0;
	struct l_genl_msg *msg;

	msg = l_genl_msg_new_sized(NL80211_CMD_SET_KEY, 512);

	l_genl_msg_append_attr(msg, NL80211_ATTR_KEY_IDX, 1, &key_id);
	l_genl_msg_append_attr(msg, NL80211_ATTR_IFINDEX, 4, &netdev->index);

	l_genl_msg_append_attr(msg, NL80211_ATTR_KEY_DEFAULT, 0, NULL);

	l_genl_msg_enter_nested(msg, NL80211_ATTR_KEY_DEFAULT_TYPES);
	l_genl_msg_append_attr(msg, NL80211_KEY_DEFAULT_TYPE_UNICAST, 0, NULL);
	l_genl_msg_leave_nested(msg);

	return msg;
}

static void netdev_new_pairwise_key_cb(struct l_genl_msg *msg, void *data)
{
	struct netdev *netdev = data;

	netdev->pairwise_new_key_cmd_id = 0;

	if (l_genl_msg_get_error(msg) >= 0)
		return;

	l_error("New Key for Pairwise Key failed for ifindex: %d",
								netdev->index);
	netdev_setting_keys_failed(netdev, MPDU_REASON_CODE_UNSPECIFIED);
}

static struct l_genl_msg *netdev_build_cmd_new_key_pairwise(
							struct netdev *netdev,
							uint32_t cipher,
							const uint8_t *aa,
							const uint8_t *tk,
							size_t tk_len)
{
	uint8_t key_id = 0;
	struct l_genl_msg *msg;

	msg = l_genl_msg_new_sized(NL80211_CMD_NEW_KEY, 512);

	l_genl_msg_append_attr(msg, NL80211_ATTR_KEY_DATA, tk_len, tk);
	l_genl_msg_append_attr(msg, NL80211_ATTR_KEY_CIPHER, 4, &cipher);
	l_genl_msg_append_attr(msg, NL80211_ATTR_MAC, ETH_ALEN, aa);
	l_genl_msg_append_attr(msg, NL80211_ATTR_KEY_IDX, 1, &key_id);
	l_genl_msg_append_attr(msg, NL80211_ATTR_IFINDEX, 4, &netdev->index);

	return msg;
}

static void netdev_set_tk(uint32_t ifindex, const uint8_t *aa,
				const uint8_t *tk, uint32_t cipher,
				void *user_data)
{
	uint8_t tk_buf[32];
	struct netdev *netdev;
	struct l_genl_msg *msg;

	netdev = netdev_find(ifindex);
	if (!netdev)
		return;

	l_debug("%d", netdev->index);

	if (netdev->event_filter)
		netdev->event_filter(netdev, NETDEV_EVENT_SETTING_KEYS,
					netdev->user_data);

	switch (cipher) {
	case CRYPTO_CIPHER_CCMP:
		memcpy(tk_buf, tk, 16);
		break;
	case CRYPTO_CIPHER_TKIP:
		/*
		 * Swap the TX and RX MIC key portions for supplicant.
		 * WPA_80211_v3_1_090922 doc's 3.3.4:
		 *   The MIC key used on the Client for transmit (TX) is in
		 *   bytes 24-31, and the MIC key used on the Client for
		 *   receive (RX) is in bytes 16-23 of the PTK.  That is,
		 *   assume that TX MIC and RX MIC referred to in Clause 8.7
		 *   are referenced to the Authenticator. Similarly, on the AP,
		 *   the MIC used for TX is in bytes 16-23, and the MIC key
		 *   used for RX is in bytes 24-31 of the PTK.
		 */
		memcpy(tk_buf, tk, 16);
		memcpy(tk_buf + 16, tk + 24, 8);
		memcpy(tk_buf + 24, tk + 16, 8);
		break;
	default:
		l_error("Unexpected cipher: %x", cipher);
		netdev_setting_keys_failed(netdev,
				MPDU_REASON_CODE_INVALID_PAIRWISE_CIPHER);
		return;
	}

	msg = netdev_build_cmd_new_key_pairwise(netdev, cipher, aa,
						tk_buf,
						crypto_cipher_key_len(cipher));
	netdev->pairwise_new_key_cmd_id =
		l_genl_family_send(nl80211, msg, netdev_new_pairwise_key_cb,
						netdev, NULL);
	if (!netdev->pairwise_new_key_cmd_id) {
		l_genl_msg_unref(msg);
		goto error;
	}

	msg = netdev_build_cmd_set_key_pairwise(netdev);

	netdev->pairwise_set_key_cmd_id =
		l_genl_family_send(nl80211, msg, netdev_set_pairwise_key_cb,
						netdev, NULL);
	if (netdev->pairwise_set_key_cmd_id > 0)
		return;

	l_genl_msg_unref(msg);
error:
	netdev_setting_keys_failed(netdev, MPDU_REASON_CODE_UNSPECIFIED);
}

static void netdev_handshake_failed(uint32_t ifindex,
					const uint8_t *aa, const uint8_t *spa,
					uint16_t reason_code, void *user_data)
{
	struct l_genl_msg *msg;
	struct netdev *netdev;

	netdev = netdev_find(ifindex);
	if (!netdev)
		return;

	l_error("4-Way Handshake failed for ifindex: %d", ifindex);

	netdev->sm = NULL;

	netdev->result = NETDEV_RESULT_HANDSHAKE_FAILED;
	msg = netdev_build_cmd_deauthenticate(netdev, reason_code);
	netdev->disconnect_cmd_id = l_genl_family_send(nl80211, msg,
						netdev_connect_failed,
						netdev, NULL);
}

static void hardware_rekey_cb(struct l_genl_msg *msg, void *data)
{
	struct netdev *netdev = data;
	int err;

	err = l_genl_msg_get_error(msg);
	if (err < 0) {
		if (err == -EOPNOTSUPP) {
			l_error("hardware_rekey not supported");
			netdev->rekey_offload_support = false;
		}
	}
}

static struct l_genl_msg *netdev_build_cmd_replay_counter(struct netdev *netdev,
					const uint8_t *kek,
					const uint8_t *kck,
					uint64_t replay_ctr)
{
	struct l_genl_msg *msg;

	msg = l_genl_msg_new_sized(NL80211_CMD_SET_REKEY_OFFLOAD, 512);

	l_genl_msg_append_attr(msg, NL80211_ATTR_IFINDEX, 4, &netdev->index);

	l_genl_msg_enter_nested(msg, NL80211_ATTR_REKEY_DATA);
	l_genl_msg_append_attr(msg, NL80211_REKEY_DATA_KEK,
					NL80211_KEK_LEN, kek);
	l_genl_msg_append_attr(msg, NL80211_REKEY_DATA_KCK,
					NL80211_KCK_LEN, kck);
	l_genl_msg_append_attr(msg, NL80211_REKEY_DATA_REPLAY_CTR,
			NL80211_REPLAY_CTR_LEN, &replay_ctr);

	l_genl_msg_leave_nested(msg);

	return msg;
}

static void netdev_set_rekey_offload(uint32_t ifindex,
					const uint8_t *kek,
					const uint8_t *kck,
					uint64_t replay_counter,
					void *user_data)
{
	struct netdev *netdev;
	struct l_genl_msg *msg;

	netdev = netdev_find(ifindex);
	if (!netdev)
		return;

	if (!netdev->rekey_offload_support)
		return;

	l_debug("%d", netdev->index);
	msg = netdev_build_cmd_replay_counter(netdev, kek, kck,
					replay_counter);
	l_genl_family_send(nl80211, msg, hardware_rekey_cb, netdev, NULL);

}

/*
 * Handle the Association Response IE contents either as part of an
 * FT initial Mobility Domain association (12.4) or a Fast Transition
 * (12.8.5).
 */
static bool netdev_handle_associate_resp_ies(struct handshake_state *hs,
					const uint8_t *rsne, const uint8_t *mde,
					const uint8_t *fte, bool transition)
{
	const uint8_t *sent_mde = hs->mde;
	bool is_rsn = hs->own_ie != NULL;

	/*
	 * During a transition in an RSN, check for an RSNE containing the
	 * PMK-R1-Name and the remaining fields same as in the advertised
	 * RSNE.
	 *
	 * 12.8.5: "The RSNE shall be present only if dot11RSNAActivated is
	 * true. If present, the RSNE shall be set as follows:
	 * — Version field shall be set to 1.
	 * — PMKID Count field shall be set to 1.
	 * — PMKID field shall contain the PMKR1Name
	 * — All other fields shall be identical to the contents of the RSNE
	 *   advertised by the target AP in Beacon and Probe Response frames."
	 */
	if (transition && is_rsn) {
		struct ie_rsn_info msg4_rsne;

		if (!rsne)
			return false;

		if (ie_parse_rsne_from_data(rsne, rsne[1] + 2,
						&msg4_rsne) < 0)
			return false;

		if (msg4_rsne.num_pmkids != 1 ||
				memcmp(msg4_rsne.pmkids, hs->pmk_r1_name, 16))
			return false;

		if (!handshake_util_ap_ie_matches(rsne, hs->ap_ie, false))
			return false;
	} else {
		if (rsne)
			return false;
	}

	/* An MD IE identical to the one we sent must be present */
	if (sent_mde && (!mde || memcmp(sent_mde, mde, sent_mde[1] + 2)))
		return false;

	/*
	 * An FT IE is required in an initial mobility domain
	 * association and re-associations in an RSN but not present
	 * in a non-RSN (12.4.2 vs. 12.4.3).
	 */
	if (sent_mde && is_rsn && !fte)
		return false;
	if (!(sent_mde && is_rsn) && fte)
		return false;

	if (fte) {
		struct ie_ft_info ft_info;

		if (ie_parse_fast_bss_transition_from_data(fte, fte[1] + 2,
								&ft_info) < 0)
			return false;

		/* Validate the FTE contents */
		if (transition) {
			/*
			 * In an RSN, check for an FT IE with the same
			 * R0KH-ID, R1KH-ID, ANonce and SNonce that we
			 * received in message 2, MIC Element Count
			 * of 6 and the correct MIC.
			 */
			uint8_t mic[16];

			if (!ft_calculate_fte_mic(hs, 6, rsne, fte, NULL, mic))
				return false;

			if (ft_info.mic_element_count != 3 ||
					memcmp(ft_info.mic, mic, 16))
				return false;

			if (hs->r0khid_len != ft_info.r0khid_len ||
					memcmp(hs->r0khid, ft_info.r0khid,
						hs->r0khid_len) ||
					!ft_info.r1khid_present ||
					memcmp(hs->r1khid, ft_info.r1khid, 6))
				return false;

			if (memcmp(ft_info.anonce, hs->anonce, 32))
				return false;

			if (memcmp(ft_info.snonce, hs->snonce, 32))
				return false;

			if (ft_info.gtk_len) {
				uint8_t gtk[32];

				if (!handshake_decode_fte_key(hs, ft_info.gtk,
								ft_info.gtk_len,
								gtk))
					return false;

				if (ft_info.gtk_rsc[6] != 0x00 ||
						ft_info.gtk_rsc[7] != 0x00)
					return false;

				handshake_state_install_gtk(hs,
							ft_info.gtk_key_id,
							gtk, ft_info.gtk_len,
							ft_info.gtk_rsc, 6);
			}

			if (ft_info.igtk_len) {
				uint8_t igtk[16];

				if (!handshake_decode_fte_key(hs, ft_info.igtk,
							ft_info.igtk_len, igtk))
					return false;

				handshake_state_install_igtk(hs,
							ft_info.igtk_key_id,
							igtk, ft_info.igtk_len,
							ft_info.igtk_ipn);
			}
		} else {
			/* Initial MD association */

			uint8_t zeros[32] = {};

			handshake_state_set_fte(hs, fte);

			/*
			 * 12.4.2: "The FTE shall have a MIC information
			 * element count of zero (i.e., no MIC present)
			 * and have ANonce, SNonce, and MIC fields set to 0."
			 */
			if (ft_info.mic_element_count != 0 ||
					memcmp(ft_info.mic, zeros, 16) ||
					memcmp(ft_info.anonce, zeros, 32) ||
					memcmp(ft_info.snonce, zeros, 32))
				return false;

			handshake_state_set_kh_ids(hs, ft_info.r0khid,
							ft_info.r0khid_len,
							ft_info.r1khid);
		}
	}

	return true;
}

static void netdev_connect_event(struct l_genl_msg *msg,
							struct netdev *netdev)
{
	struct l_genl_attr attr;
	uint16_t type, len;
	const void *data;
	const uint16_t *status_code = NULL;
	const uint8_t *ies = NULL;
	size_t ies_len;
	const uint8_t *rsne = NULL;
	const uint8_t *mde = NULL;
	const uint8_t *fte = NULL;
	bool is_rsn = netdev->handshake->own_ie != NULL;

	l_debug("");

	if (!netdev->connected)
		return;

	if (!l_genl_attr_init(&attr, msg)) {
		l_debug("attr init failed");
		goto error;
	}

	while (l_genl_attr_next(&attr, &type, &len, &data)) {
		switch (type) {
		case NL80211_ATTR_TIMED_OUT:
			l_warn("authentication timed out");
			goto error;
		case NL80211_ATTR_STATUS_CODE:
			if (len == sizeof(uint16_t))
				status_code = data;

			break;
		case NL80211_ATTR_RESP_IE:
			ies = data;
			ies_len = len;
			break;
		}
	}

	/* AP Rejected the authenticate / associate */
	if (!status_code || *status_code != 0)
		goto error;

	/* Check 802.11r IEs */
	if (ies) {
		struct ie_tlv_iter iter;

		ie_tlv_iter_init(&iter, ies, ies_len);

		while (ie_tlv_iter_next(&iter)) {
			switch (ie_tlv_iter_get_tag(&iter)) {
			case IE_TYPE_RSN:
				if (rsne)
					goto error;

				rsne = ie_tlv_iter_get_data(&iter) - 2;
				break;

			case IE_TYPE_MOBILITY_DOMAIN:
				if (mde)
					goto error;

				mde = ie_tlv_iter_get_data(&iter) - 2;
				break;

			case IE_TYPE_FAST_BSS_TRANSITION:
				if (fte)
					goto error;

				fte = ie_tlv_iter_get_data(&iter) - 2;
				break;
			}
		}
	}

	if (!netdev_handle_associate_resp_ies(netdev->handshake, rsne, mde, fte,
						netdev->in_ft))
		goto error;

	if (netdev->sm) {
		/*
		 * Start processing EAPoL frames now that the state machine
		 * has all the input data even in FT mode.
		 */
		eapol_start(netdev->sm);
	}

	if (netdev->in_ft) {
		netdev->in_ft = false;
		netdev->operational = true;

		if (is_rsn)
			handshake_state_install_ptk(netdev->handshake);

		if (netdev->connect_cb) {
			netdev->connect_cb(netdev, NETDEV_RESULT_OK,
						netdev->user_data);
			netdev->connect_cb = NULL;
		}
	} else if (is_rsn) {
		if (netdev->event_filter)
			netdev->event_filter(netdev,
					NETDEV_EVENT_4WAY_HANDSHAKE,
					netdev->user_data);
	} else
		netdev_set_linkmode_and_operstate(netdev->index, 1, IF_OPER_UP,
						netdev_operstate_cb, netdev);

	return;

error:
	netdev->result = NETDEV_RESULT_ASSOCIATION_FAILED;
	netdev_connect_failed(NULL, netdev);
}

/*
 * Build an FT Reassociation Request frame according to 12.5.2 / 12.5.4:
 * RSN or non-RSN Over-the-air FT Protocol, and with the IE contents
 * according to 12.8.4: FT authentication sequence: contents of third message.
 */
static struct l_genl_msg *netdev_build_cmd_ft_reassociate(struct netdev *netdev,
						uint32_t frequency,
						const uint8_t *prev_bssid)
{
	struct l_genl_msg *msg;
	struct iovec iov[3];
	int iov_elems = 0;
	struct handshake_state *hs = netdev_get_handshake(netdev);
	bool is_rsn = hs->own_ie != NULL;
	uint8_t *rsne = NULL;

	msg = l_genl_msg_new_sized(NL80211_CMD_ASSOCIATE, 600);
	l_genl_msg_append_attr(msg, NL80211_ATTR_IFINDEX, 4, &netdev->index);
	l_genl_msg_append_attr(msg, NL80211_ATTR_WIPHY_FREQ, 4, &frequency);
	l_genl_msg_append_attr(msg, NL80211_ATTR_MAC, ETH_ALEN, hs->aa);
	l_genl_msg_append_attr(msg, NL80211_ATTR_PREV_BSSID, ETH_ALEN,
				prev_bssid);
	l_genl_msg_append_attr(msg, NL80211_ATTR_SSID, hs->ssid_len, hs->ssid);

	if (is_rsn) {
		uint32_t nl_cipher;
		uint32_t nl_akm;
		uint32_t wpa_version;
		struct ie_rsn_info rsn_info;

		if (hs->pairwise_cipher == IE_RSN_CIPHER_SUITE_CCMP)
			nl_cipher = CRYPTO_CIPHER_CCMP;
		else
			nl_cipher = CRYPTO_CIPHER_TKIP;

		l_genl_msg_append_attr(msg, NL80211_ATTR_CIPHER_SUITES_PAIRWISE,
					4, &nl_cipher);

		if (hs->group_cipher == IE_RSN_CIPHER_SUITE_CCMP)
			nl_cipher = CRYPTO_CIPHER_CCMP;
		else
			nl_cipher = CRYPTO_CIPHER_TKIP;

		l_genl_msg_append_attr(msg, NL80211_ATTR_CIPHER_SUITE_GROUP,
					4, &nl_cipher);

		if (hs->mfp) {
			uint32_t use_mfp = NL80211_MFP_REQUIRED;
			l_genl_msg_append_attr(msg, NL80211_ATTR_USE_MFP,
								4, &use_mfp);
		}

		if (hs->akm_suite == IE_RSN_AKM_SUITE_FT_OVER_8021X)
			nl_akm = CRYPTO_AKM_FT_OVER_8021X;
		else
			nl_akm = CRYPTO_AKM_FT_USING_PSK;

		l_genl_msg_append_attr(msg, NL80211_ATTR_AKM_SUITES,
							4, &nl_akm);

		wpa_version = NL80211_WPA_VERSION_2;
		l_genl_msg_append_attr(msg, NL80211_ATTR_WPA_VERSIONS,
						4, &wpa_version);

		l_genl_msg_append_attr(msg, NL80211_ATTR_CONTROL_PORT, 0, NULL);

		/*
		 * Rebuild the RSNE to include the PMKR1Name and append
		 * MDE + FTE.
		 *
		 * 12.8.4: "If present, the RSNE shall be set as follows:
		 * — Version field shall be set to 1.
		 * — PMKID Count field shall be set to 1.
		 * — PMKID field shall contain the PMKR1Name.
		 * — All other fields shall be as specified in 8.4.2.27
		 *   and 11.5.3."
		 */
		if (ie_parse_rsne_from_data(hs->own_ie, hs->own_ie[1] + 2,
						&rsn_info) < 0)
			goto error;

		rsn_info.num_pmkids = 1;
		rsn_info.pmkids = hs->pmk_r1_name;

		rsne = alloca(256);
		ie_build_rsne(&rsn_info, rsne);

		iov[iov_elems].iov_base = rsne;
		iov[iov_elems].iov_len = rsne[1] + 2;
		iov_elems += 1;
	}

	/* The MDE advertised by the BSS must be passed verbatim */
	iov[iov_elems].iov_base = (void *) hs->mde;
	iov[iov_elems].iov_len = hs->mde[1] + 2;
	iov_elems += 1;

	if (is_rsn) {
		struct ie_ft_info ft_info;
		uint8_t *fte;

		/*
		 * 12.8.4: "If present, the FTE shall be set as follows:
		 * — ANonce, SNonce, R0KH-ID, and R1KH-ID shall be set to
		 *   the values contained in the second message of this
		 *   sequence.
		 * — The Element Count field of the MIC Control field shall
		 *   be set to the number of elements protected in this
		 *   frame (variable).
		 * [...]
		 * — All other fields shall be set to 0."
		 */

		memset(&ft_info, 0, sizeof(ft_info));

		ft_info.mic_element_count = 3;
		memcpy(ft_info.r0khid, hs->r0khid, hs->r0khid_len);
		ft_info.r0khid_len = hs->r0khid_len;
		memcpy(ft_info.r1khid, hs->r1khid, 6);
		ft_info.r1khid_present = true;
		memcpy(ft_info.anonce, hs->anonce, 32);
		memcpy(ft_info.snonce, hs->snonce, 32);

		fte = alloca(256);
		ie_build_fast_bss_transition(&ft_info, fte);

		if (!ft_calculate_fte_mic(hs, 5, rsne, fte, NULL, ft_info.mic))
			goto error;

		/* Rebuild the FT IE now with the MIC included */
		ie_build_fast_bss_transition(&ft_info, fte);

		iov[iov_elems].iov_base = fte;
		iov[iov_elems].iov_len = fte[1] + 2;
		iov_elems += 1;
	}

	l_genl_msg_append_attrv(msg, NL80211_ATTR_IE, iov, iov_elems);

	return msg;

error:
	l_genl_msg_unref(msg);

	return NULL;
}

static void netdev_cmd_ft_reassociate_cb(struct l_genl_msg *msg,
						void *user_data)
{
	struct netdev *netdev = user_data;

	netdev->connect_cmd_id = 0;

	if (l_genl_msg_get_error(msg) < 0) {
		struct l_genl_msg *cmd_deauth;

		netdev->result = NETDEV_RESULT_ASSOCIATION_FAILED;
		cmd_deauth = netdev_build_cmd_deauthenticate(netdev,
						MPDU_REASON_CODE_UNSPECIFIED);
		netdev->disconnect_cmd_id = l_genl_family_send(nl80211,
							cmd_deauth,
							netdev_connect_failed,
							netdev, NULL);
	}
}

static void netdev_authenticate_event(struct l_genl_msg *msg,
							struct netdev *netdev)
{
	struct l_genl_msg *cmd_associate, *cmd_deauth;
	struct l_genl_attr attr;
	uint16_t type, len;
	const void *data;
	uint16_t status_code;
	const uint8_t *ies = NULL;
	size_t ies_len;
	const uint8_t *frame = NULL;
	size_t frame_len = 0;
	struct ie_tlv_iter iter;
	const uint8_t *rsne = NULL;
	const uint8_t *mde = NULL;
	const uint8_t *fte = NULL;
	struct handshake_state *hs = netdev->handshake;
	bool is_rsn = hs->own_ie != NULL;

	l_debug("");

	/*
	 * During Fast Transition we use the authenticate event to start the
	 * reassociation step because the FTE necessary before we can build
	 * the FT Associate command is included in the attached frame and is
	 * not available in the Authenticate command callback.
	 */
	if (!netdev->in_ft)
		return;

	if (!l_genl_attr_init(&attr, msg)) {
		l_debug("attr init failed");

		goto auth_error;
	}

	while (l_genl_attr_next(&attr, &type, &len, &data)) {
		switch (type) {
		case NL80211_ATTR_TIMED_OUT:
			l_warn("authentication timed out");

			goto auth_error;

		case NL80211_ATTR_FRAME:
			if (frame)
				goto auth_error;

			frame = data;
			frame_len = len;
			break;
		}
	}

	if (!frame)
		goto auth_error;

	/*
	 * Parse the Authentication Response and validate the contents
	 * according to 12.5.2 / 12.5.4: RSN or non-RSN Over-the-air
	 * FT Protocol.
	 */
	if (!ft_parse_authentication_resp_frame(frame, frame_len,
					netdev->addr, hs->aa, hs->aa, 2,
					&status_code, &ies, &ies_len))
		goto auth_error;

	/* AP Rejected the authenticate / associate */
	if (status_code != 0)
		goto auth_error;

	/* Check 802.11r IEs */
	if (!ies)
		goto ft_error;

	ie_tlv_iter_init(&iter, ies, ies_len);

	while (ie_tlv_iter_next(&iter)) {
		switch (ie_tlv_iter_get_tag(&iter)) {
		case IE_TYPE_RSN:
			if (rsne)
				goto ft_error;

			rsne = ie_tlv_iter_get_data(&iter) - 2;
			break;

		case IE_TYPE_MOBILITY_DOMAIN:
			if (mde)
				goto ft_error;

			mde = ie_tlv_iter_get_data(&iter) - 2;
			break;

		case IE_TYPE_FAST_BSS_TRANSITION:
			if (fte)
				goto ft_error;

			fte = ie_tlv_iter_get_data(&iter) - 2;
			break;
		}
	}

	/*
	 * In an RSN, check for an RSNE containing the PMK-R0-Name and
	 * the remaining fields same as in the advertised RSNE.
	 *
	 * 12.8.3: "The RSNE shall be present only if dot11RSNAActivated
	 * is true. If present, the RSNE shall be set as follows:
	 * — Version field shall be set to 1.
	 * — PMKID Count field shall be set to 1.
	 * — PMKID List field shall be set to the value contained in the
	 *   first message of this sequence.
	 * — All other fields shall be identical to the contents of the
	 *   RSNE advertised by the AP in Beacon and Probe Response frames."
	 */
	if (is_rsn) {
		struct ie_rsn_info msg2_rsne;

		if (!rsne)
			goto ft_error;

		if (ie_parse_rsne_from_data(rsne, rsne[1] + 2,
						&msg2_rsne) < 0)
			goto ft_error;

		if (msg2_rsne.num_pmkids != 1 ||
				memcmp(msg2_rsne.pmkids, hs->pmk_r0_name, 16))
			goto ft_error;

		if (!handshake_util_ap_ie_matches(rsne, hs->ap_ie, false))
			goto ft_error;
	} else if (rsne)
		goto ft_error;

	/*
	 * Check for an MD IE identical to the one we sent in message 1
	 *
	 * 12.8.3: "The MDE shall contain the MDID and FT Capability and
	 * Policy fields. This element shall be the same as the MDE
	 * advertised by the target AP in Beacon and Probe Response frames."
	 */
	if (!mde || memcmp(hs->mde, mde, hs->mde[1] + 2))
		goto ft_error;

	/*
	 * In an RSN, check for an FT IE with the same R0KH-ID and the same
	 * SNonce that we sent, and check that the R1KH-ID and the ANonce
	 * are present.  Use them to generate new PMK-R1, PMK-R1-Name and PTK
	 * in handshake.c.
	 *
	 * 12.8.3: "The FTE shall be present only if dot11RSNAActivated is
	 * true. If present, the FTE shall be set as follows:
	 * — R0KH-ID shall be identical to the R0KH-ID provided by the FTO
	 *   in the first message.
	 * — R1KH-ID shall be set to the R1KH-ID of the target AP, from
	 *   dot11FTR1KeyHolderID.
	 * — ANonce shall be set to a value chosen randomly by the target AP,
	 *   following the recommendations of 11.6.5.
	 * — SNonce shall be set to the value contained in the first message
	 *   of this sequence.
	 * — All other fields shall be set to 0."
	 */
	if (is_rsn) {
		struct ie_ft_info ft_info;
		uint8_t zeros[16] = {};

		if (!fte)
			goto ft_error;

		if (ie_parse_fast_bss_transition_from_data(fte, fte[1] + 2,
								&ft_info) < 0)
			goto ft_error;

		if (ft_info.mic_element_count != 0 ||
				memcmp(ft_info.mic, zeros, 16))
			goto ft_error;

		if (hs->r0khid_len != ft_info.r0khid_len ||
				memcmp(hs->r0khid, ft_info.r0khid,
					hs->r0khid_len) ||
				!ft_info.r1khid_present)
			goto ft_error;

		if (memcmp(ft_info.snonce, hs->snonce, 32))
			goto ft_error;

		handshake_state_set_fte(hs, fte);

		handshake_state_set_anonce(hs, ft_info.anonce);

		handshake_state_set_kh_ids(hs, ft_info.r0khid,
						ft_info.r0khid_len,
						ft_info.r1khid);

		handshake_state_derive_ptk(hs);
	} else if (fte)
		goto ft_error;

	cmd_associate = netdev_build_cmd_ft_reassociate(netdev,
							netdev->frequency,
							netdev->prev_bssid);
	if (!cmd_associate)
		goto ft_error;

	netdev->connect_cmd_id = l_genl_family_send(nl80211,
						cmd_associate,
						netdev_cmd_ft_reassociate_cb,
						netdev, NULL);
	if (!netdev->connect_cmd_id) {
		l_genl_msg_unref(cmd_associate);

		goto ft_error;
	}

	if (netdev->sm)
		eapol_register(netdev->sm); /* See netdev_cmd_connect_cb */

	return;

auth_error:
	netdev->result = NETDEV_RESULT_AUTHENTICATION_FAILED;
	netdev_connect_failed(NULL, netdev);
	return;

ft_error:
	netdev->result = NETDEV_RESULT_AUTHENTICATION_FAILED;
	cmd_deauth = netdev_build_cmd_deauthenticate(netdev,
						MPDU_REASON_CODE_UNSPECIFIED);
	netdev->disconnect_cmd_id = l_genl_family_send(nl80211, cmd_deauth,
							netdev_connect_failed,
							netdev, NULL);
}

static void netdev_associate_event(struct l_genl_msg *msg,
							struct netdev *netdev)
{
	l_debug("");
}

static unsigned int ie_rsn_akm_suite_to_nl80211(enum ie_rsn_akm_suite akm)
{
	switch (akm) {
	case IE_RSN_AKM_SUITE_8021X:
		return CRYPTO_AKM_8021X;
	case IE_RSN_AKM_SUITE_PSK:
		return CRYPTO_AKM_PSK;
	case IE_RSN_AKM_SUITE_FT_OVER_8021X:
		return CRYPTO_AKM_FT_OVER_8021X;
	case IE_RSN_AKM_SUITE_FT_USING_PSK:
		return CRYPTO_AKM_FT_USING_PSK;
	case IE_RSN_AKM_SUITE_8021X_SHA256:
		return CRYPTO_AKM_8021X_SHA256;
	case IE_RSN_AKM_SUITE_PSK_SHA256:
		return CRYPTO_AKM_PSK_SHA256;
	case IE_RSN_AKM_SUITE_TDLS:
		return CRYPTO_AKM_TDLS;
	case IE_RSN_AKM_SUITE_SAE_SHA256:
		return CRYPTO_AKM_SAE_SHA256;
	case IE_RSN_AKM_SUITE_FT_OVER_SAE_SHA256:
		return CRYPTO_AKM_FT_OVER_SAE_SHA256;
	}

	return 0;
}

static void netdev_cmd_connect_cb(struct l_genl_msg *msg, void *user_data)
{
	struct netdev *netdev = user_data;

	netdev->connect_cmd_id = 0;

	/* Wait for connect event */
	if (l_genl_msg_get_error(msg) >= 0) {
		if (netdev->event_filter)
			netdev->event_filter(netdev,
						NETDEV_EVENT_ASSOCIATING,
						netdev->user_data);

		/*
		 * We register the eapol state machine here, in case the PAE
		 * socket receives EAPoL packets before the nl80211 socket
		 * receives the connected event.  The logical sequence of
		 * events can be reversed (e.g. connect_event, then PAE data)
		 * due to scheduling
		 */
		if (netdev->sm)
			eapol_register(netdev->sm);

		return;
	}

	netdev->result = NETDEV_RESULT_ASSOCIATION_FAILED;
	netdev_connect_failed(NULL, netdev);
}

static struct l_genl_msg *netdev_build_cmd_connect(struct netdev *netdev,
						struct scan_bss *bss,
						struct handshake_state *hs)
{
	uint32_t auth_type = NL80211_AUTHTYPE_OPEN_SYSTEM;
	struct l_genl_msg *msg;
	struct iovec iov[2];
	int iov_elems = 0;
	bool is_rsn = hs->own_ie != NULL;

	msg = l_genl_msg_new_sized(NL80211_CMD_CONNECT, 512);
	l_genl_msg_append_attr(msg, NL80211_ATTR_IFINDEX, 4, &netdev->index);
	l_genl_msg_append_attr(msg, NL80211_ATTR_WIPHY_FREQ,
						4, &bss->frequency);
	l_genl_msg_append_attr(msg, NL80211_ATTR_MAC, ETH_ALEN, bss->addr);
	l_genl_msg_append_attr(msg, NL80211_ATTR_SSID,
						bss->ssid_len, bss->ssid);
	l_genl_msg_append_attr(msg, NL80211_ATTR_AUTH_TYPE, 4, &auth_type);

	if (bss->capability & IE_BSS_CAP_PRIVACY)
		l_genl_msg_append_attr(msg, NL80211_ATTR_PRIVACY, 0, NULL);

	if (is_rsn) {
		uint32_t nl_cipher;
		uint32_t nl_akm;
		uint32_t wpa_version;

		if (hs->pairwise_cipher == IE_RSN_CIPHER_SUITE_CCMP)
			nl_cipher = CRYPTO_CIPHER_CCMP;
		else
			nl_cipher = CRYPTO_CIPHER_TKIP;

		l_genl_msg_append_attr(msg, NL80211_ATTR_CIPHER_SUITES_PAIRWISE,
					4, &nl_cipher);

		if (hs->group_cipher == IE_RSN_CIPHER_SUITE_CCMP)
			nl_cipher = CRYPTO_CIPHER_CCMP;
		else
			nl_cipher = CRYPTO_CIPHER_TKIP;

		l_genl_msg_append_attr(msg, NL80211_ATTR_CIPHER_SUITE_GROUP,
					4, &nl_cipher);

		if (hs->mfp) {
			uint32_t use_mfp = NL80211_MFP_REQUIRED;
			l_genl_msg_append_attr(msg, NL80211_ATTR_USE_MFP,
								4, &use_mfp);
		}

		nl_akm = ie_rsn_akm_suite_to_nl80211(hs->akm_suite);
		if (nl_akm)
			l_genl_msg_append_attr(msg, NL80211_ATTR_AKM_SUITES,
							4, &nl_akm);

		if (hs->wpa_ie)
			wpa_version = NL80211_WPA_VERSION_1;
		else
			wpa_version = NL80211_WPA_VERSION_2;

		l_genl_msg_append_attr(msg, NL80211_ATTR_WPA_VERSIONS,
						4, &wpa_version);

		l_genl_msg_append_attr(msg, NL80211_ATTR_CONTROL_PORT, 0, NULL);

		iov[iov_elems].iov_base = (void *) hs->own_ie;
		iov[iov_elems].iov_len = hs->own_ie[1] + 2;
		iov_elems += 1;
	}

	if (hs->mde) {
		iov[iov_elems].iov_base = (void *) hs->mde;
		iov[iov_elems].iov_len = hs->mde[1] + 2;
		iov_elems += 1;
	}

	if (iov_elems)
		l_genl_msg_append_attrv(msg, NL80211_ATTR_IE, iov, iov_elems);

	return msg;
}

static int netdev_connect_common(struct netdev *netdev,
					struct l_genl_msg *cmd_connect,
					struct scan_bss *bss,
					struct handshake_state *hs,
					struct eapol_sm *sm,
					netdev_event_func_t event_filter,
					netdev_connect_cb_t cb, void *user_data)
{
	netdev->connect_cmd_id = l_genl_family_send(nl80211, cmd_connect,
							netdev_cmd_connect_cb,
							netdev, NULL);

	if (!netdev->connect_cmd_id) {
		l_genl_msg_unref(cmd_connect);
		return -EIO;
	}

	netdev->event_filter = event_filter;
	netdev->connect_cb = cb;
	netdev->user_data = user_data;
	netdev->connected = true;
	netdev->handshake = hs;
	netdev->sm = sm;
	netdev->frequency = bss->frequency;

	handshake_state_set_authenticator_address(hs, bss->addr);
	handshake_state_set_supplicant_address(hs, netdev->addr);

	return 0;
}

int netdev_connect(struct netdev *netdev, struct scan_bss *bss,
				struct handshake_state *hs,
				netdev_event_func_t event_filter,
				netdev_connect_cb_t cb, void *user_data)
{
	struct l_genl_msg *cmd_connect;
	struct eapol_sm *sm = NULL;
	bool is_rsn = hs->own_ie != NULL;

	if (netdev->connected)
		return -EISCONN;

	cmd_connect = netdev_build_cmd_connect(netdev, bss, hs);
	if (!cmd_connect)
		return -EINVAL;

	if (is_rsn)
		sm = eapol_sm_new(hs);

	return netdev_connect_common(netdev, cmd_connect, bss, hs, sm,
						event_filter, cb, user_data);
}

int netdev_connect_wsc(struct netdev *netdev, struct scan_bss *bss,
				struct handshake_state *hs,
				netdev_event_func_t event_filter,
				netdev_connect_cb_t cb,
				netdev_eapol_event_func_t eapol_cb,
				void *user_data)
{
	struct l_genl_msg *cmd_connect;
	struct wsc_association_request request;
	uint8_t *pdu;
	size_t pdu_len;
	void *ie;
	size_t ie_len;
	struct eapol_sm *sm;

	if (netdev->connected)
		return -EISCONN;

	cmd_connect = netdev_build_cmd_connect(netdev, bss, hs);
	if (!cmd_connect)
		return -EINVAL;

	request.version2 = true;
	request.request_type = WSC_REQUEST_TYPE_ENROLLEE_OPEN_8021X;

	pdu = wsc_build_association_request(&request, &pdu_len);
	if (!pdu)
		goto error;

	ie = ie_tlv_encapsulate_wsc_payload(pdu, pdu_len, &ie_len);
	l_free(pdu);

	if (!ie)
		goto error;

	l_genl_msg_append_attr(cmd_connect, NL80211_ATTR_IE, ie_len, ie);
	l_free(ie);

	sm = eapol_sm_new(hs);
	eapol_sm_set_user_data(sm, user_data);
	eapol_sm_set_event_func(sm, eapol_cb);

	return netdev_connect_common(netdev, cmd_connect, bss, hs, sm,
						event_filter, cb, user_data);

error:
	l_genl_msg_unref(cmd_connect);
	return -ENOMEM;
}

int netdev_disconnect(struct netdev *netdev,
				netdev_disconnect_cb_t cb, void *user_data)
{
	struct l_genl_msg *deauthenticate;

	if (!netdev->connected)
		return -ENOTCONN;

	if (netdev->disconnect_cmd_id)
		return -EINPROGRESS;

	/* Build deauthenticate prior to handshake_state being cleared */
	deauthenticate = netdev_build_cmd_deauthenticate(netdev,
					MPDU_REASON_CODE_DEAUTH_LEAVING);

	/* Only perform this if we haven't successfully fully associated yet */
	if (!netdev->operational) {
		netdev->result = NETDEV_RESULT_ABORTED;
		netdev_connect_failed(NULL, netdev);
	} else {
		netdev_connect_free(netdev);
	}

	netdev->disconnect_cmd_id = l_genl_family_send(nl80211, deauthenticate,
				netdev_cmd_deauthenticate_cb, netdev, NULL);

	if (!netdev->disconnect_cmd_id) {
		l_genl_msg_unref(deauthenticate);
		return -EIO;
	}

	netdev->disconnect_cb = cb;
	netdev->user_data = user_data;

	return 0;
}

/*
 * Build an FT Authentication Request frame according to 12.5.2 / 12.5.4:
 * RSN or non-RSN Over-the-air FT Protocol, with the IE contents
 * according to 12.8.2: FT authentication sequence: contents of first message.
 */
static struct l_genl_msg *netdev_build_cmd_ft_authenticate(
					struct netdev *netdev,
					const struct scan_bss *bss,
					const struct handshake_state *hs)
{
	uint32_t auth_type = NL80211_AUTHTYPE_FT;
	struct l_genl_msg *msg;
	struct iovec iov[3];
	int iov_elems = 0;
	bool is_rsn = hs->own_ie != NULL;
	uint8_t mde[5];

	msg = l_genl_msg_new_sized(NL80211_CMD_AUTHENTICATE, 512);
	l_genl_msg_append_attr(msg, NL80211_ATTR_IFINDEX, 4, &netdev->index);
	l_genl_msg_append_attr(msg, NL80211_ATTR_WIPHY_FREQ,
						4, &bss->frequency);
	l_genl_msg_append_attr(msg, NL80211_ATTR_MAC, ETH_ALEN, bss->addr);
	l_genl_msg_append_attr(msg, NL80211_ATTR_SSID,
						bss->ssid_len, bss->ssid);
	l_genl_msg_append_attr(msg, NL80211_ATTR_AUTH_TYPE, 4, &auth_type);

	if (is_rsn) {
		struct ie_rsn_info rsn_info;
		uint8_t *rsne;

		/*
		 * Rebuild the RSNE to include the PMKR0Name and append
		 * MDE + FTE.
		 *
		 * 12.8.2: "If present, the RSNE shall be set as follows:
		 * — Version field shall be set to 1.
		 * — PMKID Count field shall be set to 1.
		 * — PMKID List field shall contain the PMKR0Name.
		 * — All other fields shall be as specified in 8.4.2.27
		 *   and 11.5.3."
		 */
		if (ie_parse_rsne_from_data(hs->own_ie, hs->own_ie[1] + 2,
						&rsn_info) < 0)
			goto error;

		rsn_info.num_pmkids = 1;
		rsn_info.pmkids = hs->pmk_r0_name;

		rsne = alloca(256);
		ie_build_rsne(&rsn_info, rsne);

		iov[iov_elems].iov_base = rsne;
		iov[iov_elems].iov_len = rsne[1] + 2;
		iov_elems += 1;
	}

	/* The MDE advertised by the BSS must be passed verbatim */
	mde[0] = IE_TYPE_MOBILITY_DOMAIN;
	mde[1] = 3;
	memcpy(mde + 2, bss->mde, 3);

	iov[iov_elems].iov_base = mde;
	iov[iov_elems].iov_len = 5;
	iov_elems += 1;

	if (is_rsn) {
		struct ie_ft_info ft_info;
		uint8_t *fte;

		/*
		 * 12.8.2: "If present, the FTE shall be set as follows:
		 * — R0KH-ID shall be the value of R0KH-ID obtained by the
		 *   FTO during its FT initial mobility domain association
		 *   exchange.
		 * — SNonce shall be set to a value chosen randomly by the
		 *   FTO, following the recommendations of 11.6.5.
		 * — All other fields shall be set to 0."
		 */

		memset(&ft_info, 0, sizeof(ft_info));

		memcpy(ft_info.r0khid, hs->r0khid, hs->r0khid_len);
		ft_info.r0khid_len = hs->r0khid_len;

		memcpy(ft_info.snonce, hs->snonce, 32);

		fte = alloca(256);
		ie_build_fast_bss_transition(&ft_info, fte);

		iov[iov_elems].iov_base = fte;
		iov[iov_elems].iov_len = fte[1] + 2;
		iov_elems += 1;
	}

	l_genl_msg_append_attrv(msg, NL80211_ATTR_IE, iov, iov_elems);

	return msg;

error:
	l_genl_msg_unref(msg);

	return NULL;
}

static void netdev_cmd_authenticate_ft_cb(struct l_genl_msg *msg,
						void *user_data)
{
	struct netdev *netdev = user_data;

	netdev->connect_cmd_id = 0;

	if (l_genl_msg_get_error(msg) < 0) {
		netdev->result = NETDEV_RESULT_AUTHENTICATION_FAILED;
		netdev_connect_failed(NULL, netdev);
	}
}

int netdev_fast_transition(struct netdev *netdev, struct scan_bss *target_bss,
				netdev_connect_cb_t cb)
{
	struct l_genl_msg *cmd_authenticate;
	uint8_t orig_snonce[32];
	int err;

	if (!netdev->operational)
		return -ENOTCONN;

	if (!netdev->handshake->mde || !target_bss->mde_present ||
			l_get_le16(netdev->handshake->mde + 2) !=
			l_get_le16(target_bss->mde))
		return -EINVAL;

	/*
	 * We reuse the handshake_state object and reset what's needed.
	 * Could also create a new object and copy most of the state but
	 * we would end up doing more work.
	 */

	memcpy(orig_snonce, netdev->handshake->snonce, 32);
	handshake_state_new_snonce(netdev->handshake);

	cmd_authenticate = netdev_build_cmd_ft_authenticate(netdev, target_bss,
							netdev->handshake);
	if (!cmd_authenticate) {
		err = -EINVAL;
		goto restore_snonce;
	}

	netdev->connect_cmd_id = l_genl_family_send(nl80211,
						cmd_authenticate,
						netdev_cmd_authenticate_ft_cb,
						netdev, NULL);
	if (!netdev->connect_cmd_id) {
		l_genl_msg_unref(cmd_authenticate);
		err = -EIO;
		goto restore_snonce;
	}

	memcpy(netdev->prev_bssid, netdev->handshake->aa, ETH_ALEN);
	handshake_state_set_authenticator_address(netdev->handshake,
							target_bss->addr);
	handshake_state_set_ap_rsn(netdev->handshake, target_bss->rsne);
	memcpy(netdev->handshake->mde + 2, target_bss->mde, 3);

	if (netdev->sm) {
		eapol_sm_free(netdev->sm);

		netdev->sm = eapol_sm_new(netdev->handshake);
		eapol_sm_set_use_eapol_start(netdev->sm, false);
	}

	netdev->operational = false;
	netdev->in_ft = true;

	netdev->connect_cb = cb;
	netdev->frequency = target_bss->frequency;

	/*
	 * Cancel commands that could be running because of EAPoL activity
	 * like re-keying, this way the callbacks for those commands don't
	 * have to check if failures resulted from the transition.
	 */
	if (netdev->group_new_key_cmd_id) {
		l_genl_family_cancel(nl80211, netdev->group_new_key_cmd_id);
		netdev->group_new_key_cmd_id = 0;
	}

	if (netdev->group_management_new_key_cmd_id) {
		l_genl_family_cancel(nl80211,
			netdev->group_management_new_key_cmd_id);
		netdev->group_management_new_key_cmd_id = 0;
	}

	return 0;

restore_snonce:
	memcpy(netdev->handshake->snonce, orig_snonce, 32);

	return err;
}

static uint32_t netdev_send_action_frame(struct netdev *netdev,
					const uint8_t *to,
					const uint8_t *body, size_t body_len,
					l_genl_msg_func_t callback)
{
	struct l_genl_msg *msg;
	const uint16_t frame_type = 0x00d0;
	uint8_t action_frame[24 + body_len];
	uint32_t id;

	memset(action_frame, 0, 24);

	l_put_le16(frame_type, action_frame + 0);
	memcpy(action_frame + 4, to, 6);
	memcpy(action_frame + 10, netdev->addr, 6);
	memcpy(action_frame + 16, netdev->handshake->aa, 6);
	memcpy(action_frame + 24, body, body_len);

	msg = l_genl_msg_new_sized(NL80211_CMD_FRAME, 128 + body_len);

	l_genl_msg_append_attr(msg, NL80211_ATTR_IFINDEX, 4, &netdev->index);
	l_genl_msg_append_attr(msg, NL80211_ATTR_WIPHY_FREQ, 4,
				&netdev->frequency);
	l_genl_msg_append_attr(msg, NL80211_ATTR_FRAME, sizeof(action_frame),
				action_frame);

	id = l_genl_family_send(nl80211, msg, callback, netdev, NULL);

	if (!id)
		l_genl_msg_unref(msg);

	return id;
}

static void netdev_neighbor_report_req_cb(struct l_genl_msg *msg,
						void *user_data)
{
	struct netdev *netdev = user_data;

	if (!netdev->neighbor_report_cb)
		return;

	if (l_genl_msg_get_error(msg) < 0) {
		netdev->neighbor_report_cb(netdev, l_genl_msg_get_error(msg),
						NULL, 0, netdev->user_data);

		netdev->neighbor_report_cb = NULL;

		l_timeout_remove(netdev->neighbor_report_timeout);
	}
}

static void netdev_neighbor_report_timeout(struct l_timeout *timeout,
						void *user_data)
{
	struct netdev *netdev = user_data;

	netdev->neighbor_report_cb(netdev, -ETIMEDOUT, NULL, 0,
					netdev->user_data);

	netdev->neighbor_report_cb = NULL;

	l_timeout_remove(netdev->neighbor_report_timeout);
}

int netdev_neighbor_report_req(struct netdev *netdev,
				netdev_neighbor_report_cb_t cb)
{
	const uint8_t action_frame[] = {
		0x05, /* Category: Radio Measurement */
		0x04, /* Radio Measurement Action: Neighbor Report Request */
		0x01, /* Dialog Token: a non-zero value (unused) */
	};

	if (netdev->neighbor_report_cb || !netdev->connected)
		return -EBUSY;

	if (!netdev_send_action_frame(netdev, netdev->handshake->aa,
					action_frame, sizeof(action_frame),
					netdev_neighbor_report_req_cb))
		return -EIO;

	netdev->neighbor_report_cb = cb;

	/* Set a 3-second timeout */
	netdev->neighbor_report_timeout =
		l_timeout_create(3, netdev_neighbor_report_timeout,
					netdev, NULL);

	return 0;
}

static void netdev_radio_measurement_frame_event(struct netdev *netdev,
						const uint8_t *data, size_t len)
{
	uint8_t action;

	if (len < 2) {
		l_debug("Radio Measurement frame too short");
		return;
	}

	action = data[0];

	switch (action) {
	case 5: /* Neighbor Report Response */
		if (!netdev->neighbor_report_cb)
			break;

		/*
		 * Don't use the dialog token, return the first Neighbor
		 * Report Response received.
		 */

		netdev->neighbor_report_cb(netdev, 0, data + 2, len - 2,
						netdev->user_data);
		netdev->neighbor_report_cb = NULL;

		l_timeout_remove(netdev->neighbor_report_timeout);

		break;

	default:
		l_debug("Unknown radio measurement action %u received", action);
		break;
	}
}

static void netdev_action_frame_event(struct netdev *netdev,
					const uint8_t *data, size_t len)
{
	uint8_t category;

	if (len < 1) {
		l_debug("Action frame too short");
		return;
	}

	category = data[0];

	switch (category) {
	case 5: /* Radio Measurement */
		netdev_radio_measurement_frame_event(netdev, data + 1, len - 1);
		break;

	default:
		l_debug("Unknown action frame category %u received", category);
		break;
	}
}

static void netdev_mgmt_frame_event(struct l_genl_msg *msg,
					struct netdev *netdev)
{
	struct l_genl_attr attr;
	uint16_t type, len, body_len = 0;
	const void *data, *body = NULL;
	uint16_t frame_type;

	if (!l_genl_attr_init(&attr, msg))
		return;

	while (l_genl_attr_next(&attr, &type, &len, &data)) {
		switch (type) {
		case NL80211_ATTR_FRAME:
			if (body)
				return;

			body = data;
			body_len = len;
			break;
		}
	}

	if (!body || body_len < 25)
		return;

	frame_type = l_get_le16(body + 0);

	if (memcmp(body + 4, netdev->addr, 6))
		return;

	/* Is this a management frame */
	if (((frame_type >> 2) & 3) != 0) {
		l_debug("Unknown frame of type %04x received",
				(unsigned) frame_type);
		return;
	}

	switch ((frame_type >> 4) & 15) {
	case 0xd: /* Action frame */
		netdev_action_frame_event(netdev, body + 24, body_len - 24);
		break;

	default:
		l_debug("Unknown frame of type %04x received",
				(unsigned) frame_type);
		break;
	}
}

static void netdev_mlme_notify(struct l_genl_msg *msg, void *user_data)
{
	struct netdev *netdev = NULL;
	struct l_genl_attr attr;
	uint16_t type, len;
	const void *data;
	uint8_t cmd;

	cmd = l_genl_msg_get_command(msg);

	l_debug("MLME notification %u", cmd);

	if (!l_genl_attr_init(&attr, msg))
		return;

	while (l_genl_attr_next(&attr, &type, &len, &data)) {
		switch (type) {
		case NL80211_ATTR_IFINDEX:
			if (len != sizeof(uint32_t)) {
				l_warn("Invalid interface index attribute");
				return;
			}

			netdev = netdev_find(*((uint32_t *) data));
			break;
		}
	}

	if (!netdev) {
		l_warn("MLME notification is missing ifindex attribute");
		return;
	}

	switch (cmd) {
	case NL80211_CMD_AUTHENTICATE:
		netdev_authenticate_event(msg, netdev);
		break;
	case NL80211_CMD_DEAUTHENTICATE:
		netdev_deauthenticate_event(msg, netdev);
		break;
	case NL80211_CMD_ASSOCIATE:
		netdev_associate_event(msg, netdev);
		break;
	case NL80211_CMD_CONNECT:
		netdev_connect_event(msg, netdev);
		break;
	case NL80211_CMD_DISCONNECT:
		netdev_disconnect_event(msg, netdev);
		break;
	case NL80211_CMD_NOTIFY_CQM:
		netdev_cqm_event(msg, netdev);
		break;
	case NL80211_CMD_SET_REKEY_OFFLOAD:
		netdev_rekey_offload_event(msg, netdev);
		break;
	}
}

static void netdev_unicast_notify(struct l_genl_msg *msg, void *user_data)
{
	struct netdev *netdev = NULL;
	struct l_genl_attr attr;
	uint16_t type, len;
	const void *data;
	uint8_t cmd;

	cmd = l_genl_msg_get_command(msg);
	if (!cmd)
		return;

	l_debug("Unicast notification %u", cmd);

	if (!l_genl_attr_init(&attr, msg))
		return;

	while (l_genl_attr_next(&attr, &type, &len, &data)) {
		switch (type) {
		case NL80211_ATTR_IFINDEX:
			if (len != sizeof(uint32_t)) {
				l_warn("Invalid interface index attribute");
				return;
			}

			netdev = netdev_find(*((uint32_t *) data));
			break;
		}
	}

	if (!netdev) {
		l_warn("Unicast notification is missing ifindex attribute");
		return;
	}

	switch (cmd) {
	case NL80211_CMD_FRAME:
		netdev_mgmt_frame_event(msg, netdev);
		break;
	}
}

struct netdev_watch_event_data {
	struct netdev *netdev;
	enum netdev_watch_event type;
};

static void netdev_watch_notify(void *data, void *user_data)
{
	struct netdev_watch *watch = data;
	struct netdev_watch_event_data *event = user_data;

	watch->callback(event->netdev, event->type, watch->user_data);
}

static void netdev_newlink_notify(const struct ifinfomsg *ifi, int bytes)
{
	struct netdev *netdev;
	bool old_up, new_up;
	char old_name[IFNAMSIZ];
	struct rtattr *attr;
	struct netdev_watch_event_data event;

	netdev = netdev_find(ifi->ifi_index);
	if (!netdev)
		return;

	old_up = netdev_get_is_up(netdev);
	strcpy(old_name, netdev->name);

	netdev->ifi_flags = ifi->ifi_flags;

	for (attr = IFLA_RTA(ifi); RTA_OK(attr, bytes);
			attr = RTA_NEXT(attr, bytes)) {
		if (attr->rta_type != IFLA_IFNAME)
			continue;

		strcpy(netdev->name, RTA_DATA(attr));

		break;
	}

	new_up = netdev_get_is_up(netdev);

	if (old_up != new_up) {
		event.netdev = netdev;
		event.type = new_up ? NETDEV_WATCH_EVENT_UP :
						NETDEV_WATCH_EVENT_DOWN;

		l_queue_foreach(netdev->watches, netdev_watch_notify, &event);
	}

	if (strcmp(old_name, netdev->name)) {
		event.netdev = netdev;
		event.type = NETDEV_WATCH_EVENT_NAME_CHANGE;

		l_queue_foreach(netdev->watches, netdev_watch_notify, &event);
	}
}

static void netdev_dellink_notify(const struct ifinfomsg *ifi, int bytes)
{
	struct netdev *netdev;

	netdev = l_queue_remove_if(netdev_list, netdev_match,
						L_UINT_TO_PTR(ifi->ifi_index));
	if (!netdev)
		return;

	netdev_free(netdev);
}

static void netdev_initial_up_cb(struct netdev *netdev, int result,
					void *user_data)
{
	if (result != 0) {
		l_error("Error bringing interface %i up: %s", netdev->index,
			strerror(-result));

		if (result != -ERFKILL)
			return;
	}

	netdev_set_linkmode_and_operstate(netdev->index, 1,
						IF_OPER_DORMANT,
						netdev_operstate_dormant_cb,
						netdev);

	l_debug("Interface %i initialized", netdev->index);

	netdev->device = device_create(netdev->wiphy, netdev);
}

static void netdev_initial_down_cb(struct netdev *netdev, int result,
					void *user_data)
{
	if (result != 0) {
		l_error("Error taking interface %i down: %s", netdev->index,
			strerror(-result));

		return;
	}

	netdev_set_powered(netdev, true, netdev_initial_up_cb,
				NULL, NULL);
}

static void netdev_getlink_cb(int error, uint16_t type, const void *data,
			uint32_t len, void *user_data)
{
	const struct ifinfomsg *ifi = data;
	unsigned int bytes;
	struct netdev *netdev;

	if (error != 0 || ifi->ifi_type != ARPHRD_ETHER ||
			type != RTM_NEWLINK) {
		l_error("RTM_GETLINK error %i ifi_type %i type %i",
				error, (int) ifi->ifi_type, (int) type);
		return;
	}

	netdev = netdev_find(ifi->ifi_index);
	if (!netdev)
		return;

	bytes = len - NLMSG_ALIGN(sizeof(struct ifinfomsg));

	netdev_newlink_notify(ifi, bytes);

	/*
	 * If the interface is UP, reset it to ensure a clean state,
	 * otherwise just bring it UP.
	 */
	if (netdev_get_is_up(netdev)) {
		netdev_set_powered(netdev, false, netdev_initial_down_cb,
					NULL, NULL);
	} else
		netdev_initial_down_cb(netdev, 0, NULL);
}

static bool netdev_is_managed(const char *ifname)
{
	char *pattern;
	unsigned int i;

	if (!whitelist_filter)
		goto check_blacklist;

	for (i = 0; (pattern = whitelist_filter[i]); i++) {
		if (fnmatch(pattern, ifname, 0) != 0)
			continue;

		goto check_blacklist;
	}

	l_debug("whitelist filtered ifname: %s", ifname);
	return false;

check_blacklist:
	if (!blacklist_filter)
		return true;

	for (i = 0; (pattern = blacklist_filter[i]); i++) {
		if (fnmatch(pattern, ifname, 0) == 0) {
			l_debug("blacklist filtered ifname: %s", ifname);
			return false;
		}
	}

	return true;
}

static void netdev_register_frame(struct netdev *netdev, uint16_t frame_type,
					const uint8_t *prefix,
					size_t prefix_len)
{
	struct l_genl_msg *msg;

	msg = l_genl_msg_new_sized(NL80211_CMD_REGISTER_FRAME, 128);

	l_genl_msg_append_attr(msg, NL80211_ATTR_IFINDEX, 4, &netdev->index);
	l_genl_msg_append_attr(msg, NL80211_ATTR_FRAME_TYPE, 2, &frame_type);
	l_genl_msg_append_attr(msg, NL80211_ATTR_FRAME_MATCH,
				prefix_len, prefix);

	l_genl_family_send(nl80211, msg, NULL, NULL, NULL);
}

static struct l_genl_msg *netdev_build_cmd_set_cqm_rssi(struct netdev *netdev)
{
	struct l_genl_msg *msg;
	/* -70 dBm is a popular choice for low signal threshold */
	int32_t thold = -70;
	uint32_t hyst = 5;

	msg = l_genl_msg_new_sized(NL80211_CMD_SET_CQM, 128);
	l_genl_msg_append_attr(msg, NL80211_ATTR_IFINDEX, 4, &netdev->index);
	l_genl_msg_enter_nested(msg, NL80211_ATTR_CQM);
	l_genl_msg_append_attr(msg, NL80211_ATTR_CQM_RSSI_THOLD, 4, &thold);
	l_genl_msg_append_attr(msg, NL80211_ATTR_CQM_RSSI_HYST, 4, &hyst);
	l_genl_msg_leave_nested(msg);

	return msg;
}

static void netdev_cmd_set_cqm_cb(struct l_genl_msg *msg, void *user_data)
{
	if (l_genl_msg_get_error(msg) < 0)
		l_error("CMD_SET_CQM failed");
}

static void netdev_create_from_genl(struct l_genl_msg *msg)
{
	struct l_genl_attr attr;
	uint16_t type, len;
	const void *data;
	const char *ifname = NULL;
	uint16_t ifname_len = 0;
	const uint8_t *ifaddr;
	const uint32_t *ifindex = NULL, *iftype = NULL;
	struct netdev *netdev;
	struct wiphy *wiphy = NULL;
	struct ifinfomsg *rtmmsg;
	size_t bufsize;
	const uint8_t action_neighbor_report_prefix[2] = { 0x05, 0x05 };
	struct l_genl_msg *set_cqm_msg;

	if (!l_genl_attr_init(&attr, msg))
		return;

	while (l_genl_attr_next(&attr, &type, &len, &data)) {
		switch (type) {
		case NL80211_ATTR_IFINDEX:
			if (len != sizeof(uint32_t)) {
				l_warn("Invalid interface index attribute");
				return;
			}

			ifindex = data;
			break;

		case NL80211_ATTR_IFNAME:
			if (len > IFNAMSIZ) {
				l_warn("Invalid interface name attribute");
				return;
			}

			ifname = data;
			ifname_len = len;
			break;

		case NL80211_ATTR_WIPHY:
			if (len != sizeof(uint32_t)) {
				l_warn("Invalid wiphy attribute");
				return;
			}

			wiphy = wiphy_find(*((uint32_t *) data));
			break;

		case NL80211_ATTR_IFTYPE:
			if (len != sizeof(uint32_t)) {
				l_warn("Invalid interface type attribute");
				return;
			}

			iftype = data;
			break;

		case NL80211_ATTR_MAC:
			if (len != ETH_ALEN) {
				l_warn("Invalid interface address attribute");
				return;
			}

			ifaddr = data;
			break;
		}
	}

	if (!wiphy) {
		l_warn("Missing wiphy attribute or wiphy not found");
		return;
	}

	if (!iftype) {
		l_warn("Missing iftype attribute");
		return;
	}

	if (*iftype != NL80211_IFTYPE_STATION) {
		l_warn("Skipping non-STA interfaces");
		return;
	}

	if (!ifindex || !ifaddr | !ifname) {
		l_warn("Unable to parse interface information");
		return;
	}

	if (netdev_find(*ifindex)) {
		l_debug("Skipping duplicate netdev %s[%d]", ifname, *ifindex);
		return;
	}

	if (!netdev_is_managed(ifname)) {
		l_debug("interface %s filtered out", ifname);
		return;
	}

	netdev = l_new(struct netdev, 1);
	netdev->index = *ifindex;
	netdev->type = *iftype;
	netdev->rekey_offload_support = true;
	memcpy(netdev->addr, ifaddr, sizeof(netdev->addr));
	memcpy(netdev->name, ifname, ifname_len);
	netdev->wiphy = wiphy;

	l_queue_push_tail(netdev_list, netdev);

	if (l_queue_length(netdev_list) == 1)
		eapol_pae_open();

	l_debug("Created interface %s[%d]", netdev->name, netdev->index);

	/* Query interface flags */
	bufsize = NLMSG_ALIGN(sizeof(struct ifinfomsg));
	rtmmsg = l_malloc(bufsize);
	memset(rtmmsg, 0, bufsize);

	rtmmsg->ifi_family = AF_UNSPEC;
	rtmmsg->ifi_index = *ifindex;

	l_netlink_send(rtnl, RTM_GETLINK, 0, rtmmsg, bufsize,
					netdev_getlink_cb, netdev, NULL);

	l_free(rtmmsg);

	/* Subscribe to Management -> Action -> RM -> Neighbor Report frames */
	netdev_register_frame(netdev, 0x00d0, action_neighbor_report_prefix,
				sizeof(action_neighbor_report_prefix));

	/* Set RSSI threshold for CQM notifications */
	set_cqm_msg = netdev_build_cmd_set_cqm_rssi(netdev);

	if (!l_genl_family_send(nl80211, set_cqm_msg, netdev_cmd_set_cqm_cb,
				NULL, NULL)) {
		l_error("CMD_SET_CQM failed");

		l_genl_msg_unref(set_cqm_msg);
	}
}

static void netdev_get_interface_callback(struct l_genl_msg *msg,
								void *user_data)
{
	netdev_create_from_genl(msg);
}

static void netdev_config_notify(struct l_genl_msg *msg, void *user_data)
{
	struct l_genl_attr attr;
	uint16_t type, len;
	const void *data;
	uint8_t cmd;
	const uint32_t *wiphy_id = NULL;
	const uint32_t *ifindex = NULL;
	struct netdev *netdev;

	cmd = l_genl_msg_get_command(msg);

	if (cmd == NL80211_CMD_NEW_INTERFACE) {
		netdev_create_from_genl(msg);
		return;
	}

	if (cmd != NL80211_CMD_DEL_INTERFACE)
		return;

	if (!l_genl_attr_init(&attr, msg))
		return;

	while (l_genl_attr_next(&attr, &type, &len, &data)) {
		switch (type) {
		case NL80211_ATTR_WIPHY:
			if (len != sizeof(uint32_t)) {
				l_warn("Invalid wiphy attribute");
				return;
			}

			wiphy_id = data;
			break;

		case NL80211_ATTR_IFINDEX:
			if (len != sizeof(uint32_t)) {
				l_warn("Invalid ifindex attribute");
				return;
			}

			ifindex = data;
			break;
		}
	}

	if (!wiphy_id || !ifindex)
		return;

	netdev = l_queue_remove_if(netdev_list, netdev_match,
						L_UINT_TO_PTR(*ifindex));
	if (!netdev)
		return;

	netdev_free(netdev);
}

static void netdev_link_notify(uint16_t type, const void *data, uint32_t len,
							void *user_data)
{
	const struct ifinfomsg *ifi = data;
	unsigned int bytes;

	if (ifi->ifi_type != ARPHRD_ETHER)
		return;

	bytes = len - NLMSG_ALIGN(sizeof(struct ifinfomsg));

	switch (type) {
	case RTM_NEWLINK:
		netdev_newlink_notify(ifi, bytes);
		break;
	case RTM_DELLINK:
		netdev_dellink_notify(ifi, bytes);
		break;
	}
}

static bool netdev_watch_match(const void *a, const void *b)
{
	const struct netdev_watch *item = a;
	uint32_t id = L_PTR_TO_UINT(b);

	return item->id == id;
}

uint32_t netdev_watch_add(struct netdev *netdev, netdev_watch_func_t func,
				void *user_data)
{
	struct netdev_watch *item;

	item = l_new(struct netdev_watch, 1);
	item->id = ++netdev->next_watch_id;
	item->callback = func;
	item->user_data = user_data;

	if (!netdev->watches)
		netdev->watches = l_queue_new();

	l_queue_push_tail(netdev->watches, item);

	return item->id;
}

bool netdev_watch_remove(struct netdev *netdev, uint32_t id)
{
	struct netdev_watch *item;

	item = l_queue_remove_if(netdev->watches, netdev_watch_match,
					L_UINT_TO_PTR(id));
	if (!item)
		return false;

	l_free(item);

	return true;
}

bool netdev_init(struct l_genl_family *in,
				const char *whitelist, const char *blacklist)
{
	struct l_genl_msg *msg;
	struct l_genl *genl = l_genl_family_get_genl(in);

	if (rtnl)
		return false;

	l_debug("Opening route netlink socket");

	rtnl = l_netlink_new(NETLINK_ROUTE);
	if (!rtnl) {
		l_error("Failed to open route netlink socket");
		return false;
	}

	if (getenv("IWD_RTNL_DEBUG"))
		l_netlink_set_debug(rtnl, do_debug, "[RTNL] ", NULL);

	if (!l_netlink_register(rtnl, RTNLGRP_LINK,
				netdev_link_notify, NULL, NULL)) {
		l_error("Failed to register for RTNL link notifications");
		l_netlink_destroy(rtnl);
		return false;
	}

	netdev_list = l_queue_new();

	nl80211 = in;

	if (!l_genl_family_register(nl80211, "config", netdev_config_notify,
								NULL, NULL))
		l_error("Registering for config notification failed");

	msg = l_genl_msg_new(NL80211_CMD_GET_INTERFACE);
	if (!l_genl_family_dump(nl80211, msg, netdev_get_interface_callback,
								NULL, NULL))
		l_error("Getting all interface information failed");

	if (!l_genl_family_register(nl80211, "mlme", netdev_mlme_notify,
								NULL, NULL))
		l_error("Registering for MLME notification failed");

	if (!l_genl_set_unicast_handler(genl, netdev_unicast_notify,
								NULL, NULL))
		l_error("Registering for unicast notification failed");

	__handshake_set_install_tk_func(netdev_set_tk);
	__handshake_set_install_gtk_func(netdev_set_gtk);
	__handshake_set_install_igtk_func(netdev_set_igtk);

	__eapol_set_deauthenticate_func(netdev_handshake_failed);
	__eapol_set_rekey_offload_func(netdev_set_rekey_offload);

	if (whitelist)
		whitelist_filter = l_strsplit(whitelist, ',');

	if (blacklist)
		blacklist_filter = l_strsplit(blacklist, ',');

	return true;
}

bool netdev_exit(void)
{
	if (!rtnl)
		return false;

	l_strfreev(whitelist_filter);
	l_strfreev(blacklist_filter);

	nl80211 = NULL;

	l_debug("Closing route netlink socket");
	l_netlink_destroy(rtnl);
	rtnl = NULL;

	return true;
}

void netdev_shutdown(void)
{
	if (!rtnl)
		return;

	l_queue_foreach(netdev_list, netdev_shutdown_one, NULL);

	l_queue_destroy(netdev_list, netdev_free);
	netdev_list = NULL;

	eapol_pae_close();
}
