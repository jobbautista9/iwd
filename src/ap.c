/*
 *
 *  Wireless daemon for Linux
 *
 *  Copyright (C) 2017-2019  Intel Corporation. All rights reserved.
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

#include <errno.h>
#include <linux/if_ether.h>
#include <netinet/in.h>
#include <arpa/inet.h>

#include <ell/ell.h>

#include "linux/nl80211.h"

#include "ell/useful.h"
#include "src/missing.h"
#include "src/iwd.h"
#include "src/module.h"
#include "src/scan.h"
#include "src/netdev.h"
#include "src/wiphy.h"
#include "src/crypto.h"
#include "src/ie.h"
#include "src/mpdu.h"
#include "src/util.h"
#include "src/eapol.h"
#include "src/handshake.h"
#include "src/dbus.h"
#include "src/nl80211util.h"
#include "src/frame-xchg.h"
#include "src/wscutil.h"
#include "src/eap-wsc.h"
#include "src/ap.h"
#include "src/storage.h"
#include "src/diagnostic.h"

struct ap_state {
	struct netdev *netdev;
	struct l_genl_family *nl80211;
	const struct ap_ops *ops;
	ap_stopped_func_t stopped_func;
	void *user_data;
	struct ap_config *config;

	unsigned int ciphers;
	enum ie_rsn_cipher_suite group_cipher;
	uint32_t beacon_interval;
	struct l_uintset *rates;
	uint32_t start_stop_cmd_id;
	uint32_t mlme_watch;
	uint8_t gtk[CRYPTO_MAX_GTK_LEN];
	uint8_t gtk_index;
	struct l_queue *wsc_pbc_probes;
	struct l_timeout *wsc_pbc_timeout;
	uint16_t wsc_dpid;
	uint8_t wsc_uuid_r[16];

	uint16_t last_aid;
	struct l_queue *sta_states;

	struct l_dhcp_server *server;
	uint32_t rtnl_add_cmd;
	char *own_ip;
	unsigned int ip_prefix;

	bool started : 1;
	bool gtk_set : 1;
	bool cleanup_ip : 1;
	bool use_ip_pool : 1;
};

struct sta_state {
	uint8_t addr[6];
	bool associated;
	bool rsna;
	uint16_t aid;
	struct mmpdu_field_capability capability;
	uint16_t listen_interval;
	struct l_uintset *rates;
	uint32_t assoc_resp_cmd_id;
	struct ap_state *ap;
	uint8_t *assoc_ies;
	size_t assoc_ies_len;
	uint8_t *assoc_rsne;
	struct eapol_sm *sm;
	struct handshake_state *hs;
	uint32_t gtk_query_cmd_id;
	struct l_idle *stop_handshake_work;
	struct l_settings *wsc_settings;
	uint8_t wsc_uuid_e[16];
	bool wsc_v2;
};

struct ap_wsc_pbc_probe_record {
	uint8_t mac[6];
	uint8_t uuid_e[16];
	uint64_t timestamp;
};

struct ap_ip_pool {
	uint32_t start;
	uint32_t end;
	uint8_t prefix;

	/* Fist/last valid subnet */
	uint8_t sub_start;
	uint8_t sub_end;

	struct l_uintset *used;
};

struct ap_ip_pool pool;
static uint32_t netdev_watch;
struct l_netlink *rtnl;

/*
 * Creates pool of IPs which AP intefaces can use. Each call to ip_pool_get
 * will advance the subnet +1 so there are no IP conflicts between AP
 * interfaces
 */
static bool ip_pool_create(const char *ip_prefix)
{
	if (!util_ip_prefix_tohl(ip_prefix, &pool.prefix, &pool.start,
					&pool.end, NULL))
		return false;

	if (pool.prefix > 24) {
		l_error("APRanges prefix must 24 or less (%u used)",
				pool.prefix);
		memset(&pool, 0, sizeof(pool));
		return false;
	}

	/*
	 * Find the number of subnets we can use, this will dictate the number
	 * of AP interfaces that can be created (when using DHCP)
	 */
	pool.sub_start = (pool.start & 0x0000ff00) >> 8;
	pool.sub_end = (pool.end & 0x0000ff00) >> 8;

	pool.used = l_uintset_new_from_range(pool.sub_start, pool.sub_end);

	return true;
}

static char *ip_pool_get()
{
	uint32_t ip;
	struct in_addr ia;
	uint8_t next_subnet = (uint8_t)l_uintset_find_unused_min(pool.used);

	/* This shouldn't happen */
	if (next_subnet < pool.sub_start || next_subnet > pool.sub_end)
		return NULL;

	l_uintset_put(pool.used, next_subnet);

	ip = pool.start;
	ip &= 0xffff00ff;
	ip |= (next_subnet << 8);

	ia.s_addr = htonl(ip);
	return l_strdup(inet_ntoa(ia));
}

static bool ip_pool_put(const char *address)
{
	struct in_addr ia;
	uint32_t ip;
	uint8_t subnet;

	if (inet_aton(address, &ia) < 0)
		return false;

	ip = ntohl(ia.s_addr);

	subnet = (ip & 0x0000ff00) >> 8;

	if (subnet < pool.sub_start || subnet > pool.sub_end)
		return false;

	return l_uintset_take(pool.used, subnet);
}

static void ip_pool_destroy()
{
	if (pool.used)
		l_uintset_free(pool.used);

	memset(&pool, 0, sizeof(pool));
}

static const char *broadcast_from_ip(const char *ip)
{
	struct in_addr ia;
	uint32_t bcast;

	if (inet_aton(ip, &ia) != 1)
		return NULL;

	bcast = ntohl(ia.s_addr);
	bcast &= 0xffffff00;
	bcast |= 0x000000ff;

	ia.s_addr = htonl(bcast);

	return inet_ntoa(ia);
}

void ap_config_free(struct ap_config *config)
{
	if (unlikely(!config))
		return;

	l_free(config->ssid);

	explicit_bzero(config->passphrase, sizeof(config->passphrase));
	explicit_bzero(config->psk, sizeof(config->psk));
	l_free(config->authorized_macs);
	l_free(config->wsc_name);

	if (config->profile)
		l_free(config->profile);

	l_free(config);
}

static void ap_stop_handshake(struct sta_state *sta)
{
	if (sta->sm) {
		eapol_sm_free(sta->sm);
		sta->sm = NULL;
	}

	if (sta->hs) {
		handshake_state_free(sta->hs);
		sta->hs = NULL;
	}

	if (sta->wsc_settings) {
		l_settings_free(sta->wsc_settings);
		sta->wsc_settings = NULL;
	}

	if (sta->stop_handshake_work) {
		l_idle_remove(sta->stop_handshake_work);
		sta->stop_handshake_work = NULL;
	}
}

static void ap_stop_handshake_work(struct l_idle *idle, void *user_data)
{
	struct sta_state *sta = user_data;

	ap_stop_handshake(sta);
}

static void ap_sta_free(void *data)
{
	struct sta_state *sta = data;
	struct ap_state *ap = sta->ap;

	l_uintset_free(sta->rates);
	l_free(sta->assoc_ies);

	if (sta->assoc_resp_cmd_id)
		l_genl_family_cancel(ap->nl80211, sta->assoc_resp_cmd_id);

	if (sta->gtk_query_cmd_id)
		l_genl_family_cancel(ap->nl80211, sta->gtk_query_cmd_id);

	ap_stop_handshake(sta);

	l_free(sta);
}

static void ap_reset(struct ap_state *ap)
{
	struct netdev *netdev = ap->netdev;

	if (ap->mlme_watch)
		l_genl_family_unregister(ap->nl80211, ap->mlme_watch);

	frame_watch_wdev_remove(netdev_get_wdev_id(netdev));

	if (ap->start_stop_cmd_id)
		l_genl_family_cancel(ap->nl80211, ap->start_stop_cmd_id);

	if (ap->rtnl_add_cmd)
		l_netlink_cancel(rtnl, ap->rtnl_add_cmd);

	l_queue_destroy(ap->sta_states, ap_sta_free);

	if (ap->rates)
		l_uintset_free(ap->rates);

	ap_config_free(ap->config);
	ap->config = NULL;

	l_queue_destroy(ap->wsc_pbc_probes, l_free);

	ap->started = false;

	/* Delete IP if one was set by IWD */
	if (ap->cleanup_ip)
		l_rtnl_ifaddr4_delete(rtnl, netdev_get_ifindex(netdev),
					ap->ip_prefix, ap->own_ip,
					broadcast_from_ip(ap->own_ip),
					NULL, NULL, NULL);

	if (ap->own_ip) {
		/* Release IP from pool if used */
		if (ap->use_ip_pool)
			ip_pool_put(ap->own_ip);

		l_free(ap->own_ip);
	}

	if (ap->server)
		l_dhcp_server_stop(ap->server);
}

static void ap_del_station(struct sta_state *sta, uint16_t reason,
				bool disassociate)
{
	struct ap_state *ap = sta->ap;
	struct ap_event_station_removed_data event_data;
	bool send_event = false;

	netdev_del_station(ap->netdev, sta->addr, reason, disassociate);
	sta->associated = false;

	if (sta->rsna) {
		if (ap->ops->handle_event) {
			memset(&event_data, 0, sizeof(event_data));
			event_data.mac = sta->addr;
			event_data.reason = reason;
			send_event = true;
		}

		sta->rsna = false;
	}

	if (sta->gtk_query_cmd_id) {
		l_genl_family_cancel(ap->nl80211, sta->gtk_query_cmd_id);
		sta->gtk_query_cmd_id = 0;
	}

	ap_stop_handshake(sta);

	if (send_event)
		ap->ops->handle_event(AP_EVENT_STATION_REMOVED, &event_data,
					ap->user_data);
}

static bool ap_sta_match_addr(const void *a, const void *b)
{
	const struct sta_state *sta = a;

	return !memcmp(sta->addr, b, 6);
}

static void ap_remove_sta(struct sta_state *sta)
{
	if (!l_queue_remove(sta->ap->sta_states, sta)) {
		l_error("tried to remove station that doesn't exist");
		return;
	}

	ap_sta_free(sta);
}

static void ap_set_sta_cb(struct l_genl_msg *msg, void *user_data)
{
	if (l_genl_msg_get_error(msg) < 0)
		l_error("SET_STATION failed: %i", l_genl_msg_get_error(msg));
}

static void ap_del_key_cb(struct l_genl_msg *msg, void *user_data)
{
	if (l_genl_msg_get_error(msg) < 0)
		l_debug("DEL_KEY failed: %i", l_genl_msg_get_error(msg));
}

static void ap_new_rsna(struct sta_state *sta)
{
	struct ap_state *ap = sta->ap;

	l_debug("STA "MAC" authenticated", MAC_STR(sta->addr));

	sta->rsna = true;

	if (ap->ops->handle_event) {
		struct ap_event_station_added_data event_data = {};
		event_data.mac = sta->addr;
		event_data.assoc_ies = sta->assoc_ies;
		event_data.assoc_ies_len = sta->assoc_ies_len;
		ap->ops->handle_event(AP_EVENT_STATION_ADDED, &event_data,
					ap->user_data);
	}
}

static void ap_drop_rsna(struct sta_state *sta)
{
	struct ap_state *ap = sta->ap;
	struct l_genl_msg *msg;
	uint32_t ifindex = netdev_get_ifindex(sta->ap->netdev);
	uint8_t key_id = 0;

	sta->rsna = false;

	msg = nl80211_build_set_station_unauthorized(ifindex, sta->addr);

	l_genl_msg_append_attr(msg, NL80211_ATTR_STA_AID, 2, &sta->aid);

	if (!l_genl_family_send(ap->nl80211, msg, ap_set_sta_cb, NULL, NULL)) {
		l_genl_msg_unref(msg);
		l_error("Issuing SET_STATION failed");
	}

	msg = l_genl_msg_new_sized(NL80211_CMD_DEL_KEY, 64);
	l_genl_msg_append_attr(msg, NL80211_ATTR_IFINDEX, 4, &ifindex);
	l_genl_msg_append_attr(msg, NL80211_ATTR_KEY_IDX, 1, &key_id);
	l_genl_msg_append_attr(msg, NL80211_ATTR_MAC, 6, sta->addr);

	if (!l_genl_family_send(ap->nl80211, msg, ap_del_key_cb, NULL, NULL)) {
		l_genl_msg_unref(msg);
		l_error("Issuing DEL_KEY failed");
	}

	ap_stop_handshake(sta);

	if (ap->ops->handle_event) {
		struct ap_event_station_removed_data event_data = {};
		event_data.mac = sta->addr;
		ap->ops->handle_event(AP_EVENT_STATION_REMOVED, &event_data,
					ap->user_data);
	}
}

static void ap_set_rsn_info(struct ap_state *ap, struct ie_rsn_info *rsn)
{
	memset(rsn, 0, sizeof(*rsn));
	rsn->akm_suites = IE_RSN_AKM_SUITE_PSK;
	rsn->pairwise_ciphers = ap->ciphers;
	rsn->group_cipher = ap->group_cipher;
}

static void ap_wsc_exit_pbc(struct ap_state *ap)
{
	if (!ap->wsc_pbc_timeout)
		return;

	l_timeout_remove(ap->wsc_pbc_timeout);
	ap->wsc_dpid = 0;
	ap_update_beacon(ap);

	ap->ops->handle_event(AP_EVENT_PBC_MODE_EXIT, NULL, ap->user_data);
}

struct ap_pbc_record_expiry_data {
	uint64_t min_time;
	const uint8_t *mac;
};

static bool ap_wsc_pbc_record_expire(void *data, void *user_data)
{
	struct ap_wsc_pbc_probe_record *record = data;
	const struct ap_pbc_record_expiry_data *expiry_data = user_data;

	if (record->timestamp > expiry_data->min_time &&
			memcmp(record->mac, expiry_data->mac, 6))
		return false;

	l_free(record);
	return true;
}

#define AP_WSC_PBC_MONITOR_TIME	120
#define AP_WSC_PBC_WALK_TIME	120

