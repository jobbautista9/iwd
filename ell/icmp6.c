/*
 *
 *  Embedded Linux library
 *
 *  Copyright (C) 2020  Intel Corporation. All rights reserved.
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

#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <netinet/ip6.h>
#include <netinet/icmp6.h>
#include <linux/ipv6.h>
#include <linux/rtnetlink.h>
#include <sys/types.h>
#include <sys/ioctl.h>
#include <net/if.h>
#include <unistd.h>
#include <errno.h>

#include "private.h"
#include "timeout.h"
#include "time.h"
#include "io.h"
#include "time-private.h"
#include "queue.h"
#include "net.h"
#include "netlink.h"
#include "rtnl.h"
#include "missing.h"
#include "icmp6.h"
#include "icmp6-private.h"

#define CLIENT_DEBUG(fmt, args...)					\
	l_util_debug(client->debug_handler, client->debug_data,		\
			"%s:%i " fmt, __func__, __LINE__, ## args)

#define IN6ADDR_LINKLOCAL_ALLNODES_INIT	\
			{ { { 0xff,2,0,0,0,0,0,0,0,0,0,0,0,0,0,1 } } }
#define IN6ADDR_LINKLOCAL_ALLROUTERS_INIT \
			{ { { 0xff,2,0,0,0,0,0,0,0,0,0,0,0,0,0,2 } } }

static const struct in6_addr in6addr_linklocal_allnodes_init =
					IN6ADDR_LINKLOCAL_ALLNODES_INIT;

static int add_mreq(int s, int ifindex, const struct in6_addr *mc_addr)
{
	struct ipv6_mreq mreq = {
		.ipv6mr_interface = ifindex,
		.ipv6mr_multiaddr = *mc_addr,
	};

	return setsockopt(s, IPPROTO_IPV6,
				IPV6_JOIN_GROUP, &mreq, sizeof(mreq));
}

static int icmp6_open_router_common(const struct icmp6_filter *filter,
					int ifindex)
{
	int s;
	int r;
	int yes = 1;
	int no = 0;
	int nhops = 255;

	s = socket(AF_INET6, SOCK_RAW | SOCK_CLOEXEC, IPPROTO_ICMPV6);
	if (s < 0)
		return -errno;

	r = setsockopt(s, IPPROTO_ICMPV6,
			ICMP6_FILTER, filter, sizeof(struct icmp6_filter));
	if (r < 0)
		goto fail;

	r = setsockopt(s, IPPROTO_IPV6, IPV6_MULTICAST_LOOP, &no, sizeof(no));
	if (r < 0)
		goto fail;

	r = setsockopt(s, IPPROTO_IPV6,
				IPV6_MULTICAST_IF, &ifindex, sizeof(ifindex));
	if (r < 0)
		goto fail;

	r = setsockopt(s, IPPROTO_IPV6,
				IPV6_RECVHOPLIMIT, &yes, sizeof(yes));
	if (r < 0)
		goto fail;

	r = setsockopt(s, IPPROTO_IPV6, IPV6_MULTICAST_HOPS,
							&nhops, sizeof(nhops));
	if (r < 0)
		goto fail;

	r = setsockopt(s, SOL_SOCKET, SO_BINDTOIFINDEX,
						&ifindex, sizeof(ifindex));
	if (r < 0 && errno == ENOPROTOOPT) {
		struct ifreq ifr = {
			.ifr_ifindex = ifindex,
		};

		r = ioctl(s, SIOCGIFNAME, &ifr);
		if (r < 0)
			goto fail;

		r = setsockopt(s, SOL_SOCKET, SO_BINDTODEVICE,
				ifr.ifr_name, strlen(ifr.ifr_name) + 1);
	}

	if (r < 0)
		goto fail;

	return s;

fail:
	close(s);
	return -errno;
}

static int icmp6_open_router_solicitation(int ifindex)
{
	struct icmp6_filter filter;
	int s;
	int r;

	ICMP6_FILTER_SETBLOCKALL(&filter);
	ICMP6_FILTER_SETPASS(ND_ROUTER_ADVERT, &filter);

	s = icmp6_open_router_common(&filter, ifindex);
	if (s < 0)
		return s;

	r = add_mreq(s, ifindex, &in6addr_linklocal_allnodes_init);
	if (r < 0) {
		close(s);
		return -errno;
	}

	return s;
}

static int icmp6_send_router_solicitation(int s, const uint8_t mac[static 6])
{
	struct sockaddr_in6 dst = {
		.sin6_family = AF_INET6,
		.sin6_addr = IN6ADDR_LINKLOCAL_ALLROUTERS_INIT,
	};
	struct nd_router_solicit rs = {
		.nd_rs_type = ND_ROUTER_SOLICIT,
	};
	struct nd_opt_hdr rs_opt = {
		.nd_opt_type = ND_OPT_SOURCE_LINKADDR,
		.nd_opt_len = 1,
	};
	struct iovec iov[3] = {
		{ .iov_base = &rs, .iov_len = sizeof(rs) },
		{ .iov_base = &rs_opt, .iov_len = sizeof(rs_opt) },
		{ .iov_base = (void *) mac, .iov_len = 6 } };

	struct msghdr msg = {
		.msg_name = &dst,
		.msg_namelen = sizeof(dst),
		.msg_iov = iov,
		.msg_iovlen = 3,
	};
	int r;

	r = sendmsg(s, &msg, 0);
	if (r < 0)
		return -errno;

	return 0;
}

static int icmp6_receive(int s, void *buf, size_t buf_len, struct in6_addr *src)
{
	char c_msg_buf[CMSG_SPACE(sizeof(int))];
	struct iovec iov = {
		.iov_base = buf,
		.iov_len = buf_len,
	};
	struct sockaddr_in6 saddr;
	struct msghdr msg = {
		.msg_name = (void *)&saddr,
		.msg_namelen = sizeof(struct sockaddr_in6),
		.msg_flags = 0,
		.msg_iov = &iov,
		.msg_iovlen = 1,
		.msg_control = c_msg_buf,
		.msg_controllen = sizeof(c_msg_buf),
	};
	struct cmsghdr *cmsg;
	ssize_t l;

	l = recvmsg(s, &msg, MSG_DONTWAIT);
	if (l < 0)
		return -errno;

	if ((size_t) l != buf_len)
		return -EINVAL;

	if (msg.msg_namelen != sizeof(struct sockaddr_in6) ||
			saddr.sin6_family != AF_INET6)
		return -EPFNOSUPPORT;

	for (cmsg = CMSG_FIRSTHDR(&msg); cmsg; cmsg = CMSG_NXTHDR(&msg, cmsg)) {
		if (cmsg->cmsg_level == SOL_IPV6 &&
				cmsg->cmsg_type == IPV6_HOPLIMIT &&
				cmsg->cmsg_len == CMSG_LEN(sizeof(int))) {
			int hops = l_get_u32(CMSG_DATA(cmsg));

			if (hops != 255)
				return -EMULTIHOP;
		}
	}

	memcpy(src, &saddr.sin6_addr, sizeof(saddr.sin6_addr));
	return 0;
}

struct l_icmp6_client {
	uint32_t ifindex;
	uint8_t mac[6];
	struct l_timeout *timeout_send;
	uint64_t retransmit_time;
	struct l_io *io;

	struct l_icmp6_router *ra;
	struct l_netlink *rtnl;
	uint32_t route_priority;
	struct l_queue *routes;

	l_icmp6_client_event_cb_t event_handler;
	void *event_data;
	l_icmp6_destroy_cb_t event_destroy;

	l_icmp6_debug_cb_t debug_handler;
	l_icmp6_destroy_cb_t debug_destroy;
	void *debug_data;

	bool nodelay : 1;
	bool have_mac : 1;
};

static inline void icmp6_client_event_notify(struct l_icmp6_client *client,
						enum l_icmp6_client_event event)
{
	if (client->event_handler)
		client->event_handler(client, event, client->event_data);
}

static bool icmp6_client_remove_route(void *data, void *user_data)
{
	struct l_icmp6_client *client = user_data;
	struct l_rtnl_route *r = data;

	if (client->rtnl)
		l_rtnl_route_delete(client->rtnl, client->ifindex, r,
				NULL, NULL, NULL);

	l_free(r);
	return true;
}

static void icmp6_client_setup_routes(struct l_icmp6_client *client)
{
	struct l_icmp6_router *ra = client->ra;
	struct l_rtnl_route *rt;
	char buf[INET6_ADDRSTRLEN];
	unsigned int i;

	rt = l_rtnl_route_new_gateway(inet_ntop(AF_INET6, ra->address,
							buf, sizeof(buf)));
	if (!rt) {
		CLIENT_DEBUG("Unable to parse RA 'from' address");
		return;
	}

	l_rtnl_route_set_preference(rt, ra->pref);
	l_rtnl_route_set_protocol(rt, RTPROT_RA);
	l_rtnl_route_set_mtu(rt, ra->mtu);
	l_rtnl_route_set_priority(rt, client->route_priority);
	l_queue_push_tail(client->routes, rt);

	if (client->rtnl)
		l_rtnl_route_add(client->rtnl, client->ifindex, rt,
					NULL, NULL, NULL);

	for (i = 0; i < ra->n_prefixes; i++) {
		struct route_info *info = &ra->prefixes[i];

		if (info->valid_lifetime == 0)
			continue;

		if (!inet_ntop(AF_INET6, info->address, buf, sizeof(buf)))
			continue;

		rt = l_rtnl_route_new_prefix(buf, info->prefix_len);
		if (!rt)
			continue;

		l_rtnl_route_set_preference(rt, info->preference);
		l_rtnl_route_set_protocol(rt, RTPROT_RA);
		l_rtnl_route_set_mtu(rt, ra->mtu);
		l_rtnl_route_set_priority(rt, client->route_priority);
		l_queue_push_tail(client->routes, rt);

		if (client->rtnl)
			l_rtnl_route_add(client->rtnl, client->ifindex, rt,
						NULL, NULL, NULL);
	}
}

static int icmp6_client_handle_message(struct l_icmp6_client *client,
						struct nd_router_advert *ra,
						size_t len,
						const struct in6_addr *src)
{
	struct l_icmp6_router *r = _icmp6_router_parse(ra, len, src->s6_addr);

	if (!r)
		return -EBADMSG;

	if (!client->ra) {
		client->ra = r;
		icmp6_client_event_notify(client,
					L_ICMP6_CLIENT_EVENT_ROUTER_FOUND);
		icmp6_client_setup_routes(client);
		return 0;
	}

	/*
	 * TODO: Figure out if the RA has updated info and update routes
	 * accordingly.
	 */
	_icmp6_router_free(client->ra);
	client->ra = r;
	return 0;
}

