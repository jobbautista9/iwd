/*
 *
 *  Ethernet daemon for Linux
 *
 *  Copyright (C) 2017-2018  Intel Corporation. All rights reserved.
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
#include <dirent.h>
#include <errno.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <ell/ell.h>

#include "wired/network.h"

#define STORAGEFILE_SUFFIX	".8021x"
#define STORAGEFILE_SUFFIX_LEN	6

struct network {
	char *name;
};

static struct l_queue *network_list;
static struct l_fswatch *storage_watch;

static char *network_name_from_filename(const char *filename)
{
	if (!l_str_has_suffix(filename, STORAGEFILE_SUFFIX))
		return NULL;

	return l_strndup(filename, strlen(filename) - STORAGEFILE_SUFFIX_LEN);
}

static struct network *network_new(const char *name)
{
	struct network *net;

	l_debug("Creating network '%s'", name);

	net = l_new(struct network, 1);
	net->name = l_strdup(name);

	return net;
}

static void network_free(void *data)
{
	struct network *net = data;

	l_debug("Freeing network '%s'", net->name);

	l_free(net->name);
	l_free(net);
}

static bool network_match(const void *a, const void *b)
{
	const struct network *net = a;
	const char *name = b;

	return strcmp(net->name, name);
}

static struct network *network_lookup(const char *name)
{
	return l_queue_find(network_list, network_match, name);
}

struct l_settings *network_lookup_security(const char *network)
{
	struct l_settings *conf;
	char *path;

	path = l_strdup_printf("%s/%s%s", WIRED_STORAGEDIR, network,
							STORAGEFILE_SUFFIX);

	l_debug("Loading %s", path);

	conf = l_settings_new();
	l_settings_load_from_file(conf, path);

	l_free(path);

	return conf;
}

static void network_storage_watch_cb(struct l_fswatch *watch,
					const char *filename,
					enum l_fswatch_event event,
					void *user_data)
{
	struct network *net;
	char *name;

	/*
	 * Ignore notifications for the actual directory, we can't do
	 * anything about some of them anyway.  Only react to
	 * notifications for files in the storage directory.
	 */
	if (!filename)
		return;

	name = network_name_from_filename(filename);
	if (!name)
		return;

	switch (event) {
	case L_FSWATCH_EVENT_DELETE:
	case L_FSWATCH_EVENT_MOVE:
	case L_FSWATCH_EVENT_MODIFY:
	case L_FSWATCH_EVENT_ATTRIB:
	case L_FSWATCH_EVENT_CREATE:
		/*
		 * For now treat all the operations the same.  E.g. they may
		 * result in the removal of the network (file moved out, not
		 * readable or invalid) or the creation of a new network (file
		 * created, permissions granted, syntax fixed, etc.)
		 * so we always need to re-read the file.
		 */
		net = network_lookup(name);
		if (!net) {
			net = network_new(name);
			l_queue_push_tail(network_list, net);
		} else {
			l_free(net->name);
			net->name = l_strdup(name);
		}
	}

	l_free(name);
}

static void network_storage_watch_destroy(void *user_data)
{
	storage_watch = NULL;
}

bool network_init(void)
{
	DIR *dir;
	struct dirent *dirent;

	dir = opendir(WIRED_STORAGEDIR);
	if (!dir) {
		l_info("Unable to open %s: %s", WIRED_STORAGEDIR,
							strerror(errno));
		return false;
	}

	network_list = l_queue_new();

	while ((dirent = readdir(dir))) {
		struct network *net;
		char *name;

		if (dirent->d_type != DT_REG && dirent->d_type != DT_LNK)
			continue;

		name = network_name_from_filename(dirent->d_name);
		if (!name)
			continue;

		net = network_new(name);
		l_queue_push_tail(network_list, net);

		l_free(name);
	}

	closedir(dir);

	storage_watch = l_fswatch_new(WIRED_STORAGEDIR,
					network_storage_watch_cb, NULL,
					network_storage_watch_destroy);

	return true;
}

void network_exit(void)
{
	l_fswatch_destroy(storage_watch);

	l_queue_destroy(network_list, network_free);
	network_list = NULL;
}