static void ap_process_wsc_probe_req(struct ap_state *ap, const uint8_t *from,
					const uint8_t *wsc_data,
					size_t wsc_data_len)
{
	struct wsc_probe_request req;
	struct ap_pbc_record_expiry_data expiry_data;
	struct ap_wsc_pbc_probe_record *record;
	uint64_t now;
	bool empty;
	uint8_t first_sta_addr[6] = {};
	const struct l_queue_entry *entry;

	if (wsc_parse_probe_request(wsc_data, wsc_data_len, &req) < 0)
		return;

	if (!(req.config_methods & WSC_CONFIGURATION_METHOD_PUSH_BUTTON))
		return;

	if (req.device_password_id != WSC_DEVICE_PASSWORD_ID_PUSH_BUTTON)
		return;

	/* Save the address of the first enrollee record */
	record = l_queue_peek_head(ap->wsc_pbc_probes);
	if (record)
		memcpy(first_sta_addr, record->mac, 6);

	now = l_time_now();

	/*
	 * Expire entries older than PBC Monitor Time.  While there also drop
	 * older entries from the same Enrollee that sent us this new Probe
	 * Request.  It's unclear whether we should also match by the UUID-E.
	 */
	expiry_data.min_time = now - AP_WSC_PBC_MONITOR_TIME * 1000000;
	expiry_data.mac = from;
	l_queue_foreach_remove(ap->wsc_pbc_probes, ap_wsc_pbc_record_expire,
				&expiry_data);

	empty = l_queue_isempty(ap->wsc_pbc_probes);

	if (!ap->wsc_pbc_probes)
		ap->wsc_pbc_probes = l_queue_new();

	/* Add new record */
	record = l_new(struct ap_wsc_pbc_probe_record, 1);
	memcpy(record->mac, from, 6);
	memcpy(record->uuid_e, req.uuid_e, sizeof(record->uuid_e));
	record->timestamp = now;
	l_queue_push_tail(ap->wsc_pbc_probes, record);

	/*
	 * If queue was non-empty and we've added one more record then we
	 * now have seen more than one PBC enrollee during the PBC Monitor
	 * Time and must exit "active PBC mode" due to "session overlap".
	 * WSC v2.0.5 Section 11.3:
	 * "Within the PBC Monitor Time, if the Registrar receives PBC
	 * probe requests from more than one Enrollee [...] then the
	 * Registrar SHALL signal a "session overlap" error.  As a result,
	 * the Registrar shall refuse to enter active PBC mode and shall
	 * also refuse to perform a PBC-based Registration Protocol
	 * exchange [...]"
	 */
	if (empty)
		return;

	if (ap->wsc_pbc_timeout) {
		l_debug("Exiting PBC mode due to Session Overlap");
		ap_wsc_exit_pbc(ap);
	}

	/*
	 * "If the Registrar is engaged in PBC Registration Protocol
	 * exchange with an Enrollee and receives a Probe Request or M1
	 * Message from another Enrollee, then the Registrar should
	 * signal a "session overlap" error".
	 *
	 * For simplicity just interrupt the handshake with that enrollee.
	 */
	for (entry = l_queue_get_entries(ap->sta_states); entry;
			entry = entry->next) {
		struct sta_state *sta = entry->data;

		if (!sta->associated || sta->assoc_rsne)
			continue;

		/*
		 * Check whether this enrollee is in PBC Registration
		 * Protocol by comparing its mac with the first (and only)
		 * record we had in ap->wsc_pbc_probes.  If we had more
		 * than one record we wouldn't have been in
		 * "active PBC mode".
		 */
		if (memcmp(sta->addr, first_sta_addr, 6) ||
				!memcmp(sta->addr, from, 6))
			continue;

		l_debug("Interrupting handshake with %s due to Session Overlap",
			util_address_to_string(sta->addr));

		if (sta->hs) {
			netdev_handshake_failed(sta->hs,
					MMPDU_REASON_CODE_DISASSOC_AP_BUSY);
			sta->sm = NULL;
		}

		ap_remove_sta(sta);
	}
}

static size_t ap_get_wsc_ie_len(struct ap_state *ap,
				enum mpdu_management_subtype type,
				const struct mmpdu_header *client_frame,
				size_t client_frame_len)
{
	return 256;
}

static size_t ap_write_wsc_ie(struct ap_state *ap,
				enum mpdu_management_subtype type,
				const struct mmpdu_header *client_frame,
				size_t client_frame_len,
				uint8_t *out_buf)
{
	const uint8_t *from = client_frame->address_2;
	uint8_t *wsc_data;
	size_t wsc_data_size;
	uint8_t *wsc_ie;
	size_t wsc_ie_size;
	size_t len = 0;

	/* WSC IE */
	if (type == MPDU_MANAGEMENT_SUBTYPE_PROBE_RESPONSE) {
		struct wsc_probe_response wsc_pr = {};
		const struct mmpdu_probe_request *req =
			mmpdu_body(client_frame);
		size_t req_ies_len = (void *) client_frame + client_frame_len -
			(void *) req->ies;
		ssize_t req_wsc_data_size;

		/*
		 * Process the client Probe Request WSC IE first as it may
		 * cause us to exit "active PBC mode" and that will be
		 * immediately reflected in our Probe Response WSC IE.
		 */
		wsc_data = ie_tlv_extract_wsc_payload(req->ies, req_ies_len,
							&req_wsc_data_size);
		if (wsc_data) {
			ap_process_wsc_probe_req(ap, from, wsc_data,
							req_wsc_data_size);
			l_free(wsc_data);
		}

		wsc_pr.version2 = true;
		wsc_pr.state = WSC_STATE_CONFIGURED;

		if (ap->wsc_pbc_timeout) {
			wsc_pr.selected_registrar = true;
			wsc_pr.device_password_id = ap->wsc_dpid;
			wsc_pr.selected_reg_config_methods =
				WSC_CONFIGURATION_METHOD_PUSH_BUTTON;
		}

		wsc_pr.response_type = WSC_RESPONSE_TYPE_AP;
		memcpy(wsc_pr.uuid_e, ap->wsc_uuid_r, sizeof(wsc_pr.uuid_e));
		wsc_pr.primary_device_type =
			ap->config->wsc_primary_device_type;

		if (ap->config->wsc_name)
			l_strlcpy(wsc_pr.device_name, ap->config->wsc_name,
					sizeof(wsc_pr.device_name));

		wsc_pr.config_methods =
			WSC_CONFIGURATION_METHOD_PUSH_BUTTON;

		if (ap->config->authorized_macs_num) {
			size_t len;

			len = ap->config->authorized_macs_num * 6;
			if (len > sizeof(wsc_pr.authorized_macs))
				len = sizeof(wsc_pr.authorized_macs);

			memcpy(wsc_pr.authorized_macs,
				ap->config->authorized_macs, len);
		}

		wsc_data = wsc_build_probe_response(&wsc_pr, &wsc_data_size);
	} else if (type == MPDU_MANAGEMENT_SUBTYPE_BEACON) {
		struct wsc_beacon wsc_beacon = {};

		wsc_beacon.version2 = true;
		wsc_beacon.state = WSC_STATE_CONFIGURED;

		if (ap->wsc_pbc_timeout) {
			wsc_beacon.selected_registrar = true;
			wsc_beacon.device_password_id = ap->wsc_dpid;
			wsc_beacon.selected_reg_config_methods =
				WSC_CONFIGURATION_METHOD_PUSH_BUTTON;
		}

		if (ap->config->authorized_macs_num) {
			size_t len;

			len = ap->config->authorized_macs_num * 6;
			if (len > sizeof(wsc_beacon.authorized_macs))
				len = sizeof(wsc_beacon.authorized_macs);

			memcpy(wsc_beacon.authorized_macs,
				ap->config->authorized_macs, len);
		}

		wsc_data = wsc_build_beacon(&wsc_beacon, &wsc_data_size);
	} else if (L_IN_SET(type, MPDU_MANAGEMENT_SUBTYPE_ASSOCIATION_RESPONSE,
			MPDU_MANAGEMENT_SUBTYPE_REASSOCIATION_RESPONSE)) {
		struct wsc_association_response wsc_resp = {};
		struct sta_state *sta =
			l_queue_find(ap->sta_states, ap_sta_match_addr, from);

		if (!sta || sta->assoc_rsne)
			return 0;

		wsc_resp.response_type = WSC_RESPONSE_TYPE_AP;
		wsc_resp.version2 = sta->wsc_v2;

		wsc_data = wsc_build_association_response(&wsc_resp,
								&wsc_data_size);
	} else
		return 0;

	if (!wsc_data) {
		l_error("wsc_build_<mgmt-subtype> error (stype 0x%x)", type);
		return 0;
	}

	wsc_ie = ie_tlv_encapsulate_wsc_payload(wsc_data, wsc_data_size,
						&wsc_ie_size);
	l_free(wsc_data);

	if (!wsc_ie) {
		l_error("ie_tlv_encapsulate_wsc_payload error (stype 0x%x)",
			type);
		return 0;
	}

	memcpy(out_buf + len, wsc_ie, wsc_ie_size);
	len += wsc_ie_size;
	l_free(wsc_ie);

	return len;
}

static size_t ap_get_extra_ies_len(struct ap_state *ap,
					enum mpdu_management_subtype type,
					const struct mmpdu_header *client_frame,
					size_t client_frame_len)
{
	size_t len = 0;

	len += ap_get_wsc_ie_len(ap, type, client_frame, client_frame_len);

	if (ap->ops->get_extra_ies_len)
		len += ap->ops->get_extra_ies_len(type, client_frame,
							client_frame_len,
							ap->user_data);

	return len;
}

static size_t ap_write_extra_ies(struct ap_state *ap,
					enum mpdu_management_subtype type,
					const struct mmpdu_header *client_frame,
					size_t client_frame_len,
					uint8_t *out_buf)
{
	size_t len = 0;

	len += ap_write_wsc_ie(ap, type, client_frame, client_frame_len,
				out_buf + len);

	if (ap->ops->write_extra_ies)
		len += ap->ops->write_extra_ies(type,
						client_frame, client_frame_len,
						out_buf + len, ap->user_data);

	return len;
}

/*
 * Build a Beacon frame or a Probe Response frame's header and body until
 * the TIM IE.  Except for the optional TIM IE which is inserted by the
 * kernel when needed, our contents for both frames are the same.
 * See Beacon format in 8.3.3.2 and Probe Response format in 8.3.3.10.
 *
 * 802.11-2016, Section 9.4.2.1:
 * "The frame body components specified for many management subtypes result
 * in elements ordered by ascending values of the Element ID field and then
 * the Element ID Extension field (when present), with the exception of the
 * MIC Management element (9.4.2.55)."
 */
static size_t ap_build_beacon_pr_head(struct ap_state *ap,
					enum mpdu_management_subtype stype,
					const uint8_t *dest, uint8_t *out_buf,
					size_t out_len)
{
	struct mmpdu_header *mpdu = (void *) out_buf;
	unsigned int len;
	uint16_t capability = IE_BSS_CAP_ESS | IE_BSS_CAP_PRIVACY;
	const uint8_t *bssid = netdev_get_address(ap->netdev);
	uint32_t minr, maxr, count, r;
	uint8_t *rates;
	struct ie_tlv_builder builder;

	memset(mpdu, 0, 36); /* Zero out header + non-IE fields */

	/* Header */
	mpdu->fc.protocol_version = 0;
	mpdu->fc.type = MPDU_TYPE_MANAGEMENT;
	mpdu->fc.subtype = stype;
	memcpy(mpdu->address_1, dest, 6);	/* DA */
	memcpy(mpdu->address_2, bssid, 6);	/* SA */
	memcpy(mpdu->address_3, bssid, 6);	/* BSSID */

	/* Body non-IE fields */
	l_put_le16(ap->beacon_interval, out_buf + 32);	/* Beacon Interval */
	l_put_le16(capability, out_buf + 34);		/* Capability Info */

	ie_tlv_builder_init(&builder, out_buf + 36, out_len - 36);

	/* SSID IE */
	ie_tlv_builder_next(&builder, IE_TYPE_SSID);
	ie_tlv_builder_set_data(&builder, ap->config->ssid,
				strlen(ap->config->ssid));

	/* Supported Rates IE */
	ie_tlv_builder_next(&builder, IE_TYPE_SUPPORTED_RATES);
	rates = ie_tlv_builder_get_data(&builder);

	minr = l_uintset_find_min(ap->rates);
	maxr = l_uintset_find_max(ap->rates);
	count = 0;
	for (r = minr; r <= maxr && count < 8; r++)
		if (l_uintset_contains(ap->rates, r)) {
			uint8_t flag = 0;

			/* Mark only the lowest rate as Basic Rate */
			if (count == 0)
				flag = 0x80;

			*rates++ = r | flag;
			count++;
		}

	ie_tlv_builder_set_length(&builder, count);

	/* DSSS Parameter Set IE for DSSS, HR, ERP and HT PHY rates */
	ie_tlv_builder_next(&builder, IE_TYPE_DSSS_PARAMETER_SET);
	ie_tlv_builder_set_data(&builder, &ap->config->channel, 1);

	ie_tlv_builder_finalize(&builder, &len);
	return 36 + len;
}

/* Beacon / Probe Response frame portion after the TIM IE */
static size_t ap_build_beacon_pr_tail(struct ap_state *ap,
					enum mpdu_management_subtype stype,
					const struct mmpdu_header *req,
					size_t req_len, uint8_t *out_buf)
{
	size_t len;
	struct ie_rsn_info rsn;

	/* TODO: Country IE between TIM IE and RSNE */

	/* RSNE */
	ap_set_rsn_info(ap, &rsn);
	if (!ie_build_rsne(&rsn, out_buf))
		return 0;
	len = 2 + out_buf[1];

	len += ap_write_extra_ies(ap, stype, req, req_len, out_buf + len);
	return len;
}

static void ap_set_beacon_cb(struct l_genl_msg *msg, void *user_data)
{
	int error = l_genl_msg_get_error(msg);

	if (error < 0)
		l_error("SET_BEACON failed: %s (%i)", strerror(-error), -error);
}

void ap_update_beacon(struct ap_state *ap)
{
	struct l_genl_msg *cmd;
	uint8_t head[256];
	L_AUTO_FREE_VAR(uint8_t *, tail) =
		l_malloc(256 + ap_get_extra_ies_len(ap,
						MPDU_MANAGEMENT_SUBTYPE_BEACON,
						NULL, 0));
	size_t head_len, tail_len;
	uint64_t wdev_id = netdev_get_wdev_id(ap->netdev);
	static const uint8_t bcast_addr[6] = {
		0xff, 0xff, 0xff, 0xff, 0xff, 0xff
	};

	if (L_WARN_ON(!ap->started))
		return;

	head_len = ap_build_beacon_pr_head(ap, MPDU_MANAGEMENT_SUBTYPE_BEACON,
						bcast_addr, head, sizeof(head));
	tail_len = ap_build_beacon_pr_tail(ap, MPDU_MANAGEMENT_SUBTYPE_BEACON,
						NULL, 0, tail);
	if (L_WARN_ON(!head_len || !tail_len))
		return;

	cmd = l_genl_msg_new_sized(NL80211_CMD_SET_BEACON,
					32 + head_len + tail_len);
	l_genl_msg_append_attr(cmd, NL80211_ATTR_WDEV, 8, &wdev_id);
	l_genl_msg_append_attr(cmd, NL80211_ATTR_BEACON_HEAD, head_len, head);
	l_genl_msg_append_attr(cmd, NL80211_ATTR_BEACON_TAIL, tail_len, tail);
	l_genl_msg_append_attr(cmd, NL80211_ATTR_IE, 0, "");
	l_genl_msg_append_attr(cmd, NL80211_ATTR_IE_PROBE_RESP, 0, "");
	l_genl_msg_append_attr(cmd, NL80211_ATTR_IE_ASSOC_RESP, 0, "");

	if (l_genl_family_send(ap->nl80211, cmd, ap_set_beacon_cb, NULL, NULL))
		return;

	l_genl_msg_unref(cmd);
	l_error("Issuing SET_BEACON failed");
}