static bool icmp6_client_read_handler(struct l_io *io, void *userdata)
{
	struct l_icmp6_client *client = userdata;
	int s = l_io_get_fd(io);
	struct nd_router_advert *ra;
	ssize_t l;
	struct in6_addr src;

	/* Poke to see how many bytes we need to read / alloc */
	l = recv(s, NULL, 0, MSG_PEEK|MSG_TRUNC);
	if (l < 0) {
		CLIENT_DEBUG("Unable to read len info from socket: %s",
				strerror(-errno));
		return false;
	}

	ra = l_malloc(l);
	if (icmp6_receive(s, ra, l, &src) < 0)
		goto done;

	if ((size_t) l < sizeof(struct nd_router_advert)) {
		CLIENT_DEBUG("Message to small - ignore");
		goto done;
	}

	if (icmp6_client_handle_message(client, ra, l, &src) < 0)
		goto done;

	/* Stop solicitations */
	client->retransmit_time = 0;
	l_timeout_remove(client->timeout_send);
	client->timeout_send = NULL;

done:
	l_free(ra);
	return true;
}

static void icmp6_client_timeout_send(struct l_timeout *timeout,
								void *user_data)
{
	static const uint64_t MAX_SOLICITATION_INTERVAL = 3600 * L_MSEC_PER_SEC;
	static const uint64_t SOLICITATION_INTERVAL = 4 * L_MSEC_PER_SEC;
	struct l_icmp6_client *client = user_data;
	int r;

	CLIENT_DEBUG("");

	if (client->retransmit_time > MAX_SOLICITATION_INTERVAL / 2)
		client->retransmit_time =
				_time_fuzz_msecs(MAX_SOLICITATION_INTERVAL);
	else
		client->retransmit_time +=
			_time_fuzz_msecs(client->retransmit_time ?:
						SOLICITATION_INTERVAL);

	r = icmp6_send_router_solicitation(l_io_get_fd(client->io),
								client->mac);
	if (r < 0) {
		CLIENT_DEBUG("Error sending Router Solicitation: %s",
				strerror(-r));
		l_icmp6_client_stop(client);
		return;
	}

	CLIENT_DEBUG("Sent router solicitation, next attempt in %"PRIu64" ms",
			client->retransmit_time);
	l_timeout_modify_ms(timeout, client->retransmit_time);
}

