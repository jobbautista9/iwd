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

#define uninitialized_var(x) x = x

struct l_genl;
struct l_genl_family;

const struct l_settings *iwd_get_config(void);
struct l_genl *iwd_get_genl(void);

void netdev_shutdown(void);

const char *iwd_get_iface_whitelist(void);
const char *iwd_get_iface_blacklist(void);

const char *iwd_get_phy_whitelist(void);
const char *iwd_get_phy_blacklist(void);

struct iwd_module_desc {
	const char *name;
	int (*init)(void);
	void (*exit)(void);
	bool active;
} __attribute__((aligned(8)));

struct iwd_module_depends {
	const char *self;
	const char *target;
};

#define IWD_MODULE(name, init, exit)					\
	static struct iwd_module_desc __iwd_module_ ## name		\
		__attribute__((used, section("__iwd_module"), aligned(8))) = {\
			#name, init, exit				\
		};

#define IWD_MODULE_DEPENDS(name, dep)					\
	static struct iwd_module_depends				\
				__iwd_module__##name_##dep		\
		__attribute__((used, section("__iwd_module_dep"),       \
					aligned(8))) = {		\
			.self = #name,					\
			.target = #dep,					\
		};

int iwd_modules_init();
void iwd_modules_exit();
