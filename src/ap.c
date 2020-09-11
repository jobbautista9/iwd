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

#include <ell/ell.h>

#include "linux/nl80211.h"

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

struct ap_state {
	struct netdev *netdev;
	struct l_genl_family *nl80211;
	ap_event_func_t event_func;
	ap_stopped_func_t stopped_func;
	void *user_data;
	struct ap_config *config;

	unsigned int ciphers;
	enum ie_rsn_cipher_suite group_cipher;
	uint32_t beacon_interval;
	struct l_uintset *rates;
	uint8_t pmk[32];
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

	bool started : 1;
	bool gtk_set : 1;
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

static uint32_t netdev_watch;

void ap_config_free(struct ap_config *config)
{
	if (unlikely(!config))
		return;

	l_free(config->ssid);

	if (config->psk) {
		explicit_bzero(config->psk, strlen(config->psk));
		l_free(config->psk);
	}

	l_free(config->authorized_macs);
	l_free(config->wsc_name);
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
	l_free(sta->assoc_rsne);

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

	explicit_bzero(ap->pmk, sizeof(ap->pmk));

	if (ap->mlme_watch)
		l_genl_family_unregister(ap->nl80211, ap->mlme_watch);

	frame_watch_wdev_remove(netdev_get_wdev_id(netdev));

	if (ap->start_stop_cmd_id)
		l_genl_family_cancel(ap->nl80211, ap->start_stop_cmd_id);

	l_queue_destroy(ap->sta_states, ap_sta_free);

	if (ap->rates)
		l_uintset_free(ap->rates);

	ap_config_free(ap->config);
	ap->config = NULL;

	l_queue_destroy(ap->wsc_pbc_probes, l_free);

	ap->started = false;
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
		if (ap->event_func) {
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
		ap->event_func(AP_EVENT_STATION_REMOVED, &event_data,
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

	if (ap->event_func) {
		struct ap_event_station_added_data event_data = {};
		event_data.mac = sta->addr;
		event_data.rsn_ie = sta->assoc_rsne;
		ap->event_func(AP_EVENT_STATION_ADDED, &event_data,
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

	if (ap->event_func) {
		struct ap_event_station_removed_data event_data = {};
		event_data.mac = sta->addr;
		ap->event_func(AP_EVENT_STATION_REMOVED, &event_data,
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

/*
 * Build a Beacon frame or a Probe Response frame's header and body until
 * the TIM IE.  Except for the optional TIM IE which is inserted by the
 * kernel when needed, our contents for both frames are the same.
 * See Beacon format in 8.3.3.2 and Probe Response format in 8.3.3.10.
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
		}

	ie_tlv_builder_set_length(&builder, rates -
					ie_tlv_builder_get_data(&builder));

	/* DSSS Parameter Set IE for DSSS, HR, ERP and HT PHY rates */
	ie_tlv_builder_next(&builder, IE_TYPE_DSSS_PARAMETER_SET);
	ie_tlv_builder_set_data(&builder, &ap->config->channel, 1);

	ie_tlv_builder_finalize(&builder, &len);
	return 36 + len;
}

/* Beacon / Probe Response frame portion after the TIM IE */
static size_t ap_build_beacon_pr_tail(struct ap_state *ap, bool pr,
					uint8_t *out_buf)
{
	size_t len;
	struct ie_rsn_info rsn;
	uint8_t *wsc_data;
	size_t wsc_data_size;
	uint8_t *wsc_ie;
	size_t wsc_ie_size;

	/* TODO: Country IE between TIM IE and RSNE */

	/* RSNE */
	ap_set_rsn_info(ap, &rsn);
	if (!ie_build_rsne(&rsn, out_buf))
		return 0;
	len = 2 + out_buf[1];

	/* WSC IE */
	if (pr) {
		struct wsc_probe_response wsc_pr = {};

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
	} else {
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
	}

	if (!wsc_data)
		return 0;

	wsc_ie = ie_tlv_encapsulate_wsc_payload(wsc_data, wsc_data_size,
						&wsc_ie_size);
	l_free(wsc_data);

	if (!wsc_ie)
		return 0;

	memcpy(out_buf + len, wsc_ie, wsc_ie_size);
	len += wsc_ie_size;
	l_free(wsc_ie);

	return len;
}

static void ap_set_beacon_cb(struct l_genl_msg *msg, void *user_data)
{
	int error = l_genl_msg_get_error(msg);

	if (error < 0)
		l_error("SET_BEACON failed: %s (%i)", strerror(-error), -error);
}

static void ap_update_beacon(struct ap_state *ap)
{
	struct l_genl_msg *cmd;
	uint8_t head[256], tail[256];
	size_t head_len, tail_len;
	uint64_t wdev_id = netdev_get_wdev_id(ap->netdev);
	static const uint8_t bcast_addr[6] = {
		0xff, 0xff, 0xff, 0xff, 0xff, 0xff
	};

	head_len = ap_build_beacon_pr_head(ap, MPDU_MANAGEMENT_SUBTYPE_BEACON,
						bcast_addr, head, sizeof(head));
	tail_len = ap_build_beacon_pr_tail(ap, false, tail);
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

static void ap_wsc_exit_pbc(struct ap_state *ap)
{
	if (!ap->wsc_pbc_timeout)
		return;

	l_timeout_remove(ap->wsc_pbc_timeout);
	ap->wsc_dpid = 0;
	ap_update_beacon(ap);

	ap->event_func(AP_EVENT_PBC_MODE_EXIT, NULL, ap->user_data);
}

static uint32_t ap_send_mgmt_frame(struct ap_state *ap,
					const struct mmpdu_header *frame,
					size_t frame_len, bool wait_ack,
					l_genl_msg_func_t callback,
					void *user_data)
{
	struct l_genl_msg *msg;
	uint32_t ifindex = netdev_get_ifindex(ap->netdev);
	uint32_t id;
	uint32_t ch_freq = scan_channel_to_freq(ap->config->channel,
						SCAN_BAND_2_4_GHZ);

	msg = l_genl_msg_new_sized(NL80211_CMD_FRAME, 128 + frame_len);

	l_genl_msg_append_attr(msg, NL80211_ATTR_IFINDEX, 4, &ifindex);
	l_genl_msg_append_attr(msg, NL80211_ATTR_WIPHY_FREQ, 4, &ch_freq);
	l_genl_msg_append_attr(msg, NL80211_ATTR_FRAME, frame_len, frame);
	if (!wait_ack)
		l_genl_msg_append_attr(msg, NL80211_ATTR_DONT_WAIT_FOR_ACK,
					0, NULL);

	if (ap->config->no_cck_rates)
		l_genl_msg_append_attr(msg, NL80211_ATTR_TX_NO_CCK_RATE, 0,
					NULL);


	id = l_genl_family_send(ap->nl80211, msg, callback, user_data, NULL);

	if (!id)
		l_genl_msg_unref(msg);

	return id;
}

static void ap_start_handshake(struct sta_state *sta, bool use_eapol_start)
{
	struct ap_state *ap = sta->ap;
	const uint8_t *own_addr = netdev_get_address(ap->netdev);
	struct ie_rsn_info rsn;
	uint8_t bss_rsne[24];

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
	handshake_state_set_pmk(sta->hs, sta->ap->pmk, 32);

	if (gtk_rsc)
		handshake_state_set_gtk(sta->hs, sta->ap->gtk,
					sta->ap->gtk_index, gtk_rsc);

	ap_start_handshake(sta, false);
}

static void ap_gtk_query_cb(struct l_genl_msg *msg, void *user_data)
{
	struct sta_state *sta = user_data;
	const void *gtk_rsc;

	sta->gtk_query_cmd_id = 0;

	gtk_rsc = nl80211_parse_get_key_seq(msg);
	if (!gtk_rsc)
		goto error;

	ap_start_rsna(sta, gtk_rsc);
	return;

error:
	ap_del_station(sta, MMPDU_REASON_CODE_UNSPECIFIED, true);
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
		sta->ap->event_func(AP_EVENT_REGISTRATION_SUCCESS, &event_data,
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

	L_AUTO_FREE_VAR(char *, uuid_r_str) = NULL;
	L_AUTO_FREE_VAR(char *, uuid_e_str) = NULL;

	uuid_r_str = l_util_hexstring(ap->wsc_uuid_r, 16);
	uuid_e_str = l_util_hexstring(sta->wsc_uuid_e, 16);

	sta->wsc_settings = l_settings_new();
	l_settings_set_string(sta->wsc_settings, "Security", "EAP-Method",
				"WSC-R");
	l_settings_set_string(sta->wsc_settings, "WSC", "EnrolleeMAC",
				util_address_to_string(sta->addr));
	l_settings_set_string(sta->wsc_settings, "WSC", "UUID-R",
				uuid_r_str);
	l_settings_set_string(sta->wsc_settings, "WSC", "UUID-E",
				uuid_e_str);
	l_settings_set_uint(sta->wsc_settings, "WSC", "RFBand",
				WSC_RF_BAND_2_4_GHZ);
	l_settings_set_uint(sta->wsc_settings, "WSC", "ConfigurationMethods",
				WSC_CONFIGURATION_METHOD_PUSH_BUTTON);
	l_settings_set_string(sta->wsc_settings, "WSC", "WPA2-SSID",
				ap->config->ssid);
	l_settings_set_string(sta->wsc_settings, "WSC", "WPA2-Passphrase",
				ap->config->psk);

	sta->hs = netdev_handshake_state_new(ap->netdev);
	handshake_state_set_authenticator(sta->hs, true);
	handshake_state_set_event_func(sta->hs, ap_wsc_handshake_event, sta);
	handshake_state_set_8021x_config(sta->hs, sta->wsc_settings);

	ap_start_handshake(sta, wait_for_eapol_start);
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

static void ap_success_assoc_resp_cb(struct l_genl_msg *msg, void *user_data)
{
	struct sta_state *sta = user_data;
	struct ap_state *ap = sta->ap;

	sta->assoc_resp_cmd_id = 0;

	if (l_genl_msg_get_error(msg) < 0) {
		l_error("AP (Re)Association Response not sent or not ACKed: %i",
			l_genl_msg_get_error(msg));

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

static void ap_fail_assoc_resp_cb(struct l_genl_msg *msg, void *user_data)
{
	if (l_genl_msg_get_error(msg) < 0)
		l_error("AP (Re)Association Response with an error status not "
			"sent or not ACKed: %i", l_genl_msg_get_error(msg));
	else
		l_info("AP (Re)Association Response with an error status "
			"delivered OK");
}

static uint32_t ap_assoc_resp(struct ap_state *ap, struct sta_state *sta,
				const uint8_t *dest,
				enum mmpdu_reason_code status_code,
				bool reassoc, l_genl_msg_func_t callback)
{
	const uint8_t *addr = netdev_get_address(ap->netdev);
	uint8_t mpdu_buf[128];
	struct mmpdu_header *mpdu = (void *) mpdu_buf;
	struct mmpdu_association_response *resp;
	size_t ies_len = 0;
	uint16_t capability = IE_BSS_CAP_ESS | IE_BSS_CAP_PRIVACY;
	uint32_t r, minr, maxr, count;

	memset(mpdu, 0, sizeof(*mpdu));

	/* Header */
	mpdu->fc.protocol_version = 0;
	mpdu->fc.type = MPDU_TYPE_MANAGEMENT;
	mpdu->fc.subtype = reassoc ?
		MPDU_MANAGEMENT_SUBTYPE_REASSOCIATION_RESPONSE :
		MPDU_MANAGEMENT_SUBTYPE_ASSOCIATION_RESPONSE;
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

	if (sta && !sta->assoc_rsne) {
		struct wsc_association_response wsc_resp = {};
		uint8_t *wsc_data;
		size_t wsc_data_len;
		uint8_t *wsc_ie;
		size_t wsc_ie_len;

		wsc_resp.response_type = WSC_RESPONSE_TYPE_AP;
		wsc_resp.version2 = sta->wsc_v2;

		wsc_data = wsc_build_association_response(&wsc_resp,
								&wsc_data_len);
		if (!wsc_data) {
			l_error("wsc_build_beacon error");
			goto send_frame;
		}

		wsc_ie = ie_tlv_encapsulate_wsc_payload(wsc_data, wsc_data_len,
							&wsc_ie_len);
		l_free(wsc_data);

		if (!wsc_ie) {
			l_error("ie_tlv_encapsulate_wsc_payload error");
			goto send_frame;
		}

		memcpy(resp->ies + ies_len, wsc_ie, wsc_ie_len);
		ies_len += wsc_ie_len;
		l_free(wsc_ie);
	}

send_frame:
	return ap_send_mgmt_frame(ap, mpdu, resp->ies + ies_len - mpdu_buf,
					true, callback, sta);
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
				uint16_t listen_interval, const uint8_t *ies,
				size_t ies_len)
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
		ap->event_func(AP_EVENT_REGISTRATION_START, &event_data,
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

		if (!(rsn_info.pairwise_ciphers & ap->ciphers)) {
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

	if (sta->assoc_rsne)
		l_free(sta->assoc_rsne);

	if (rsn)
		sta->assoc_rsne = l_memdup(rsn, rsn[1] + 2);
	else
		sta->assoc_rsne = NULL;

	sta->assoc_resp_cmd_id = ap_assoc_resp(ap, sta, sta->addr, 0, reassoc,
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
				false, ap_fail_assoc_resp_cb))
			l_error("Sending error Association Response failed");

		return;
	}

	ap_assoc_reassoc(sta, false, &req->capability,
				L_LE16_TO_CPU(req->listen_interval),
				req->ies, body_len - sizeof(*req));
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
				req->ies, body_len - sizeof(*req));
	return;

bad_frame:
	if (!ap_assoc_resp(ap, NULL, from, err, true, ap_fail_assoc_resp_cb))
		l_error("Sending error Reassociation Response failed");
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

static void ap_probe_resp_cb(struct l_genl_msg *msg, void *user_data)
{
	if (l_genl_msg_get_error(msg) < 0)
		l_error("AP Probe Response not sent: %i",
			l_genl_msg_get_error(msg));
	else
		l_info("AP Probe Response sent OK");
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
	uint8_t resp[512];
	uint8_t *wsc_data;
	ssize_t wsc_data_len;

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

	/*
	 * Process the WSC IE first as it may cause us to exit "active PBC
	 * mode" and that can be immediately reflected in our Probe Response.
	 */
	wsc_data = ie_tlv_extract_wsc_payload(req->ies, body_len - sizeof(*req),
						&wsc_data_len);
	if (wsc_data) {
		ap_process_wsc_probe_req(ap, hdr->address_2,
						wsc_data, wsc_data_len);
		l_free(wsc_data);
	}

	len = ap_build_beacon_pr_head(ap,
					MPDU_MANAGEMENT_SUBTYPE_PROBE_RESPONSE,
					hdr->address_2, resp, sizeof(resp));
	len += ap_build_beacon_pr_tail(ap, true, resp + len);

	ap_send_mgmt_frame(ap, (struct mmpdu_header *) resp, len, false,
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

static void ap_auth_reply_cb(struct l_genl_msg *msg, void *user_data)
{
	if (l_genl_msg_get_error(msg) < 0)
		l_error("AP Authentication frame 2 not sent or not ACKed: %i",
			l_genl_msg_get_error(msg));
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

	ap_send_mgmt_frame(ap, mpdu, (uint8_t *) auth + 6 - mpdu_buf, true,
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

static void ap_start_cb(struct l_genl_msg *msg, void *user_data)
{
	struct ap_state *ap = user_data;

	ap->start_stop_cmd_id = 0;

	if (l_genl_msg_get_error(msg) < 0) {
		l_error("START_AP failed: %i", l_genl_msg_get_error(msg));

		ap->event_func(AP_EVENT_START_FAILED, NULL, ap->user_data);
		ap_reset(ap);
		l_genl_family_free(ap->nl80211);
		l_free(ap);
		return;
	}

	ap->started = true;
	ap->event_func(AP_EVENT_STARTED, NULL, ap->user_data);
}

static struct l_genl_msg *ap_build_cmd_start_ap(struct ap_state *ap)
{
	struct l_genl_msg *cmd;

	uint8_t head[256], tail[256];
	size_t head_len, tail_len;

	uint32_t dtim_period = 3;
	uint32_t ifindex = netdev_get_ifindex(ap->netdev);
	struct wiphy *wiphy = netdev_get_wiphy(ap->netdev);
	uint32_t hidden_ssid = NL80211_HIDDEN_SSID_NOT_IN_USE;
	uint32_t nl_ciphers = ie_rsn_cipher_suite_to_cipher(ap->ciphers);
	uint32_t nl_akm = CRYPTO_AKM_PSK;
	uint32_t wpa_version = NL80211_WPA_VERSION_2;
	uint32_t auth_type = NL80211_AUTHTYPE_OPEN_SYSTEM;
	uint32_t ch_freq = scan_channel_to_freq(ap->config->channel,
						SCAN_BAND_2_4_GHZ);
	uint32_t ch_width = NL80211_CHAN_WIDTH_20;

	static const uint8_t bcast_addr[6] = {
		0xff, 0xff, 0xff, 0xff, 0xff, 0xff
	};

	head_len = ap_build_beacon_pr_head(ap, MPDU_MANAGEMENT_SUBTYPE_BEACON,
						bcast_addr, head, sizeof(head));
	tail_len = ap_build_beacon_pr_tail(ap, false, tail);

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
	l_genl_msg_append_attr(cmd, NL80211_ATTR_CIPHER_SUITES_PAIRWISE, 4,
				&nl_ciphers);
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
			ap->event_func(AP_EVENT_START_FAILED, NULL,
					ap->user_data);
		} else if (ap->started) {
			ap->started = false;
			ap->event_func(AP_EVENT_STOPPING, NULL, ap->user_data);
		}

		ap_reset(ap);
		l_genl_family_free(ap->nl80211);
		l_free(ap);
		break;
	}
}

/*
 * Start a simple independent WPA2 AP on given netdev.
 *
 * @event_func is required and must react to AP_EVENT_START_FAILED
 * and AP_EVENT_STOPPING by forgetting the ap_state struct, which
 * is going to be freed automatically.
 * In the @config struct only .ssid and .psk need to be non-NUL,
 * other fields are optional.  If @ap_start succeeds, the returned
 * ap_state takes ownership of @config and the caller shouldn't
 * free it or any of the memory pointed to by its members (they
 * also can't be static).
 */
struct ap_state *ap_start(struct netdev *netdev, struct ap_config *config,
				ap_event_func_t event_func, void *user_data)
{
	struct ap_state *ap;
	struct wiphy *wiphy = netdev_get_wiphy(netdev);
	struct l_genl_msg *cmd;
	uint64_t wdev_id = netdev_get_wdev_id(netdev);

	ap = l_new(struct ap_state, 1);
	ap->nl80211 = l_genl_family_new(iwd_get_genl(), NL80211_GENL_NAME);
	ap->config = config;
	ap->netdev = netdev;
	ap->event_func = event_func;
	ap->user_data = user_data;

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
		ap->rates = l_uintset_new(200);
		l_uintset_put(ap->rates, 2); /* 1 Mbps*/
		l_uintset_put(ap->rates, 11); /* 5.5 Mbps*/
		l_uintset_put(ap->rates, 22); /* 11 Mbps*/
	}

	wsc_uuid_from_addr(netdev_get_address(netdev), ap->wsc_uuid_r);

	if (crypto_psk_from_passphrase(config->psk, (uint8_t *) config->ssid,
					strlen(config->ssid), ap->pmk) < 0)
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

	cmd = ap_build_cmd_start_ap(ap);
	if (!cmd)
		goto error;

	ap->start_stop_cmd_id = l_genl_family_send(ap->nl80211, cmd,
							ap_start_cb, ap, NULL);
	if (!ap->start_stop_cmd_id) {
		l_genl_msg_unref(cmd);
		goto error;
	}

	return ap;

error:
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
		l_error("STOP_AP failed: %s (%i)", strerror(error), error);

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
 * event_func and user_data are forgotten and a new callback can be
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
		ap->event_func(AP_EVENT_STOPPING, NULL, ap->user_data);
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

		reply = l_dbus_message_new_method_return(ap_if->pending);
		dbus_pending_reply(&ap_if->pending, reply);
		l_dbus_property_changed(dbus_get_bus(),
					netdev_get_path(ap_if->netdev),
					IWD_AP_INTERFACE, "Started");
		break;

	case AP_EVENT_STOPPING:
		l_dbus_property_changed(dbus_get_bus(),
					netdev_get_path(ap_if->netdev),
					IWD_AP_INTERFACE, "Started");

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

static struct l_dbus_message *ap_dbus_start(struct l_dbus *dbus,
		struct l_dbus_message *message, void *user_data)
{
	struct ap_if_data *ap_if = user_data;
	const char *ssid, *wpa2_psk;
	struct ap_config *config;

	if (ap_if->ap && ap_if->ap->started)
		return dbus_error_already_exists(message);

	if (ap_if->ap || ap_if->pending)
		return dbus_error_busy(message);

	if (!l_dbus_message_get_arguments(message, "ss", &ssid, &wpa2_psk))
		return dbus_error_invalid_args(message);

	config = l_new(struct ap_config, 1);
	config->ssid = l_strdup(ssid);
	config->psk = l_strdup(wpa2_psk);

	ap_if->ap = ap_start(ap_if->netdev, config, ap_if_event_func, ap_if);
	if (!ap_if->ap) {
		ap_config_free(config);
		return dbus_error_invalid_args(message);
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

static void ap_setup_interface(struct l_dbus_interface *interface)
{
	l_dbus_interface_method(interface, "Start", 0, ap_dbus_start, "",
			"ss", "ssid", "wpa2_psk");
	l_dbus_interface_method(interface, "Stop", 0, ap_dbus_stop, "", "");

	l_dbus_interface_property(interface, "Started", 0, "b",
					ap_dbus_property_get_started, NULL);
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
	netdev_watch = netdev_watch_add(ap_netdev_watch, NULL, NULL);

	l_dbus_register_interface(dbus_get_bus(), IWD_AP_INTERFACE,
			ap_setup_interface, ap_destroy_interface, false);

	return 0;
}

static void ap_exit(void)
{
	netdev_watch_remove(netdev_watch);
	l_dbus_unregister_interface(dbus_get_bus(), IWD_AP_INTERFACE);
}

IWD_MODULE(ap, ap_init, ap_exit)
IWD_MODULE_DEPENDS(ap, netdev);