LIB_EXPORT struct l_icmp6_client *l_icmp6_client_new(uint32_t ifindex)
{
	struct l_icmp6_client *client = l_new(struct l_icmp6_client, 1);

	client->ifindex = ifindex;
	client->routes = l_queue_new();

	return client;
}

LIB_EXPORT void l_icmp6_client_free(struct l_icmp6_client *client)
{
	if (unlikely(!client))
		return;

	l_icmp6_client_stop(client);
	l_queue_destroy(client->routes, NULL);
	l_free(client);
}

LIB_EXPORT bool l_icmp6_client_start(struct l_icmp6_client *client)
{
	uint64_t delay = 0;
	int s;

	if (unlikely(!client))
		return false;

	if (client->io)
		return false;

	CLIENT_DEBUG("Starting ICMPv6 Client");

	s = icmp6_open_router_solicitation(client->ifindex);
	if (s < 0)
		return false;

	if (!client->have_mac) {
		if (!l_net_get_mac_address(client->ifindex, client->mac)) {
			close(s);
			return false;
		}

		client->have_mac = true;
	}

	client->io = l_io_new(s);
	l_io_set_close_on_destroy(client->io, true);
	l_io_set_read_handler(client->io, icmp6_client_read_handler,
					client, NULL);

	if (!client->nodelay)
		delay = _time_pick_interval_secs(0, 1);

	client->timeout_send = l_timeout_create_ms(delay,
						icmp6_client_timeout_send,
						client, NULL);

	if (client->nodelay)
		icmp6_client_timeout_send(client->timeout_send, client);

	return true;
}

