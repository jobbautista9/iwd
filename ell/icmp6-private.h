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

struct route_info {
	uint8_t address[16];
	uint8_t prefix_len;
	uint8_t preference;
	uint32_t preferred_lifetime;
	uint32_t valid_lifetime;
};

struct l_icmp6_router {
	uint8_t address[16];
	bool managed : 1;
	bool other : 1;
	uint8_t pref;
	uint16_t lifetime;
	uint32_t mtu;
	uint32_t n_prefixes;
	struct route_info *prefixes;
};

struct l_icmp6_router *_icmp6_router_new();
void _icmp6_router_free(struct l_icmp6_router *r);
struct l_icmp6_router *_icmp6_router_parse(const struct nd_router_advert *ra,
						size_t len,
						const uint8_t src[static 16]);