static uint32_t ap_send_mgmt_frame(struct ap_state *ap,
					const struct mmpdu_header *frame,
					size_t frame_len,
					frame_xchg_cb_t callback,
					void *user_data)
{
	uint32_t ch_freq = scan_channel_to_freq(ap->config->channel,
						SCAN_BAND_2_4_GHZ);
	uint64_t wdev_id = netdev_get_wdev_id(ap->netdev);
	struct iovec iov[2];

	iov[0].iov_base = (void *) frame;
	iov[0].iov_len = frame_len;
	iov[1].iov_base = NULL;
	return frame_xchg_start(wdev_id, iov, ch_freq, 0, 0, 0, 0,
					callback, user_data, NULL, NULL);
}

static void ap_start_handshake(struct sta_state *sta, bool use_eapol_start,
				const uint8_t *gtk_rsc)
{
	struct ap_state *ap = sta->ap;
	const uint8_t *own_addr = netdev_get_address(ap->netdev);
	struct ie_rsn_info rsn;
	uint8_t bss_rsne[64];

	handshake_state_set_ssid(sta->hs, (void *) ap->config->ssid,
					strlen(ap->config->ssid));
	handshake_state_set_authenticator_address(sta->hs, own_addr);
	handshake_state_set_supplicant_address(sta->hs, sta->addr);

	ap_set_rsn_info(ap, &rsn);
	/*
	 * Note: This assumes the length that ap_set_rsn_info() requires. If
	 * ap_set_rsn_info() changes then this will need to be updated.
	 */
	ie_build_rsne(&rsn, bss_rsne);
	handshake_state_set_authenticator_ie(sta->hs, bss_rsne);

	if (gtk_rsc)
		handshake_state_set_gtk(sta->hs, sta->ap->gtk,
					sta->ap->gtk_index, gtk_rsc);

	sta->sm = eapol_sm_new(sta->hs);
	if (!sta->sm) {
		ap_stop_handshake(sta);
		l_error("could not create sm object");
		goto error;
	}

	eapol_sm_set_listen_interval(sta->sm, sta->listen_interval);
	eapol_sm_set_use_eapol_start(sta->sm, use_eapol_start);

	eapol_register(sta->sm);
	eapol_start(sta->sm);

	return;

error:
	ap_del_station(sta, MMPDU_REASON_CODE_UNSPECIFIED, true);
}

static void ap_handshake_event(struct handshake_state *hs,
		enum handshake_event event, void *user_data, ...)
{
	struct sta_state *sta = user_data;
	va_list args;

	va_start(args, user_data);

	switch (event) {
	case HANDSHAKE_EVENT_COMPLETE:
		ap_new_rsna(sta);
		break;
	case HANDSHAKE_EVENT_FAILED:
		netdev_handshake_failed(hs, va_arg(args, int));
		/* fall through */
	case HANDSHAKE_EVENT_SETTING_KEYS_FAILED:
		sta->sm = NULL;
		ap_remove_sta(sta);
	default:
		break;
	}

	va_end(args);
}

static void ap_start_rsna(struct sta_state *sta, const uint8_t *gtk_rsc)
{
	/* this handshake setup assumes PSK network */
	sta->hs = netdev_handshake_state_new(sta->ap->netdev);
	handshake_state_set_authenticator(sta->hs, true);
	handshake_state_set_event_func(sta->hs, ap_handshake_event, sta);
	handshake_state_set_supplicant_ie(sta->hs, sta->assoc_rsne);
	handshake_state_set_pmk(sta->hs, sta->ap->config->psk, 32);
	ap_start_handshake(sta, false, gtk_rsc);
}

static void ap_gtk_query_cb(struct l_genl_msg *msg, void *user_data)
{
	struct sta_state *sta = user_data;
	const void *gtk_rsc;
	uint8_t zero_gtk_rsc[6];

	sta->gtk_query_cmd_id = 0;

	if (l_genl_msg_get_error(msg) < 0)
		goto error;

	gtk_rsc = nl80211_parse_get_key_seq(msg);
	if (!gtk_rsc) {
		memset(zero_gtk_rsc, 0, 6);
		gtk_rsc = zero_gtk_rsc;
	}

	ap_start_rsna(sta, gtk_rsc);
	return;

error:
	ap_del_station(sta, MMPDU_REASON_CODE_UNSPECIFIED, true);
}

static void ap_stop_handshake_schedule(struct sta_state *sta)
{
	if (sta->stop_handshake_work)
		return;

	sta->stop_handshake_work = l_idle_create(ap_stop_handshake_work,
							sta, NULL);
}

static void ap_wsc_handshake_event(struct handshake_state *hs,
		enum handshake_event event, void *user_data, ...)
{
	struct sta_state *sta = user_data;
	va_list args;
	struct ap_event_registration_success_data event_data;
	struct ap_pbc_record_expiry_data expiry_data;

	va_start(args, user_data);

	switch (event) {
	case HANDSHAKE_EVENT_FAILED:
		sta->sm = NULL;
		ap_stop_handshake_schedule(sta);
		/*
		 * Some diagrams in WSC v2.0.5 indicate we should
		 * automatically deauthenticate the Enrollee.  The text
		 * generally indicates the Enrollee may disassociate
		 * meaning that we should neither deauthenticate nor
		 * disassociate it automatically.  Some places indicate
		 * that the enrollee can send a new EAPoL-Start right away
		 * on an unsuccessful registration, we don't implement
		 * this for now.  STA remains associated but not authorized
		 * and basically has no other option than to re-associate
		 * or disassociate/deauthenticate.
		 */
		break;
	case HANDSHAKE_EVENT_EAP_NOTIFY:
		if (va_arg(args, unsigned int) != EAP_WSC_EVENT_CREDENTIAL_SENT)
			break;

		/*
		 * WSC v2.0.5 Section 11.3:
		 * "If the Registrar successfully runs the PBC method to
		 * completion with an Enrollee, that Enrollee's probe requests
		 * are removed from the Monitor Time check the next time the
		 * Registrar's PBC button is pressed."
		 */
		expiry_data.min_time = 0;
		expiry_data.mac = sta->addr;
		l_queue_foreach_remove(sta->ap->wsc_pbc_probes,
					ap_wsc_pbc_record_expire,
					&expiry_data);

		event_data.mac = sta->addr;
		sta->ap->ops->handle_event(AP_EVENT_REGISTRATION_SUCCESS,
						&event_data,
						sta->ap->user_data);
		break;
	default:
		break;
	}

	va_end(args);
}

static void ap_start_eap_wsc(struct sta_state *sta)
{
	struct ap_state *ap = sta->ap;

	/*
	 * WSC v2.0.5 Section 8.2: "The AP is allowed to send
	 * EAP-Request/Identity to the station before EAPOL-Start is received
	 * if a WSC IE is included in the (re)association request and the
	 * WSC IE is version 2.0 or higher.
	 */
	bool wait_for_eapol_start = !sta->wsc_v2;

	sta->wsc_settings = l_settings_new();
	l_settings_set_string(sta->wsc_settings, "Security", "EAP-Method",
				"WSC-R");
	l_settings_set_string(sta->wsc_settings, "WSC", "EnrolleeMAC",
				util_address_to_string(sta->addr));
	l_settings_set_bytes(sta->wsc_settings, "WSC", "UUID-R",
				ap->wsc_uuid_r, 16);
	l_settings_set_bytes(sta->wsc_settings, "WSC", "UUID-E",
				sta->wsc_uuid_e, 16);
	l_settings_set_uint(sta->wsc_settings, "WSC", "RFBand",
				WSC_RF_BAND_2_4_GHZ);
	l_settings_set_uint(sta->wsc_settings, "WSC", "ConfigurationMethods",
				WSC_CONFIGURATION_METHOD_PUSH_BUTTON);
	l_settings_set_string(sta->wsc_settings, "WSC", "WPA2-SSID",
				ap->config->ssid);

	if (ap->config->passphrase[0])
		l_settings_set_string(sta->wsc_settings,
					"WSC", "WPA2-Passphrase",
					ap->config->passphrase);
	else
		l_settings_set_bytes(sta->wsc_settings,
					"WSC", "WPA2-PSK", ap->config->psk, 32);

	sta->hs = netdev_handshake_state_new(ap->netdev);
	handshake_state_set_authenticator(sta->hs, true);
	handshake_state_set_event_func(sta->hs, ap_wsc_handshake_event, sta);
	handshake_state_set_8021x_config(sta->hs, sta->wsc_settings);

	ap_start_handshake(sta, wait_for_eapol_start, NULL);
}

static struct l_genl_msg *ap_build_cmd_del_key(struct ap_state *ap)
{
	uint32_t ifindex = netdev_get_ifindex(ap->netdev);
	struct l_genl_msg *msg;

	msg = l_genl_msg_new_sized(NL80211_CMD_DEL_KEY, 128);

	l_genl_msg_append_attr(msg, NL80211_ATTR_IFINDEX, 4, &ifindex);
	l_genl_msg_enter_nested(msg, NL80211_ATTR_KEY);
	l_genl_msg_append_attr(msg, NL80211_KEY_IDX, 1, &ap->gtk_index);
	l_genl_msg_leave_nested(msg);

	return msg;
}

static struct l_genl_msg *ap_build_cmd_new_station(struct sta_state *sta)
{
	struct l_genl_msg *msg;
	uint32_t ifindex = netdev_get_ifindex(sta->ap->netdev);
	/*
	 * This should hopefully work both with and without
	 * NL80211_FEATURE_FULL_AP_CLIENT_STATE.
	 */
	struct nl80211_sta_flag_update flags = {
		.mask = (1 << NL80211_STA_FLAG_AUTHENTICATED) |
			(1 << NL80211_STA_FLAG_ASSOCIATED) |
			(1 << NL80211_STA_FLAG_AUTHORIZED) |
			(1 << NL80211_STA_FLAG_MFP),
		.set = (1 << NL80211_STA_FLAG_AUTHENTICATED) |
			(1 << NL80211_STA_FLAG_ASSOCIATED),
	};

	msg = l_genl_msg_new_sized(NL80211_CMD_NEW_STATION, 300);

	l_genl_msg_append_attr(msg, NL80211_ATTR_IFINDEX, 4, &ifindex);
	l_genl_msg_append_attr(msg, NL80211_ATTR_MAC, 6, sta->addr);
	l_genl_msg_append_attr(msg, NL80211_ATTR_STA_FLAGS2, 8, &flags);

	return msg;
}

static void ap_gtk_op_cb(struct l_genl_msg *msg, void *user_data)
{
	if (l_genl_msg_get_error(msg) < 0) {
		uint8_t cmd = l_genl_msg_get_command(msg);
		const char *cmd_name =
			cmd == NL80211_CMD_NEW_KEY ? "NEW_KEY" :
			cmd == NL80211_CMD_SET_KEY ? "SET_KEY" :
			"DEL_KEY";

		l_error("%s failed for the GTK: %i",
			cmd_name, l_genl_msg_get_error(msg));
	}
}

static void ap_associate_sta_cb(struct l_genl_msg *msg, void *user_data)
{
	struct sta_state *sta = user_data;
	struct ap_state *ap = sta->ap;

	if (l_genl_msg_get_error(msg) < 0) {
		l_error("NEW_STATION/SET_STATION failed: %i",
			l_genl_msg_get_error(msg));
		return;
	}

	/*
	 * WSC v2.0.5 Section 8.2:
	 * "Therefore if a WSC IE is present in the (re)association request,
	 * the AP shall engage in EAP-WSC with the station and shall not
	 * attempt any other security handshake."
	 *
	 * So no need for group traffic, skip the GTK setup below.
	 */
	if (!sta->assoc_rsne) {
		ap_start_eap_wsc(sta);
		return;
	}

	/*
	 * Set up the group key.  If this is our first STA then we have
	 * to add the new GTK to the kernel.  In theory we should be
	 * able to supply our own RSC (e.g. generated randomly) and use it
	 * immediately for our 4-Way Handshake without querying the kernel.
	 * However NL80211_CMD_NEW_KEY only lets us set the receive RSC --
	 * the Rx PN for CCMP and the Rx IV for TKIP -- and the
	 * transmit RSC always starts as all zeros.  There's effectively
	 * no way to set the Tx RSC or query the Rx RSC through nl80211.
	 * So we query the Tx RSC in both scenarios just in case some
	 * driver/hardware uses a different initial Tx RSC.
	 *
	 * Optimally we would get called back by the EAPoL state machine
	 * only when building the step 3 of 4 message to query the RSC as
	 * late as possible but that would complicate EAPoL.
	 */
	if (ap->group_cipher != IE_RSN_CIPHER_SUITE_NO_GROUP_TRAFFIC &&
			!ap->gtk_set) {
		enum crypto_cipher group_cipher =
			ie_rsn_cipher_suite_to_cipher(ap->group_cipher);
		int gtk_len = crypto_cipher_key_len(group_cipher);

		/*
		 * Generate our GTK.  Not following the example derivation
		 * method in 802.11-2016 section 12.7.1.4 because a simple
		 * l_getrandom is just as good.
		 */
		l_getrandom(ap->gtk, gtk_len);
		ap->gtk_index = 1;

		msg = nl80211_build_new_key_group(
						netdev_get_ifindex(ap->netdev),
						group_cipher, ap->gtk_index,
						ap->gtk, gtk_len, NULL,
						0, NULL);

		if (!l_genl_family_send(ap->nl80211, msg, ap_gtk_op_cb, NULL,
					NULL)) {
			l_genl_msg_unref(msg);
			l_error("Issuing NEW_KEY failed");
			goto error;
		}

		msg = nl80211_build_set_key(netdev_get_ifindex(ap->netdev),
						ap->gtk_index);
		if (!l_genl_family_send(ap->nl80211, msg, ap_gtk_op_cb, NULL,
					NULL)) {
			l_genl_msg_unref(msg);
			l_error("Issuing SET_KEY failed");
			goto error;
		}

		/*
		 * Set the flag now because any new associating STA will
		 * just use NL80211_CMD_GET_KEY from now.
		 */
		ap->gtk_set = true;
	}

	if (ap->group_cipher == IE_RSN_CIPHER_SUITE_NO_GROUP_TRAFFIC)
		ap_start_rsna(sta, NULL);
	else {
		msg = nl80211_build_get_key(netdev_get_ifindex(ap->netdev),
					ap->gtk_index);
		sta->gtk_query_cmd_id = l_genl_family_send(ap->nl80211, msg,
								ap_gtk_query_cb,
								sta, NULL);
		if (!sta->gtk_query_cmd_id) {
			l_genl_msg_unref(msg);
			l_error("Issuing GET_KEY failed");
			goto error;
		}
	}

	return;

error:
	ap_del_station(sta, MMPDU_REASON_CODE_UNSPECIFIED, true);
}