LIB_EXPORT bool l_icmp6_client_stop(struct l_icmp6_client *client)
{
	if (unlikely(!client))
		return false;

	if (!client->io)
		return false;

	CLIENT_DEBUG("Stopping...");

	l_io_destroy(client->io);
	client->io = NULL;

	l_queue_foreach_remove(client->routes,
					icmp6_client_remove_route, client);

	client->retransmit_time = 0;
	l_timeout_remove(client->timeout_send);
	client->timeout_send = NULL;

	if (client->ra) {
		_icmp6_router_free(client->ra);
		client->ra = NULL;
	}

	return true;
}

LIB_EXPORT const struct l_icmp6_router *l_icmp6_client_get_router(
						struct l_icmp6_client *client)
{
	if (unlikely(!client))
		return NULL;

	return client->ra;
}

LIB_EXPORT bool l_icmp6_client_set_event_handler(struct l_icmp6_client *client,
					l_icmp6_client_event_cb_t handler,
					void *userdata,
					l_icmp6_destroy_cb_t destroy)
{
	if (unlikely(!client))
		return false;

	if (client->event_destroy)
		client->event_destroy(client->event_data);

	client->event_handler = handler;
	client->event_data = userdata;
	client->event_destroy = destroy;

	return true;
}

LIB_EXPORT bool l_icmp6_client_set_debug(struct l_icmp6_client *client,
				l_icmp6_debug_cb_t function,
				void *user_data, l_icmp6_destroy_cb_t destroy)
{
	if (unlikely(!client))
		return false;

	if (client->debug_destroy)
		client->debug_destroy(client->debug_data);

	client->debug_handler = function;
	client->debug_destroy = destroy;
	client->debug_data = user_data;

	return true;
}

LIB_EXPORT bool l_icmp6_client_set_address(struct l_icmp6_client *client,
						const uint8_t addr[static 6])
{
	if (unlikely(!client))
		return false;

	if (client->io)
		return false;

	memcpy(client->mac, addr, 6);
	client->have_mac = true;

	return true;
}