static void ap_associate_sta(struct ap_state *ap, struct sta_state *sta)
{
	struct l_genl_msg *msg;
	uint32_t ifindex = netdev_get_ifindex(ap->netdev);

	uint8_t rates[256];
	uint32_t r, minr, maxr, count = 0;
	uint16_t capability = l_get_le16(&sta->capability);

	if (sta->associated)
		msg = nl80211_build_set_station_associated(ifindex, sta->addr);
	else
		msg = ap_build_cmd_new_station(sta);

	sta->associated = true;
	sta->rsna = false;

	minr = l_uintset_find_min(sta->rates);
	maxr = l_uintset_find_max(sta->rates);

	for (r = minr; r <= maxr; r++)
		if (l_uintset_contains(sta->rates, r))
			rates[count++] = r;

	l_genl_msg_append_attr(msg, NL80211_ATTR_STA_AID, 2, &sta->aid);
	l_genl_msg_append_attr(msg, NL80211_ATTR_STA_SUPPORTED_RATES,
				count, &rates);
	l_genl_msg_append_attr(msg, NL80211_ATTR_STA_LISTEN_INTERVAL, 2,
				&sta->listen_interval);
	l_genl_msg_append_attr(msg, NL80211_ATTR_STA_CAPABILITY, 2,
				&capability);

	if (!l_genl_family_send(ap->nl80211, msg, ap_associate_sta_cb,
								sta, NULL)) {
		l_genl_msg_unref(msg);
		if (l_genl_msg_get_command(msg) == NL80211_CMD_NEW_STATION)
			l_error("Issuing NEW_STATION failed");
		else
			l_error("Issuing SET_STATION failed");
	}
}

static bool ap_common_rates(struct l_uintset *ap_rates,
				struct l_uintset *sta_rates)
{
	uint32_t minr = l_uintset_find_min(ap_rates);

	/* Our lowest rate is a Basic Rate so must be supported */
	if (l_uintset_contains(sta_rates, minr))
		return true;

	return false;
}

static void ap_success_assoc_resp_cb(int err, void *user_data)
{
	struct sta_state *sta = user_data;
	struct ap_state *ap = sta->ap;

	sta->assoc_resp_cmd_id = 0;

	if (err) {
		if (err == -ECOMM)
			l_error("AP (Re)Association Response received no ACK");
		else
			l_error("AP (Re)Association Response not sent %s (%i)",
				strerror(-err), -err);

		/* If we were in State 3 or 4 go to back to State 2 */
		if (sta->associated)
			ap_del_station(sta, MMPDU_REASON_CODE_UNSPECIFIED,
					true);

		return;
	}

	/* If we were in State 2, 3 or 4 also go to State 3 */
	ap_associate_sta(ap, sta);

	l_info("AP (Re)Association Response ACK received");
}

static void ap_fail_assoc_resp_cb(int err, void *user_data)
{
	if (err == -ECOMM)
		l_error("AP (Re)Association Response with an error status "
			"received no ACK");
	else if (err)
		l_error("AP (Re)Association Response with an error status "
			"not sent: %s (%i)", strerror(-err), -err);
	else
		l_info("AP (Re)Association Response with an error status "
			"delivered OK");
}

static uint32_t ap_assoc_resp(struct ap_state *ap, struct sta_state *sta,
				const uint8_t *dest,
				enum mmpdu_reason_code status_code,
				bool reassoc, const struct mmpdu_header *req,
				size_t req_len, frame_xchg_cb_t callback)
{
	const uint8_t *addr = netdev_get_address(ap->netdev);
	enum mpdu_management_subtype stype = reassoc ?
		MPDU_MANAGEMENT_SUBTYPE_REASSOCIATION_RESPONSE :
		MPDU_MANAGEMENT_SUBTYPE_ASSOCIATION_RESPONSE;
	L_AUTO_FREE_VAR(uint8_t *, mpdu_buf) =
		l_malloc(128 + ap_get_extra_ies_len(ap, stype, req, req_len));
	struct mmpdu_header *mpdu = (void *) mpdu_buf;
	struct mmpdu_association_response *resp;
	size_t ies_len = 0;
	uint16_t capability = IE_BSS_CAP_ESS | IE_BSS_CAP_PRIVACY;
	uint32_t r, minr, maxr, count;

	memset(mpdu, 0, sizeof(*mpdu));

	/* Header */
	mpdu->fc.protocol_version = 0;
	mpdu->fc.type = MPDU_TYPE_MANAGEMENT;
	mpdu->fc.subtype = stype;
	memcpy(mpdu->address_1, dest, 6);	/* DA */
	memcpy(mpdu->address_2, addr, 6);	/* SA */
	memcpy(mpdu->address_3, addr, 6);	/* BSSID */

	/* Association Response body */
	resp = (void *) mmpdu_body(mpdu);
	l_put_le16(capability, &resp->capability);
	resp->status_code = L_CPU_TO_LE16(status_code);
	resp->aid = sta ? L_CPU_TO_LE16(sta->aid | 0xc000) : 0;

	/* Supported Rates IE */
	resp->ies[ies_len++] = IE_TYPE_SUPPORTED_RATES;

	minr = l_uintset_find_min(ap->rates);
	maxr = l_uintset_find_max(ap->rates);
	count = 0;
	for (r = minr; r <= maxr && count < 8; r++)
		if (l_uintset_contains(ap->rates, r)) {
			uint8_t flag = 0;

			/* Mark only the lowest rate as Basic Rate */
			if (count == 0)
				flag = 0x80;

			resp->ies[ies_len + 1 + count++] = r | flag;
		}

	resp->ies[ies_len++] = count;
	ies_len += count;

	ies_len += ap_write_extra_ies(ap, stype, req, req_len,
					resp->ies + ies_len);

	return ap_send_mgmt_frame(ap, mpdu, resp->ies + ies_len - mpdu_buf,
					callback, sta);
}

static int ap_parse_supported_rates(struct ie_tlv_iter *iter,
					struct l_uintset **set)
{
	const uint8_t *rates;
	unsigned int len;
	unsigned int i;

	len = ie_tlv_iter_get_length(iter);

	if (ie_tlv_iter_get_tag(iter) == IE_TYPE_SUPPORTED_RATES && len == 0)
		return -EINVAL;

	rates = ie_tlv_iter_get_data(iter);

	if (!*set)
		*set = l_uintset_new(108);

	for (i = 0; i < len; i++) {
		if (rates[i] == 0xff)
			continue;

		l_uintset_put(*set, rates[i] & 0x7f);
	}

	return 0;
}

/*
 * This handles both the Association and Reassociation Request frames.
 * Association Request is documented in 802.11-2016 9.3.3.6 (frame format),
 * 802.11-2016 11.3.5.3 (MLME/SME) and Reassociation in 802.11-2016
 * 9.3.3.8 (frame format), 802.11-2016 11.3.5.3 (MLME/SME).
 *
 * The difference between Association and Reassociation procedures is
 * documented in 11.3.5.1 "General" but seems inconsistent with specific
 * instructions in 11.3.5.3 vs. 11.3.5.5 and 11.3.5.2 vs. 11.3.5.4.
 * According to 11.3.5.1:
 *  1. Reassociation requires the STA to be already associated in the ESS,
 *     Association doesn't.
 *  2. Unsuccessful Reassociation should not cause a state transition of
 *     the authentication state between the two STAs.
 *
 * The first requirement is not present in 11.3.5.5 which is virtually
 * identical with 11.3.5.3, but we do implement it.  Number 2 is also not
 * reflected in 11.3.5.5 where the state transitions are the same as in
 * 11.3.5.3 and 11.3.5.4 where the state transitions are the same as in
 * 11.3.5.2 including f) "If a Reassociation Response frame is received
 * with a status code other than SUCCESS [...] 1. [...] the state for
 * the AP [...] shall be set to State 2 [...]"
 *
 * For the record here are the apparent differences between 802.11-2016
 * 11.3.5.2 and 11.3.5.4 ignoring the s/Associate/Reassociate/ changes
 * and the special case of Reassociation during a Fast Transition.
 *  o Points c) and d) are switched around.
 *  o On success, the STA is disassociated from all other APs in 11.3.5.2,
 *    and from the previous AP in 11.3.5.4 c).  (Shouldn't make a
 *    difference as there seems to be no way for the STA to become
 *    associated with more than one AP)
 *  o After Association a 4-Way Handshake is always performed, after
 *    Reassociation it is only performed if STA was in State 3 according
 *    to 11.3.5.4 g).  This is not reflected in 11.3.5.5 though.
 *    Additionally 11.3.5.4 and 11.3.5.5 require the STA and AP
 *    respectively to delete current PTKSA/GTKSA/IGTKSA at the beginning
 *    of the procedure independent of the STA state so without a 4-Way
 *    Handshake the two stations end up with no encryption keys.
 *
 * The main difference between 11.3.5.3 and 11.3.5.5 is presence of p).
 */
static void ap_assoc_reassoc(struct sta_state *sta, bool reassoc,
				const struct mmpdu_field_capability *capability,
				uint16_t listen_interval,
				const uint8_t *ies, size_t ies_len,
				const struct mmpdu_header *req)
{
	struct ap_state *ap = sta->ap;
	const char *ssid = NULL;
	const uint8_t *rsn = NULL;
	size_t ssid_len = 0;
	struct l_uintset *rates = NULL;
	struct ie_rsn_info rsn_info;
	int err;
	struct ie_tlv_iter iter;
	uint8_t *wsc_data = NULL;
	ssize_t wsc_data_len;

	if (sta->assoc_resp_cmd_id)
		return;

	if (reassoc && !sta->associated) {
		err = MMPDU_REASON_CODE_CLASS3_FRAME_FROM_NONASSOC_STA;
		goto unsupported;
	}

	wsc_data = ie_tlv_extract_wsc_payload(ies, ies_len, &wsc_data_len);

	ie_tlv_iter_init(&iter, ies, ies_len);

	while (ie_tlv_iter_next(&iter))
		switch (ie_tlv_iter_get_tag(&iter)) {
		case IE_TYPE_SSID:
			ssid = (const char *) ie_tlv_iter_get_data(&iter);
			ssid_len = ie_tlv_iter_get_length(&iter);
			break;

		case IE_TYPE_SUPPORTED_RATES:
		case IE_TYPE_EXTENDED_SUPPORTED_RATES:
			if (ap_parse_supported_rates(&iter, &rates) < 0) {
				err = MMPDU_REASON_CODE_INVALID_IE;
				goto bad_frame;
			}

			break;

		case IE_TYPE_RSN:
			/*
			 * WSC v2.0.5 Section 8.2:
			 * "Note that during the WSC association [...] the
			 * RSN IE and the WPA IE are irrelevant and shall be
			 * ignored by both the station and AP."
			 */
			if (wsc_data)
				break;

			if (ie_parse_rsne(&iter, &rsn_info) < 0) {
				err = MMPDU_REASON_CODE_INVALID_IE;
				goto bad_frame;
			}

			rsn = (const uint8_t *) ie_tlv_iter_get_data(&iter) - 2;
			break;
		}

	if (!rates || !ssid || (!wsc_data && !rsn) ||
			ssid_len != strlen(ap->config->ssid) ||
			memcmp(ssid, ap->config->ssid, ssid_len)) {
		err = MMPDU_REASON_CODE_INVALID_IE;
		goto bad_frame;
	}

	if (!ap_common_rates(ap->rates, rates)) {
		err = MMPDU_REASON_CODE_UNSPECIFIED;
		goto unsupported;
	}

	/* Is the client requesting RSNA establishment or WSC registration */
	if (!rsn) {
		struct wsc_association_request wsc_req;
		struct ap_event_registration_start_data event_data;
		struct ap_wsc_pbc_probe_record *record;

		if (wsc_parse_association_request(wsc_data, wsc_data_len,
							&wsc_req) < 0) {
			err = MMPDU_REASON_CODE_INVALID_IE;
			goto bad_frame;
		}

		l_free(wsc_data);
		wsc_data = NULL;

		if (wsc_req.request_type !=
				WSC_REQUEST_TYPE_ENROLLEE_OPEN_8021X) {
			err = MMPDU_REASON_CODE_INVALID_IE;
			goto bad_frame;
		}

		if (!ap->wsc_pbc_timeout) {
			l_debug("WSC association from %s but we're not in "
				"PBC mode", util_address_to_string(sta->addr));
			err = MMPDU_REASON_CODE_UNSPECIFIED;
			goto bad_frame;
		}

		if (l_queue_isempty(ap->wsc_pbc_probes)) {
			l_debug("%s tried to register as enrollee but we "
				"don't have their Probe Request record",
				util_address_to_string(sta->addr));
			err = MMPDU_REASON_CODE_UNSPECIFIED;
			goto bad_frame;
		}

		/*
		 * For PBC, the Enrollee must have sent the only PBC Probe
		 * Request within the monitor time and walk time.
		 */
		record = l_queue_peek_head(ap->wsc_pbc_probes);
		if (memcmp(sta->addr, record->mac, 6)) {
			l_debug("Session overlap during %s's attempt to "
				"register as WSC enrollee",
				util_address_to_string(sta->addr));
			err = MMPDU_REASON_CODE_UNSPECIFIED;
			goto bad_frame;
		}

		memcpy(sta->wsc_uuid_e, record->uuid_e, 16);
		sta->wsc_v2 = wsc_req.version2;

		event_data.mac = sta->addr;
		event_data.assoc_ies = ies;
		event_data.assoc_ies_len = ies_len;
		ap->ops->handle_event(AP_EVENT_REGISTRATION_START, &event_data,
					ap->user_data);

		/*
		 * Since we're starting the PBC Registration Protocol
		 * we can now exit the "active PBC mode".
		 */
		ap_wsc_exit_pbc(ap);
	} else {
		if (rsn_info.mfpr && rsn_info.spp_a_msdu_required) {
			err = MMPDU_REASON_CODE_UNSPECIFIED;
			goto unsupported;
		}

		if (__builtin_popcount(rsn_info.pairwise_ciphers) != 1 ||
				!(rsn_info.pairwise_ciphers & ap->ciphers)) {
			err = MMPDU_REASON_CODE_INVALID_PAIRWISE_CIPHER;
			goto unsupported;
		}

		if (rsn_info.akm_suites != IE_RSN_AKM_SUITE_PSK) {
			err = MMPDU_REASON_CODE_INVALID_AKMP;
			goto unsupported;
		}
	}

	/* 802.11-2016 11.3.5.3 j) */
	if (sta->rsna)
		ap_drop_rsna(sta);
	else if (sta->associated)
		ap_stop_handshake(sta);

	if (!sta->associated) {
		/*
		 * Everything fine so far, assign an AID, send response.
		 * According to 802.11-2016 11.3.5.3 l) we will only go to
		 * State 3 (set sta->associated) once we receive the station's
		 * ACK or gave up on resends.
		 */
		sta->aid = ++ap->last_aid;
	}

	sta->capability = *capability;
	sta->listen_interval = listen_interval;

	if (sta->rates)
		l_uintset_free(sta->rates);

	sta->rates = rates;

	l_free(sta->assoc_ies);

	if (rsn) {
		sta->assoc_ies = l_memdup(ies, ies_len);
		sta->assoc_ies_len = ies_len;
		sta->assoc_rsne = sta->assoc_ies + (rsn - ies);
	} else {
		sta->assoc_ies = NULL;
		sta->assoc_rsne = NULL;
	}

	sta->assoc_resp_cmd_id = ap_assoc_resp(ap, sta, sta->addr, 0, reassoc,
						req, (void *) ies + ies_len -
						(void *) req,
						ap_success_assoc_resp_cb);
	if (!sta->assoc_resp_cmd_id)
		l_error("Sending success (Re)Association Response failed");

	return;

unsupported:
bad_frame:
	/*
	 * TODO: MFP
	 *
	 * 802.11-2016 11.3.5.3 m)
	 * "If the ResultCode in the MLME-ASSOCIATE.response primitive is
	 * not SUCCESS and management frame protection is in use the state
	 * for the STA shall be left unchanged.  If the ResultCode is not
	 * SUCCESS and management frame protection is not in use the state
	 * for the STA shall be set to State 3 if it was State 4."
	 *
	 * For now, we need to drop the RSNA.
	 */
	if (sta->rsna)
		ap_drop_rsna(sta);
	else if (sta->associated)
		ap_stop_handshake(sta);

	if (rates)
		l_uintset_free(rates);

	l_free(wsc_data);

	if (!ap_assoc_resp(ap, sta, sta->addr, err, reassoc,
				req, (void *) ies + ies_len - (void *) req,
				ap_fail_assoc_resp_cb))
		l_error("Sending error (Re)Association Response failed");
}

/* 802.11-2016 9.3.3.6 */
static void ap_assoc_req_cb(const struct mmpdu_header *hdr, const void *body,
				size_t body_len, int rssi, void *user_data)
{
	struct ap_state *ap = user_data;
	struct sta_state *sta;
	const uint8_t *from = hdr->address_2;
	const struct mmpdu_association_request *req = body;
	const uint8_t *bssid = netdev_get_address(ap->netdev);

	l_info("AP Association Request from %s", util_address_to_string(from));

	if (memcmp(hdr->address_1, bssid, 6) ||
			memcmp(hdr->address_3, bssid, 6))
		return;

	sta = l_queue_find(ap->sta_states, ap_sta_match_addr, from);
	if (!sta) {
		if (!ap_assoc_resp(ap, NULL, from,
				MMPDU_REASON_CODE_STA_REQ_ASSOC_WITHOUT_AUTH,
				false, hdr, body + body_len - (void *) hdr,
				ap_fail_assoc_resp_cb))
			l_error("Sending error Association Response failed");

		return;
	}

	ap_assoc_reassoc(sta, false, &req->capability,
				L_LE16_TO_CPU(req->listen_interval),
				req->ies, body_len - sizeof(*req), hdr);
}

/* 802.11-2016 9.3.3.8 */
static void ap_reassoc_req_cb(const struct mmpdu_header *hdr, const void *body,
				size_t body_len, int rssi, void *user_data)
{
	struct ap_state *ap = user_data;
	struct sta_state *sta;
	const uint8_t *from = hdr->address_2;
	const struct mmpdu_reassociation_request *req = body;
	const uint8_t *bssid = netdev_get_address(ap->netdev);
	int err;

	l_info("AP Reassociation Request from %s",
		util_address_to_string(from));

	if (memcmp(hdr->address_1, bssid, 6) ||
			memcmp(hdr->address_3, bssid, 6))
		return;

	sta = l_queue_find(ap->sta_states, ap_sta_match_addr, from);
	if (!sta) {
		err = MMPDU_REASON_CODE_STA_REQ_ASSOC_WITHOUT_AUTH;
		goto bad_frame;
	}

	if (memcmp(req->current_ap_address, bssid, 6)) {
		err = MMPDU_REASON_CODE_UNSPECIFIED;
		goto bad_frame;
	}

	ap_assoc_reassoc(sta, true, &req->capability,
				L_LE16_TO_CPU(req->listen_interval),
				req->ies, body_len - sizeof(*req), hdr);
	return;

bad_frame:
	if (!ap_assoc_resp(ap, NULL, from, err, true,
				hdr, body + body_len - (void *) hdr,
				ap_fail_assoc_resp_cb))
		l_error("Sending error Reassociation Response failed");
}

static void ap_probe_resp_cb(int err, void *user_data)
{
	if (err == -ECOMM)
		l_error("AP Probe Response received no ACK");
	else if (err)
		l_error("AP Probe Response not sent: %s (%i)",
			strerror(-err), -err);
	else
		l_info("AP Probe Response delivered OK");
}

/*
 * Parse Probe Request according to 802.11-2016 9.3.3.10 and act according
 * to 802.11-2016 11.1.4.3
 */
static void ap_probe_req_cb(const struct mmpdu_header *hdr, const void *body,
				size_t body_len, int rssi, void *user_data)
{
	struct ap_state *ap = user_data;
	const struct mmpdu_probe_request *req = body;
	const char *ssid = NULL;
	const uint8_t *ssid_list = NULL;
	size_t ssid_len = 0, ssid_list_len = 0, len;
	uint8_t dsss_channel = 0;
	struct ie_tlv_iter iter;
	const uint8_t *bssid = netdev_get_address(ap->netdev);
	bool match = false;
	L_AUTO_FREE_VAR(uint8_t *, resp) =
		l_malloc(512 + ap_get_extra_ies_len(ap,
				MPDU_MANAGEMENT_SUBTYPE_PROBE_RESPONSE, hdr,
				body + body_len - (void *) hdr));

	l_info("AP Probe Request from %s",
		util_address_to_string(hdr->address_2));

	ie_tlv_iter_init(&iter, req->ies, body_len - sizeof(*req));

	while (ie_tlv_iter_next(&iter))
		switch (ie_tlv_iter_get_tag(&iter)) {
		case IE_TYPE_SSID:
			ssid = (const char *) ie_tlv_iter_get_data(&iter);
			ssid_len = ie_tlv_iter_get_length(&iter);
			break;

		case IE_TYPE_SSID_LIST:
			ssid_list = ie_tlv_iter_get_data(&iter);
			ssid_list_len = ie_tlv_iter_get_length(&iter);
			break;

		case IE_TYPE_DSSS_PARAMETER_SET:
			if (ie_tlv_iter_get_length(&iter) != 1)
				return;

			dsss_channel = ie_tlv_iter_get_data(&iter)[0];
			break;
		}

	/*
	 * Check if we should reply to this Probe Request according to
	 * 802.11-2016 section 11.1.4.3.2.
	 */

	if (memcmp(hdr->address_1, bssid, 6) &&
			!util_is_broadcast_address(hdr->address_1))
		return;

	if (memcmp(hdr->address_3, bssid, 6) &&
			!util_is_broadcast_address(hdr->address_3))
		return;

	if (!ssid || ssid_len == 0) /* Wildcard SSID */
		match = true;
	else if (ssid && ssid_len == strlen(ap->config->ssid) && /* One SSID */
			!memcmp(ssid, ap->config->ssid, ssid_len))
		match = true;
	else if (ssid && ssid_len == 7 && !memcmp(ssid, "DIRECT-", 7) &&
			!memcmp(ssid, ap->config->ssid, 7)) /* P2P wildcard */
		match = true;
	else if (ssid_list) { /* SSID List */
		ie_tlv_iter_init(&iter, ssid_list, ssid_list_len);

		while (ie_tlv_iter_next(&iter)) {
			if (ie_tlv_iter_get_tag(&iter) != IE_TYPE_SSID)
				return;

			ssid = (const char *) ie_tlv_iter_get_data(&iter);
			ssid_len = ie_tlv_iter_get_length(&iter);

			if (ssid_len == strlen(ap->config->ssid) &&
					!memcmp(ssid, ap->config->ssid,
						ssid_len)) {
				match = true;
				break;
			}
		}
	}

	if (dsss_channel != 0 && dsss_channel != ap->config->channel)
		match = false;

	if (!match)
		return;

	len = ap_build_beacon_pr_head(ap,
					MPDU_MANAGEMENT_SUBTYPE_PROBE_RESPONSE,
					hdr->address_2, resp, sizeof(resp));
	len += ap_build_beacon_pr_tail(ap,
					MPDU_MANAGEMENT_SUBTYPE_PROBE_RESPONSE,
					hdr, body + body_len - (void *) hdr,
					resp + len);

	ap_send_mgmt_frame(ap, (struct mmpdu_header *) resp, len,
				ap_probe_resp_cb, NULL);
}

/* 802.11-2016 9.3.3.5 (frame format), 802.11-2016 11.3.5.9 (MLME/SME) */
static void ap_disassoc_cb(const struct mmpdu_header *hdr, const void *body,
				size_t body_len, int rssi, void *user_data)
{
	struct ap_state *ap = user_data;
	struct sta_state *sta;
	const struct mmpdu_disassociation *disassoc = body;
	const uint8_t *bssid = netdev_get_address(ap->netdev);

	l_info("AP Disassociation from %s, reason %i",
		util_address_to_string(hdr->address_2),
		(int) L_LE16_TO_CPU(disassoc->reason_code));

	if (memcmp(hdr->address_1, bssid, 6) ||
			memcmp(hdr->address_3, bssid, 6))
		return;

	sta = l_queue_find(ap->sta_states, ap_sta_match_addr, hdr->address_2);

	if (sta && sta->assoc_resp_cmd_id) {
		l_genl_family_cancel(ap->nl80211, sta->assoc_resp_cmd_id);
		sta->assoc_resp_cmd_id = 0;
	}

	if (!sta || !sta->associated)
		return;

	ap_del_station(sta, L_LE16_TO_CPU(disassoc->reason_code), true);
}

static void ap_auth_reply_cb(int err, void *user_data)
{
	if (err == -ECOMM)
		l_error("AP Authentication frame 2 received no ACK");
	else if (err)
		l_error("AP Authentication frame 2 not sent: %s (%i)",
			strerror(-err), -err);
	else
		l_info("AP Authentication frame 2 ACKed by STA");
}

static void ap_auth_reply(struct ap_state *ap, const uint8_t *dest,
				enum mmpdu_reason_code status_code)
{
	const uint8_t *addr = netdev_get_address(ap->netdev);
	uint8_t mpdu_buf[64];
	struct mmpdu_header *mpdu = (struct mmpdu_header *) mpdu_buf;
	struct mmpdu_authentication *auth;

	memset(mpdu, 0, sizeof(*mpdu));

	/* Header */
	mpdu->fc.protocol_version = 0;
	mpdu->fc.type = MPDU_TYPE_MANAGEMENT;
	mpdu->fc.subtype = MPDU_MANAGEMENT_SUBTYPE_AUTHENTICATION;
	memcpy(mpdu->address_1, dest, 6);	/* DA */
	memcpy(mpdu->address_2, addr, 6);	/* SA */
	memcpy(mpdu->address_3, addr, 6);	/* BSSID */

	/* Authentication body */
	auth = (void *) mmpdu_body(mpdu);
	auth->algorithm = L_CPU_TO_LE16(MMPDU_AUTH_ALGO_OPEN_SYSTEM);
	auth->transaction_sequence = L_CPU_TO_LE16(2);
	auth->status = L_CPU_TO_LE16(status_code);

	ap_send_mgmt_frame(ap, mpdu, (uint8_t *) auth + 6 - mpdu_buf,
				ap_auth_reply_cb, NULL);
}

/*
 * 802.11-2016 9.3.3.12 (frame format), 802.11-2016 11.3.4.3 and
 * 802.11-2016 12.3.3.2 (MLME/SME)
 */
static void ap_auth_cb(const struct mmpdu_header *hdr, const void *body,
			size_t body_len, int rssi, void *user_data)
{
	struct ap_state *ap = user_data;
	const struct mmpdu_authentication *auth = body;
	const uint8_t *from = hdr->address_2;
	const uint8_t *bssid = netdev_get_address(ap->netdev);
	struct sta_state *sta;

	l_info("AP Authentication from %s", util_address_to_string(from));

	if (memcmp(hdr->address_1, bssid, 6) ||
			memcmp(hdr->address_3, bssid, 6))
		return;

	if (ap->config->authorized_macs_num) {
		unsigned int i;

		for (i = 0; i < ap->config->authorized_macs_num; i++)
			if (!memcmp(from, ap->config->authorized_macs + i * 6,
					6))
				break;

		if (i == ap->config->authorized_macs_num) {
			ap_auth_reply(ap, from, MMPDU_REASON_CODE_UNSPECIFIED);
			return;
		}
	}

	/* Only Open System authentication implemented here */
	if (L_LE16_TO_CPU(auth->algorithm) !=
			MMPDU_AUTH_ALGO_OPEN_SYSTEM) {
		ap_auth_reply(ap, from, MMPDU_REASON_CODE_UNSPECIFIED);
		return;
	}

	if (L_LE16_TO_CPU(auth->transaction_sequence) != 1) {
		ap_auth_reply(ap, from, MMPDU_REASON_CODE_UNSPECIFIED);
		return;
	}

	sta = l_queue_find(ap->sta_states, ap_sta_match_addr, from);

	/*
	 * Figure 11-13 in 802.11-2016 11.3.2 shows a transition from
	 * States 3 / 4 to State 2 on "Successful 802.11 Authentication"
	 * however 11.3.4.2 and 11.3.4.3 clearly say the connection goes to
	 * State 2 only if it was in State 1:
	 *
	 * "c) [...] the state for the indicated STA shall be set to State 2
	 * if it was State 1; the state shall remain unchanged if it was other
	 * than State 1."
	 */
	if (sta)
		goto done;

	/*
	 * Per 12.3.3.2.3 with Open System the state change is immediate,
	 * no waiting for the response to be ACKed as with the association
	 * frames.
	 */
	sta = l_new(struct sta_state, 1);
	memcpy(sta->addr, from, 6);
	sta->ap = ap;

	if (!ap->sta_states)
		ap->sta_states = l_queue_new();

	l_queue_push_tail(ap->sta_states, sta);

	/*
	 * Nothing to do here netlink-wise as we can't receive any data
	 * frames until after association anyway.  We do need to add a
	 * timeout for the authentication and possibly the kernel could
	 * handle that if we registered the STA with NEW_STATION now (TODO)
	 */

done:
	ap_auth_reply(ap, from, 0);
}