LIB_EXPORT bool l_icmp6_client_set_nodelay(struct l_icmp6_client *client,
						bool nodelay)
{
	if (unlikely(!client))
		return false;

	client->nodelay = nodelay;

	return true;
}

LIB_EXPORT bool l_icmp6_client_set_rtnl(struct l_icmp6_client *client,
						struct l_netlink *rtnl)
{
	if (unlikely(!client))
		return false;

	client->rtnl = rtnl;
	return true;
}

LIB_EXPORT bool l_icmp6_client_set_route_priority(struct l_icmp6_client *client,
							uint32_t priority)
{
	if (unlikely(!client))
		return false;

	client->route_priority = priority;
	return true;
}

struct l_icmp6_router *_icmp6_router_new()
{
	struct l_icmp6_router *r = l_new(struct l_icmp6_router, 1);

	return r;
}

void _icmp6_router_free(struct l_icmp6_router *r)
{
	l_free(r->prefixes);
	l_free(r);
}

struct l_icmp6_router *_icmp6_router_parse(const struct nd_router_advert *ra,
						size_t len,
						const uint8_t src[static 16])
{
	struct l_icmp6_router *r;
	const uint8_t *opts;
	uint32_t opts_len;
	uint32_t n_prefixes = 0;

	if (ra->nd_ra_type != ND_ROUTER_ADVERT)
		return NULL;

	if (ra->nd_ra_code != 0)
		return NULL;

	opts = (uint8_t *) (ra + 1);
	opts_len = len - sizeof(struct nd_router_advert);

	while (opts_len) {
		uint8_t t;
		uint32_t l;

		if (opts_len < 2)
			return NULL;

		l = opts[1] * 8;
		if (!l || opts_len < l)
			return NULL;

		t = opts[0];

		switch (t) {
		case ND_OPT_MTU:
			if (l != 8)
				return NULL;
			break;
		case ND_OPT_PREFIX_INFORMATION:
			if (l != 32)
				return NULL;

			if (opts[2] > 128)
				return NULL;

			if (opts[3] & ND_OPT_PI_FLAG_ONLINK)
				n_prefixes += 1;
			break;
		}

		opts += l;
		opts_len -= l;
	}


	r = _icmp6_router_new();
	memcpy(r->address, src, sizeof(r->address));
	r->prefixes = l_new(struct route_info, n_prefixes);
	r->n_prefixes = n_prefixes;

	if (ra->nd_ra_flags_reserved & ND_RA_FLAG_MANAGED)
		r->managed = true;

	if (ra->nd_ra_flags_reserved & ND_RA_FLAG_OTHER)
		r->other = true;

	r->pref = (ra->nd_ra_flags_reserved >> 3) & 0x3;
	if (r->pref == 0x2) /* If invalid, reset to medium */
		r->pref = 0;

	r->lifetime = L_BE16_TO_CPU(ra->nd_ra_router_lifetime);

	opts = (uint8_t *) (ra + 1);
	opts_len = len - sizeof(struct nd_router_advert);
	n_prefixes = 0;

	while (opts_len) {
		uint8_t t = opts[0];
		uint32_t l = opts[1] * 8;

		switch (t) {
		case ND_OPT_MTU:
			if (r->mtu)
				break;

			r->mtu = l_get_be32(opts + 4);
			if (r->mtu < IPV6_MIN_MTU)
				r->mtu = 0;

			break;
		case ND_OPT_PREFIX_INFORMATION:
		{
			struct route_info *i = &r->prefixes[n_prefixes];

			if (!(opts[3] & ND_OPT_PI_FLAG_ONLINK))
				break;

			i->prefix_len = opts[2];
			i->valid_lifetime = l_get_be32(opts + 4);
			i->preferred_lifetime = l_get_be32(opts + 8);
			memcpy(i->address, opts + 16, 16);

			n_prefixes += 1;
			break;
		}
		}

		opts += l;
		opts_len -= l ;
	}

	return r;
}

LIB_EXPORT bool l_icmp6_router_get_managed(const struct l_icmp6_router *r)
{
	if (unlikely(!r))
		return false;

	return r->managed;
}

LIB_EXPORT bool l_icmp6_router_get_other(const struct l_icmp6_router *r)
{
	if (unlikely(!r))
		return false;

	return r->other;
}