/* 802.11-2016 9.3.3.13 (frame format), 802.11-2016 11.3.4.5 (MLME/SME) */
static void ap_deauth_cb(const struct mmpdu_header *hdr, const void *body,
				size_t body_len, int rssi, void *user_data)
{
	struct ap_state *ap = user_data;
	struct sta_state *sta;
	const struct mmpdu_deauthentication *deauth = body;
	const uint8_t *bssid = netdev_get_address(ap->netdev);

	l_info("AP Deauthentication from %s, reason %i",
		util_address_to_string(hdr->address_2),
		(int) L_LE16_TO_CPU(deauth->reason_code));

	if (memcmp(hdr->address_1, bssid, 6) ||
			memcmp(hdr->address_3, bssid, 6))
		return;

	sta = l_queue_remove_if(ap->sta_states, ap_sta_match_addr,
				hdr->address_2);
	if (!sta)
		return;

	ap_del_station(sta, L_LE16_TO_CPU(deauth->reason_code), false);

	ap_sta_free(sta);
}

static void do_debug(const char *str, void *user_data)
{
	const char *prefix = user_data;

	l_info("%s%s", prefix, str);
}

static void ap_start_failed(struct ap_state *ap)
{
	ap->ops->handle_event(AP_EVENT_START_FAILED, NULL, ap->user_data);
	ap_reset(ap);
	l_genl_family_free(ap->nl80211);

	l_free(ap);
}

static void ap_start_cb(struct l_genl_msg *msg, void *user_data)
{
	struct ap_state *ap = user_data;

	ap->start_stop_cmd_id = 0;

	if (l_genl_msg_get_error(msg) < 0) {
		l_error("START_AP failed: %i", l_genl_msg_get_error(msg));

		goto failed;
	}

	if (ap->server && !l_dhcp_server_start(ap->server)) {
		l_error("DHCP server failed to start");
		goto failed;
	}

	ap->started = true;
	ap->ops->handle_event(AP_EVENT_STARTED, NULL, ap->user_data);

	return;

failed:
	ap_start_failed(ap);
}

static struct l_genl_msg *ap_build_cmd_start_ap(struct ap_state *ap)
{
	struct l_genl_msg *cmd;

	uint8_t head[256];
	L_AUTO_FREE_VAR(uint8_t *, tail) =
		l_malloc(256 + ap_get_extra_ies_len(ap,
						MPDU_MANAGEMENT_SUBTYPE_BEACON,
						NULL, 0));
	size_t head_len, tail_len;

	uint32_t dtim_period = 3;
	uint32_t ifindex = netdev_get_ifindex(ap->netdev);
	struct wiphy *wiphy = netdev_get_wiphy(ap->netdev);
	uint32_t hidden_ssid = NL80211_HIDDEN_SSID_NOT_IN_USE;
	unsigned int nl_ciphers_cnt = __builtin_popcount(ap->ciphers);
	uint32_t nl_ciphers[nl_ciphers_cnt];
	uint32_t group_nl_cipher =
		ie_rsn_cipher_suite_to_cipher(ap->group_cipher);
	uint32_t nl_akm = CRYPTO_AKM_PSK;
	uint32_t wpa_version = NL80211_WPA_VERSION_2;
	uint32_t auth_type = NL80211_AUTHTYPE_OPEN_SYSTEM;
	uint32_t ch_freq = scan_channel_to_freq(ap->config->channel,
						SCAN_BAND_2_4_GHZ);
	uint32_t ch_width = NL80211_CHAN_WIDTH_20;
	unsigned int i;

	static const uint8_t bcast_addr[6] = {
		0xff, 0xff, 0xff, 0xff, 0xff, 0xff
	};

	for (i = 0, nl_ciphers_cnt = 0; i < 8; i++)
		if (ap->ciphers & (1 << i))
			nl_ciphers[nl_ciphers_cnt++] =
				ie_rsn_cipher_suite_to_cipher(1 << i);

	head_len = ap_build_beacon_pr_head(ap, MPDU_MANAGEMENT_SUBTYPE_BEACON,
						bcast_addr, head, sizeof(head));
	tail_len = ap_build_beacon_pr_tail(ap, MPDU_MANAGEMENT_SUBTYPE_BEACON,
						NULL, 0, tail);

	if (!head_len || !tail_len)
		return NULL;

	cmd = l_genl_msg_new_sized(NL80211_CMD_START_AP, 256 + head_len +
					tail_len + strlen(ap->config->ssid));

	/* SET_BEACON attrs */
	l_genl_msg_append_attr(cmd, NL80211_ATTR_BEACON_HEAD, head_len, head);
	l_genl_msg_append_attr(cmd, NL80211_ATTR_BEACON_TAIL, tail_len, tail);
	l_genl_msg_append_attr(cmd, NL80211_ATTR_IE, 0, "");
	l_genl_msg_append_attr(cmd, NL80211_ATTR_IE_PROBE_RESP, 0, "");
	l_genl_msg_append_attr(cmd, NL80211_ATTR_IE_ASSOC_RESP, 0, "");

	/* START_AP attrs */
	l_genl_msg_append_attr(cmd, NL80211_ATTR_BEACON_INTERVAL, 4,
				&ap->beacon_interval);
	l_genl_msg_append_attr(cmd, NL80211_ATTR_DTIM_PERIOD, 4, &dtim_period);
	l_genl_msg_append_attr(cmd, NL80211_ATTR_IFINDEX, 4, &ifindex);
	l_genl_msg_append_attr(cmd, NL80211_ATTR_SSID, strlen(ap->config->ssid),
				ap->config->ssid);
	l_genl_msg_append_attr(cmd, NL80211_ATTR_HIDDEN_SSID, 4,
				&hidden_ssid);
	l_genl_msg_append_attr(cmd, NL80211_ATTR_CIPHER_SUITES_PAIRWISE,
				nl_ciphers_cnt * 4, nl_ciphers);
	l_genl_msg_append_attr(cmd, NL80211_ATTR_CIPHER_SUITE_GROUP, 4,
				&group_nl_cipher);
	l_genl_msg_append_attr(cmd, NL80211_ATTR_WPA_VERSIONS, 4, &wpa_version);
	l_genl_msg_append_attr(cmd, NL80211_ATTR_AKM_SUITES, 4, &nl_akm);
	l_genl_msg_append_attr(cmd, NL80211_ATTR_AUTH_TYPE, 4, &auth_type);
	l_genl_msg_append_attr(cmd, NL80211_ATTR_WIPHY_FREQ, 4, &ch_freq);
	l_genl_msg_append_attr(cmd, NL80211_ATTR_CHANNEL_WIDTH, 4, &ch_width);

	if (wiphy_has_ext_feature(wiphy,
			NL80211_EXT_FEATURE_CONTROL_PORT_OVER_NL80211)) {
		l_genl_msg_append_attr(cmd, NL80211_ATTR_SOCKET_OWNER, 0, NULL);
		l_genl_msg_append_attr(cmd,
				NL80211_ATTR_CONTROL_PORT_OVER_NL80211,
				0, NULL);
	}

	return cmd;
}

static void ap_ifaddr4_added_cb(int error, uint16_t type, const void *data,
				uint32_t len, void *user_data)
{
	struct ap_state *ap = user_data;
	struct l_genl_msg *cmd;

	ap->rtnl_add_cmd = 0;

	if (error) {
		l_error("Failed to set IP address");
		goto error;
	}

	cmd = ap_build_cmd_start_ap(ap);
	if (!cmd)
		goto error;

	ap->start_stop_cmd_id = l_genl_family_send(ap->nl80211, cmd,
							ap_start_cb, ap, NULL);
	if (!ap->start_stop_cmd_id) {
		l_genl_msg_unref(cmd);
		goto error;
	}

	return;

error:
	ap_start_failed(ap);
}

static bool ap_parse_new_station_ies(const void *data, uint16_t len,
					uint8_t **rsn_out,
					struct l_uintset **rates_out)
{
	struct ie_tlv_iter iter;
	uint8_t *rsn = NULL;
	struct l_uintset *rates = NULL;

	ie_tlv_iter_init(&iter, data, len);

	while (ie_tlv_iter_next(&iter)) {
		switch (ie_tlv_iter_get_tag(&iter)) {
		case IE_TYPE_RSN:
			if (rsn || ie_parse_rsne(&iter, NULL) < 0)
				goto parse_error;

			rsn = l_memdup(ie_tlv_iter_get_data(&iter) - 2,
					ie_tlv_iter_get_length(&iter) + 2);
			break;
		case IE_TYPE_EXTENDED_SUPPORTED_RATES:
			if (rates || ap_parse_supported_rates(&iter, &rates) <
					0)
				goto parse_error;

			break;
		}
	}

	*rsn_out = rsn;
	*rates_out = rates;

	return true;

parse_error:
	if (rsn)
		l_free(rsn);

	if (rates)
		l_uintset_free(rates);

	return false;
}

static void ap_handle_new_station(struct ap_state *ap, struct l_genl_msg *msg)
{
	struct sta_state *sta;
	struct l_genl_attr attr;
	uint16_t type;
	uint16_t len;
	const void *data;
	uint8_t mac[6];
	uint8_t *assoc_rsne = NULL;
	struct l_uintset *rates = NULL;

	if (!l_genl_attr_init(&attr, msg))
		return;

	while (l_genl_attr_next(&attr, &type, &len, &data)) {
		switch (type) {
		case NL80211_ATTR_IE:
			if (assoc_rsne || rates)
				goto cleanup;

			if (!ap_parse_new_station_ies(data, len, &assoc_rsne,
							&rates))
				return;
			break;
		case NL80211_ATTR_MAC:
			if (len != 6)
				goto cleanup;

			memcpy(mac, data, 6);
			break;
		}
	}

	if (!assoc_rsne || !rates)
		goto cleanup;

	/*
	 * Softmac's should already have a station created. The above check
	 * may also fail for softmac cards.
	 */
	sta = l_queue_find(ap->sta_states, ap_sta_match_addr, mac);
	if (sta)
		goto cleanup;

	sta = l_new(struct sta_state, 1);
	memcpy(sta->addr, mac, 6);
	sta->ap = ap;
	sta->assoc_rsne = assoc_rsne;
	sta->rates = rates;
	sta->aid = ++ap->last_aid;

	sta->associated = true;

	if (!ap->sta_states)
		ap->sta_states = l_queue_new();

	l_queue_push_tail(ap->sta_states, sta);

	msg = nl80211_build_set_station_unauthorized(
					netdev_get_ifindex(ap->netdev), mac);

	if (!l_genl_family_send(ap->nl80211, msg, ap_associate_sta_cb,
								sta, NULL)) {
		l_genl_msg_unref(msg);
		l_error("Issuing SET_STATION failed");
		ap_del_station(sta, MMPDU_REASON_CODE_UNSPECIFIED, true);
	}

	return;

cleanup:
	l_free(assoc_rsne);
	l_uintset_free(rates);
}

static void ap_handle_del_station(struct ap_state *ap, struct l_genl_msg *msg)
{
	struct sta_state *sta;
	struct l_genl_attr attr;
	uint16_t type;
	uint16_t len;
	const void *data;
	uint8_t mac[6];
	uint16_t reason = MMPDU_REASON_CODE_UNSPECIFIED;

	if (!l_genl_attr_init(&attr, msg))
		return;

	while (l_genl_attr_next(&attr, &type, &len, &data)) {
		switch (type) {
		case NL80211_ATTR_MAC:
			if (len != 6)
				return;

			memcpy(mac, data, 6);
			break;
		case NL80211_ATTR_REASON_CODE:
			if (len != 2)
				return;

			reason = l_get_u16(data);
		}
	}

	sta = l_queue_find(ap->sta_states, ap_sta_match_addr, mac);
	if (!sta)
		return;

	ap_del_station(sta, reason, true);
	ap_remove_sta(sta);
}

static void ap_mlme_notify(struct l_genl_msg *msg, void *user_data)
{
	struct ap_state *ap = user_data;
	uint32_t ifindex;

	if (nl80211_parse_attrs(msg, NL80211_ATTR_IFINDEX, &ifindex,
				NL80211_ATTR_UNSPEC) < 0 ||
			ifindex != netdev_get_ifindex(ap->netdev))
		return;

	switch (l_genl_msg_get_command(msg)) {
	case NL80211_CMD_STOP_AP:
		if (ap->start_stop_cmd_id) {
			l_genl_family_cancel(ap->nl80211,
						ap->start_stop_cmd_id);
			ap->start_stop_cmd_id = 0;
			ap->ops->handle_event(AP_EVENT_START_FAILED, NULL,
						ap->user_data);
		} else if (ap->started) {
			ap->started = false;
			ap->ops->handle_event(AP_EVENT_STOPPING, NULL,
						ap->user_data);
		}

		ap_reset(ap);
		l_genl_family_free(ap->nl80211);
		l_free(ap);
		break;
	case NL80211_CMD_NEW_STATION:
		ap_handle_new_station(ap, msg);
		break;
	case NL80211_CMD_DEL_STATION:
		ap_handle_del_station(ap, msg);
		break;
	}
}

static bool dhcp_load_settings(struct ap_state *ap, struct l_settings *settings)
{
	struct l_dhcp_server *server = ap->server;
	struct in_addr ia;

	L_AUTO_FREE_VAR(char *, netmask) = l_settings_get_string(settings,
							"IPv4", "Netmask");
	L_AUTO_FREE_VAR(char *, gateway) = l_settings_get_string(settings,
							"IPv4", "Gateway");
	char **dns = l_settings_get_string_list(settings, "IPv4",
							"DNSList", ',');
	char **ip_range = l_settings_get_string_list(settings, "IPv4",
							"IPRange", ',');
	unsigned int lease_time;
	bool ret = false;

	if (!l_settings_get_uint(settings, "IPv4", "LeaseTime", &lease_time))
		lease_time = 0;

	if (gateway && !l_dhcp_server_set_gateway(server, gateway)) {
		l_error("[IPv4].Gateway value error");
		goto error;
	}

	if (dns && !l_dhcp_server_set_dns(server, dns)) {
		l_error("[IPv4].DNSList value error");
		goto error;
	}

	if (netmask && !l_dhcp_server_set_netmask(server, netmask)) {
		l_error("[IPv4].Netmask value error");
		goto error;
	}

	if (ip_range) {
		if (l_strv_length(ip_range) != 2) {
			l_error("Two addresses expected in [IPv4].IPRange");
			goto error;
		}

		if (!l_dhcp_server_set_ip_range(server, ip_range[0],
							ip_range[1])) {
			l_error("Error setting IP range from [IPv4].IPRange");
			goto error;
		}
	}

	if (lease_time && !l_dhcp_server_set_lease_time(server, lease_time)) {
		l_error("[IPv4].LeaseTime value error");
		goto error;
	}

	if (netmask && inet_pton(AF_INET, netmask, &ia) > 0)
		ap->ip_prefix = __builtin_popcountl(ia.s_addr);
	else
		ap->ip_prefix = 24;

	ret = true;

error:
	l_strv_free(dns);
	l_strv_free(ip_range);
	return ret;
}

/*
 * This will determine the IP being used for DHCP. The IP will be automatically
 * set to ap->own_ip.
 *
 * The address to set (or keep) is determined in this order:
 * 1. Address defined in provisioning file
 * 2. Address already set on interface
 * 3. Address in IP pool.
 *
 * Returns:  0 if an IP was successfully selected and needs to be set
 *          -EALREADY if an IP was already set on the interface
 *          -EEXIST if the IP pool ran out of IP's
 *          -EINVAL if there was an error.
 */
static int ap_setup_dhcp(struct ap_state *ap, struct l_settings *settings)
{
	uint32_t ifindex = netdev_get_ifindex(ap->netdev);
	struct in_addr ia;
	uint32_t address = 0;
	int ret = -EINVAL;

	ap->server = l_dhcp_server_new(ifindex);
	if (!ap->server) {
		l_error("Failed to create DHCP server on %u", ifindex);
		return -EINVAL;;
	}

	if (getenv("IWD_DHCP_DEBUG"))
		l_dhcp_server_set_debug(ap->server, do_debug,
							"[DHCPv4 SERV] ", NULL);

	/* get the current address if there is one */
	if (l_net_get_address(ifindex, &ia) && ia.s_addr != 0)
		address = ia.s_addr;

	if (ap->config->profile) {
		char *addr;

		addr = l_settings_get_string(settings, "IPv4", "Address");
		if (addr) {
			if (inet_pton(AF_INET, addr, &ia) < 0)
				goto free_addr;

			/* Is a matching address already set on interface? */
			if (ia.s_addr == address)
				ret = -EALREADY;
			else
				ret = 0;
		} else if (address) {
			/* No address in config, but interface has one set */
			addr = l_strdup(inet_ntoa(ia));
			ret = -EALREADY;
		} else
			goto free_addr;

		/* Set the remaining DHCP options in config file */
		if (!dhcp_load_settings(ap, settings)) {
			ret = -EINVAL;
			goto free_addr;
		}

		if (!l_dhcp_server_set_ip_address(ap->server, addr)) {
			ret = -EINVAL;
			goto free_addr;
		}

		ap->own_ip = l_strdup(addr);

free_addr:
		l_free(addr);

		return ret;
	} else if (address) {
		/* No config file and address is already set */
		ap->own_ip = l_strdup(inet_ntoa(ia));

		return -EALREADY;
	} else if (pool.used) {
		/* No config file, no address set. Use IP pool */
		ap->own_ip = ip_pool_get();
		if (!ap->own_ip) {
			l_error("No more IP's in pool, cannot start AP on %u",
					ifindex);
			return -EEXIST;
		}

		ap->use_ip_pool = true;
		ap->ip_prefix = pool.prefix;

		return 0;
	}

	return -EINVAL;
}

static int ap_load_profile_and_dhcp(struct ap_state *ap, bool *wait_dhcp)
{
	uint32_t ifindex = netdev_get_ifindex(ap->netdev);
	char *passphrase;
	struct l_settings *settings = NULL;
	int err = -EINVAL;

	/* No profile or DHCP settings */
	if (!ap->config->profile && !pool.used)
		return 0;

	if (ap->config->profile) {
		settings = l_settings_new();

		if (!l_settings_load_from_file(settings, ap->config->profile))
			goto cleanup;

		passphrase = l_settings_get_string(settings, "Security",
							"Passphrase");
		if (passphrase) {
			if (strlen(passphrase) > 63) {
				l_error("[Security].Passphrase must not exceed "
						"63 characters");
				l_free(passphrase);
				goto cleanup;
			}

			strcpy(ap->config->passphrase, passphrase);
			l_free(passphrase);
		}

		if (!l_settings_has_group(settings, "IPv4")) {
			*wait_dhcp = false;
			err = 0;
			goto cleanup;
		}
	}

	err = ap_setup_dhcp(ap, settings);
	if (err == 0) {
		/* Address change required */
		ap->rtnl_add_cmd = l_rtnl_ifaddr4_add(rtnl, ifindex,
					ap->ip_prefix, ap->own_ip,
					broadcast_from_ip(ap->own_ip),
					ap_ifaddr4_added_cb, ap, NULL);

		if (!ap->rtnl_add_cmd) {
			l_error("Failed to add IPv4 address");
			err = -EIO;
			goto cleanup;
		}

		ap->cleanup_ip = true;

		*wait_dhcp = true;
		err = 0;
	/* Selected address already set, continue normally */
	} else if (err == -EALREADY) {
		*wait_dhcp = false;
		err = 0;
	}

cleanup:
	l_settings_free(settings);
	return err;
}

/*
 * Start a simple independent WPA2 AP on given netdev.
 *
 * @ops.handle_event is required and must react to AP_EVENT_START_FAILED
 * and AP_EVENT_STOPPING by forgetting the ap_state struct, which is
 * going to be freed automatically.
 * In the @config struct the .ssid field is required and one of
 * .passphrase and .psk must be filled in.  All other fields are optional.
 * If @ap_start succeeds, the returned ap_state takes ownership of
 * @config and the caller shouldn't free it or any of the memory pointed
 * to by its members (which also can't be static).
 */
struct ap_state *ap_start(struct netdev *netdev, struct ap_config *config,
				const struct ap_ops *ops, int *err_out,
				void *user_data)
{
	struct ap_state *ap;
	struct wiphy *wiphy = netdev_get_wiphy(netdev);
	struct l_genl_msg *cmd;
	uint64_t wdev_id = netdev_get_wdev_id(netdev);
	int err = -EINVAL;
	bool wait_on_address = false;

	if (err_out)
		*err_out = err;

	if (L_WARN_ON(!config->ssid))
		return NULL;

	if (L_WARN_ON(!config->profile && !config->passphrase[0] &&
			l_memeqzero(config->psk, sizeof(config->psk))))
		return NULL;

	ap = l_new(struct ap_state, 1);
	ap->nl80211 = l_genl_family_new(iwd_get_genl(), NL80211_GENL_NAME);
	ap->config = config;
	ap->netdev = netdev;
	ap->ops = ops;
	ap->user_data = user_data;

	/*
	 * This both loads a profile if required and loads DHCP settings either
	 * by the profile itself or the IP pool (or does nothing in the case
	 * of a profile-less configuration). wait_on_address will be set true
	 * if an address change is required.
	 */
	err = ap_load_profile_and_dhcp(ap, &wait_on_address);
	if (err < 0)
		goto error;

	if (!config->channel)
		/* TODO: Start a Get Survey to decide the channel */
		config->channel = 6;

	if (!config->wsc_name)
		config->wsc_name = l_strdup(config->ssid);

	if (!config->wsc_primary_device_type.category) {
		/* Make ourselves a WFA standard PC by default */
		config->wsc_primary_device_type.category = 1;
		memcpy(config->wsc_primary_device_type.oui, wsc_wfa_oui, 3);
		config->wsc_primary_device_type.oui_type = 0x04;
		config->wsc_primary_device_type.subcategory = 1;
	}

	/* TODO: Add all ciphers supported by wiphy */
	ap->ciphers = wiphy_select_cipher(wiphy, 0xffff);
	ap->group_cipher = wiphy_select_cipher(wiphy, 0xffff);
	ap->beacon_interval = 100;
	ap->rates = l_uintset_new(200);

	/* TODO: Pick from actual supported rates */
	if (config->no_cck_rates) {
		l_uintset_put(ap->rates, 12); /* 6 Mbps*/
		l_uintset_put(ap->rates, 18); /* 9 Mbps*/
		l_uintset_put(ap->rates, 24); /* 12 Mbps*/
		l_uintset_put(ap->rates, 36); /* 18 Mbps*/
		l_uintset_put(ap->rates, 48); /* 24 Mbps*/
		l_uintset_put(ap->rates, 72); /* 36 Mbps*/
		l_uintset_put(ap->rates, 96); /* 48 Mbps*/
		l_uintset_put(ap->rates, 108); /* 54 Mbps*/
	} else {
		l_uintset_put(ap->rates, 2); /* 1 Mbps*/
		l_uintset_put(ap->rates, 11); /* 5.5 Mbps*/
		l_uintset_put(ap->rates, 22); /* 11 Mbps*/
	}

	wsc_uuid_from_addr(netdev_get_address(netdev), ap->wsc_uuid_r);

	if (config->passphrase[0] &&
			crypto_psk_from_passphrase(config->passphrase,
						(uint8_t *) config->ssid,
						strlen(config->ssid),
						config->psk) < 0)
		goto error;

	if (!frame_watch_add(wdev_id, 0, 0x0000 |
			(MPDU_MANAGEMENT_SUBTYPE_ASSOCIATION_REQUEST << 4),
			NULL, 0, ap_assoc_req_cb, ap, NULL))
		goto error;

	if (!frame_watch_add(wdev_id, 0, 0x0000 |
			(MPDU_MANAGEMENT_SUBTYPE_REASSOCIATION_REQUEST << 4),
			NULL, 0, ap_reassoc_req_cb, ap, NULL))
		goto error;

	if (!frame_watch_add(wdev_id, 0, 0x0000 |
				(MPDU_MANAGEMENT_SUBTYPE_PROBE_REQUEST << 4),
				NULL, 0, ap_probe_req_cb, ap, NULL))
		goto error;

	if (!frame_watch_add(wdev_id, 0, 0x0000 |
				(MPDU_MANAGEMENT_SUBTYPE_DISASSOCIATION << 4),
				NULL, 0, ap_disassoc_cb, ap, NULL))
		goto error;

	if (!frame_watch_add(wdev_id, 0, 0x0000 |
				(MPDU_MANAGEMENT_SUBTYPE_AUTHENTICATION << 4),
				NULL, 0, ap_auth_cb, ap, NULL))
		goto error;

	if (!frame_watch_add(wdev_id, 0, 0x0000 |
				(MPDU_MANAGEMENT_SUBTYPE_DEAUTHENTICATION << 4),
				NULL, 0, ap_deauth_cb, ap, NULL))
		goto error;

	ap->mlme_watch = l_genl_family_register(ap->nl80211, "mlme",
						ap_mlme_notify, ap, NULL);
	if (!ap->mlme_watch)
		l_error("Registering for MLME notification failed");

	if (wait_on_address) {
		if (err_out)
			*err_out = 0;

		return ap;
	}

	cmd = ap_build_cmd_start_ap(ap);
	if (!cmd)
		goto error;

	ap->start_stop_cmd_id = l_genl_family_send(ap->nl80211, cmd,
							ap_start_cb, ap, NULL);
	if (!ap->start_stop_cmd_id) {
		l_genl_msg_unref(cmd);
		goto error;
	}

	if (err_out)
		*err_out = 0;

	return ap;

error:
	if (err_out)
		*err_out = err;

	ap->config = NULL;
	ap_reset(ap);
	l_genl_family_free(ap->nl80211);
	l_free(ap);
	return NULL;
}

static void ap_stop_cb(struct l_genl_msg *msg, void *user_data)
{
	struct ap_state *ap = user_data;
	int error = l_genl_msg_get_error(msg);

	ap->start_stop_cmd_id = 0;

	if (error < 0)
		l_error("STOP_AP failed: %s (%i)", strerror(-error), -error);

	if (ap->stopped_func)
		ap->stopped_func(ap->user_data);

	l_genl_family_free(ap->nl80211);
	l_free(ap);
}

static struct l_genl_msg *ap_build_cmd_stop_ap(struct ap_state *ap)
{
	struct l_genl_msg *cmd;
	uint32_t ifindex = netdev_get_ifindex(ap->netdev);

	cmd = l_genl_msg_new_sized(NL80211_CMD_STOP_AP, 16);
	l_genl_msg_append_attr(cmd, NL80211_ATTR_IFINDEX, 4, &ifindex);

	return cmd;
}

/*
 * Schedule the running @ap to be stopped and freed.  The original
 * ops and user_data are forgotten and a new callback can be
 * provided if the caller needs to know when the interface becomes
 * free, for example for a new ap_start call.
 *
 * The user must forget @ap when @stopped_func is called.  If the
 * @user_data ends up being destroyed before that, ap_free(ap) should
 * be used to prevent @stopped_func from being called.
 * If @stopped_func is not provided, the caller must forget @ap
 * immediately.
 */
void ap_shutdown(struct ap_state *ap, ap_stopped_func_t stopped_func,
			void *user_data)
{
	struct l_genl_msg *cmd;

	if (ap->started) {
		ap->started = false;
		ap->ops->handle_event(AP_EVENT_STOPPING, NULL, ap->user_data);
	}

	ap_reset(ap);

	if (ap->gtk_set) {
		ap->gtk_set = false;

		cmd = ap_build_cmd_del_key(ap);
		if (!cmd) {
			l_error("ap_build_cmd_del_key failed");
			goto free_ap;
		}

		if (!l_genl_family_send(ap->nl80211, cmd, ap_gtk_op_cb, NULL,
					NULL)) {
			l_genl_msg_unref(cmd);
			l_error("Issuing DEL_KEY failed");
			goto free_ap;
		}
	}

	cmd = ap_build_cmd_stop_ap(ap);
	if (!cmd) {
		l_error("ap_build_cmd_stop_ap failed");
		goto free_ap;
	}

	ap->start_stop_cmd_id = l_genl_family_send(ap->nl80211, cmd, ap_stop_cb,
							ap, NULL);
	if (!ap->start_stop_cmd_id) {
		l_genl_msg_unref(cmd);
		l_error("Sending STOP_AP failed");
		goto free_ap;
	}

	ap->stopped_func = stopped_func;
	ap->user_data = user_data;
	return;

free_ap:
	if (stopped_func)
		stopped_func(user_data);

	l_genl_family_free(ap->nl80211);
	l_free(ap);
}

/* Free @ap without a graceful shutdown */
void ap_free(struct ap_state *ap)
{
	ap_reset(ap);
	l_genl_family_free(ap->nl80211);
	if (ap->server)
		l_dhcp_server_destroy(ap->server);
	l_free(ap);
}

bool ap_station_disconnect(struct ap_state *ap, const uint8_t *mac,
				enum mmpdu_reason_code reason)
{
	struct sta_state *sta;

	if (!ap->started)
		return false;

	sta = l_queue_remove_if(ap->sta_states, ap_sta_match_addr, mac);
	if (!sta)
		return false;

	ap_del_station(sta, reason, false);
	ap_sta_free(sta);
	return true;
}

static void ap_wsc_pbc_timeout_cb(struct l_timeout *timeout, void *user_data)
{
	struct ap_state *ap = user_data;

	l_debug("PBC mode timeout");
	ap_wsc_exit_pbc(ap);
}

static void ap_wsc_pbc_timeout_destroy(void *user_data)
{
	struct ap_state *ap = user_data;

	ap->wsc_pbc_timeout = NULL;
}

bool ap_push_button(struct ap_state *ap)
{
	if (!ap->started)
		return false;

	if (l_queue_length(ap->wsc_pbc_probes) > 1) {
		l_debug("Can't start PBC mode due to Session Overlap");
		return false;
	}

	/*
	 * WSC v2.0.5 Section 11.3: "Multiple presses of the button are
	 * permitted.  If a PBC button on an Enrollee or Registrar is
	 * pressed again during Walk Time, the timers for that device are
	 * restarted at that time [...]"
	 */
	if (ap->wsc_pbc_timeout) {
		l_timeout_modify(ap->wsc_pbc_timeout, AP_WSC_PBC_WALK_TIME);
		return true;
	}

	ap->wsc_pbc_timeout = l_timeout_create(AP_WSC_PBC_WALK_TIME,
						ap_wsc_pbc_timeout_cb, ap,
						ap_wsc_pbc_timeout_destroy);
	ap->wsc_dpid = WSC_DEVICE_PASSWORD_ID_PUSH_BUTTON;
	ap_update_beacon(ap);
	return true;
}

struct ap_if_data {
	struct netdev *netdev;
	struct ap_state *ap;
	struct l_dbus_message *pending;
};

static void ap_if_event_func(enum ap_event_type type, const void *event_data,
				void *user_data)
{
	struct ap_if_data *ap_if = user_data;
	struct l_dbus_message *reply;

	switch (type) {
	case AP_EVENT_START_FAILED:
		if (L_WARN_ON(!ap_if->pending))
			break;

		reply = dbus_error_failed(ap_if->pending);
		dbus_pending_reply(&ap_if->pending, reply);
		ap_if->ap = NULL;
		break;

	case AP_EVENT_STARTED:
		if (L_WARN_ON(!ap_if->pending))
			break;

		l_dbus_object_add_interface(dbus_get_bus(),
						netdev_get_path(ap_if->netdev),
						IWD_AP_DIAGNOSTIC_INTERFACE,
						ap_if);

		reply = l_dbus_message_new_method_return(ap_if->pending);
		dbus_pending_reply(&ap_if->pending, reply);
		l_dbus_property_changed(dbus_get_bus(),
					netdev_get_path(ap_if->netdev),
					IWD_AP_INTERFACE, "Started");
		l_dbus_property_changed(dbus_get_bus(),
					netdev_get_path(ap_if->netdev),
					IWD_AP_INTERFACE, "Name");
		break;

	case AP_EVENT_STOPPING:
		l_dbus_object_remove_interface(dbus_get_bus(),
						netdev_get_path(ap_if->netdev),
						IWD_AP_DIAGNOSTIC_INTERFACE);

		l_dbus_property_changed(dbus_get_bus(),
					netdev_get_path(ap_if->netdev),
					IWD_AP_INTERFACE, "Started");
		l_dbus_property_changed(dbus_get_bus(),
					netdev_get_path(ap_if->netdev),
					IWD_AP_INTERFACE, "Name");

		if (!ap_if->pending)
			ap_if->ap = NULL;

		break;

	case AP_EVENT_STATION_ADDED:
	case AP_EVENT_STATION_REMOVED:
	case AP_EVENT_REGISTRATION_START:
	case AP_EVENT_REGISTRATION_SUCCESS:
	case AP_EVENT_PBC_MODE_EXIT:
		/* Ignored */
		break;
	}
}

static const struct ap_ops ap_dbus_ops = {
	.handle_event = ap_if_event_func,
};

static struct l_dbus_message *ap_dbus_start(struct l_dbus *dbus,
		struct l_dbus_message *message, void *user_data)
{
	struct ap_if_data *ap_if = user_data;
	const char *ssid, *wpa2_passphrase;
	struct ap_config *config;
	int err;

	if (ap_if->ap && ap_if->ap->started)
		return dbus_error_already_exists(message);

	if (ap_if->ap || ap_if->pending)
		return dbus_error_busy(message);

	if (!l_dbus_message_get_arguments(message, "ss",
						&ssid, &wpa2_passphrase))
		return dbus_error_invalid_args(message);

	config = l_new(struct ap_config, 1);
	config->ssid = l_strdup(ssid);
	l_strlcpy(config->passphrase, wpa2_passphrase,
			sizeof(config->passphrase));

	ap_if->ap = ap_start(ap_if->netdev, config, &ap_dbus_ops, &err, ap_if);
	if (!ap_if->ap) {
		ap_config_free(config);
		return dbus_error_from_errno(err, message);
	}

	ap_if->pending = l_dbus_message_ref(message);
	return NULL;
}

static void ap_dbus_stop_cb(void *user_data)
{
	struct ap_if_data *ap_if = user_data;
	struct l_dbus_message *reply;

	if (L_WARN_ON(!ap_if->pending))
		return;

	reply = l_dbus_message_new_method_return(ap_if->pending);
	dbus_pending_reply(&ap_if->pending, reply);
	ap_if->ap = NULL;
}

static struct l_dbus_message *ap_dbus_stop(struct l_dbus *dbus,
		struct l_dbus_message *message, void *user_data)
{
	struct ap_if_data *ap_if = user_data;

	if (!ap_if->ap) {
		if (ap_if->pending)
			return dbus_error_busy(message);

		/* already stopped, no-op */
		return l_dbus_message_new_method_return(message);
	}

	if (ap_if->pending) {
		struct l_dbus_message *reply;

		reply = dbus_error_aborted(ap_if->pending);
		dbus_pending_reply(&ap_if->pending, reply);
	}

	ap_if->pending = l_dbus_message_ref(message);
	ap_shutdown(ap_if->ap, ap_dbus_stop_cb, ap_if);
	return NULL;
}

static struct l_dbus_message *ap_dbus_start_profile(struct l_dbus *dbus,
						struct l_dbus_message *message,
						void *user_data)
{
	struct ap_if_data *ap_if = user_data;
	const char *ssid;
	struct ap_config *config;
	int err;

	if (ap_if->ap && ap_if->ap->started)
		return dbus_error_already_exists(message);

	if (ap_if->ap || ap_if->pending)
		return dbus_error_busy(message);

	if (!l_dbus_message_get_arguments(message, "s", &ssid))
		return dbus_error_invalid_args(message);

	config = l_new(struct ap_config, 1);
	config->ssid = l_strdup(ssid);
	/* This tells ap_start to pull settings from a profile on disk */
	config->profile = storage_get_path("ap/%s.ap", ssid);

	ap_if->ap = ap_start(ap_if->netdev, config, &ap_dbus_ops, &err, ap_if);
	if (!ap_if->ap) {
		ap_config_free(config);
		return dbus_error_from_errno(err, message);
	}

	ap_if->pending = l_dbus_message_ref(message);
	return NULL;
}

static bool ap_dbus_property_get_started(struct l_dbus *dbus,
					struct l_dbus_message *message,
					struct l_dbus_message_builder *builder,
					void *user_data)
{
	struct ap_if_data *ap_if = user_data;
	bool started = ap_if->ap && ap_if->ap->started;

	l_dbus_message_builder_append_basic(builder, 'b', &started);

	return true;
}

static bool ap_dbus_property_get_name(struct l_dbus *dbus,
					struct l_dbus_message *message,
					struct l_dbus_message_builder *builder,
					void *user_data)
{
	struct ap_if_data *ap_if = user_data;

	if (!ap_if->ap || !ap_if->ap->config || !ap_if->ap->started)
		return false;

	l_dbus_message_builder_append_basic(builder, 's',
						ap_if->ap->config->ssid);

	return true;
}

static void ap_setup_interface(struct l_dbus_interface *interface)
{
	l_dbus_interface_method(interface, "Start", 0, ap_dbus_start, "",
			"ss", "ssid", "wpa2_passphrase");
	l_dbus_interface_method(interface, "Stop", 0, ap_dbus_stop, "", "");
	l_dbus_interface_method(interface, "StartProfile", 0,
					ap_dbus_start_profile, "", "s",
					"ssid");

	l_dbus_interface_property(interface, "Started", 0, "b",
					ap_dbus_property_get_started, NULL);
	l_dbus_interface_property(interface, "Name", 0, "s",
					ap_dbus_property_get_name, NULL);
}

static void ap_destroy_interface(void *user_data)
{
	struct ap_if_data *ap_if = user_data;

	if (ap_if->pending) {
		struct l_dbus_message *reply;

		reply = dbus_error_aborted(ap_if->pending);
		dbus_pending_reply(&ap_if->pending, reply);
	}

	if (ap_if->ap)
		ap_free(ap_if->ap);

	l_free(ap_if);
}

struct diagnostic_data {
	struct l_dbus_message *pending;
	struct l_dbus_message_builder *builder;
};

static void ap_get_station_cb(const struct diagnostic_station_info *info,
				void *user_data)
{
	struct diagnostic_data *data = user_data;

	/* First station info */
	if (!data->builder) {
		struct l_dbus_message *reply =
				l_dbus_message_new_method_return(data->pending);

		data->builder = l_dbus_message_builder_new(reply);

		l_dbus_message_builder_enter_array(data->builder, "a{sv}");
	}

	l_dbus_message_builder_enter_array(data->builder, "{sv}");
	dbus_append_dict_basic(data->builder, "Address", 's',
					util_address_to_string(info->addr));

	diagnostic_info_to_dict(info, data->builder);

	l_dbus_message_builder_leave_array(data->builder);
}

static void ap_get_station_destroy(void *user_data)
{
	struct diagnostic_data *data = user_data;
	struct l_dbus_message *reply;

	if (!data->builder) {
		reply = l_dbus_message_new_method_return(data->pending);

		data->builder = l_dbus_message_builder_new(reply);

		l_dbus_message_builder_enter_array(data->builder, "a{sv}");
	}

	l_dbus_message_builder_leave_array(data->builder);
	reply = l_dbus_message_builder_finalize(data->builder);
	l_dbus_message_builder_destroy(data->builder);

	dbus_pending_reply(&data->pending, reply);

	l_free(data);
}

static struct l_dbus_message *ap_dbus_get_diagnostics(struct l_dbus *dbus,
		struct l_dbus_message *message, void *user_data)
{
	struct ap_if_data *ap_if = user_data;
	struct diagnostic_data *data;
	int ret;

	data = l_new(struct diagnostic_data, 1);
	data->pending = l_dbus_message_ref(message);

	ret = netdev_get_all_stations(ap_if->ap->netdev, ap_get_station_cb,
					data, ap_get_station_destroy);

	if (ret < 0) {
		l_dbus_message_unref(data->pending);
		l_free(data);
		return dbus_error_from_errno(ret, message);
	}

	return NULL;
}

static void ap_setup_diagnostic_interface(struct l_dbus_interface *interface)
{
	l_dbus_interface_method(interface, "GetDiagnostics", 0,
				ap_dbus_get_diagnostics,
				"aa{sv}", "", "diagnostic");
}

static void ap_diagnostic_interface_destroy(void *user_data)
{
}

static void ap_add_interface(struct netdev *netdev)
{
	struct ap_if_data *ap_if;

	/*
	 * TODO: Check wiphy supported channels and NL80211_ATTR_TX_FRAME_TYPES
	 */

	/* just allocate/set device, Start method will complete setup */
	ap_if = l_new(struct ap_if_data, 1);
	ap_if->netdev = netdev;

	/* setup ap dbus interface */
	l_dbus_object_add_interface(dbus_get_bus(),
			netdev_get_path(netdev), IWD_AP_INTERFACE, ap_if);
}

static void ap_remove_interface(struct netdev *netdev)
{
	l_dbus_object_remove_interface(dbus_get_bus(),
			netdev_get_path(netdev), IWD_AP_INTERFACE);
}

static void ap_netdev_watch(struct netdev *netdev,
				enum netdev_watch_event event, void *userdata)
{
	switch (event) {
	case NETDEV_WATCH_EVENT_UP:
	case NETDEV_WATCH_EVENT_NEW:
		if (netdev_get_iftype(netdev) == NETDEV_IFTYPE_AP &&
				netdev_get_is_up(netdev))
			ap_add_interface(netdev);
		break;
	case NETDEV_WATCH_EVENT_DOWN:
	case NETDEV_WATCH_EVENT_DEL:
		ap_remove_interface(netdev);
		break;
	default:
		break;
	}
}

static int ap_init(void)
{
	const struct l_settings *settings = iwd_get_config();
	bool dhcp_enable;

	netdev_watch = netdev_watch_add(ap_netdev_watch, NULL, NULL);

	l_dbus_register_interface(dbus_get_bus(), IWD_AP_INTERFACE,
			ap_setup_interface, ap_destroy_interface, false);
	l_dbus_register_interface(dbus_get_bus(), IWD_AP_DIAGNOSTIC_INTERFACE,
			ap_setup_diagnostic_interface,
			ap_diagnostic_interface_destroy, false);

	/*
	 * Reusing [General].EnableNetworkConfiguration as a switch to enable
	 * DHCP server. If no value is found or it is false do not create a
	 * DHCP server.
	 */
	if (!l_settings_get_bool(settings, "General",
				"EnableNetworkConfiguration", &dhcp_enable))
		dhcp_enable = false;

	if (dhcp_enable) {
		L_AUTO_FREE_VAR(char *, ip_prefix);

		ip_prefix = l_settings_get_string(settings, "General",
							"APRanges");
		/*
		 * In this case its assumed the user only cares about station
		 * netconfig so we let ap_init pass but DHCP will not be
		 * enabled.
		 */
		if (!ip_prefix) {
			l_warn("[General].APRanges must be set for DHCP");
			return 0;
		}

		if (!ip_pool_create(ip_prefix))
			return -EINVAL;
	}

	rtnl = iwd_get_rtnl();

	return 0;
}

static void ap_exit(void)
{
	netdev_watch_remove(netdev_watch);
	l_dbus_unregister_interface(dbus_get_bus(), IWD_AP_INTERFACE);

	ip_pool_destroy();
}

IWD_MODULE(ap, ap_init, ap_exit)
IWD_MODULE_DEPENDS(ap, netdev);
