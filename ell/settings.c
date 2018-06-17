/*
 *
 *  Embedded Linux library
 *
 *  Copyright (C) 2011-2014  Intel Corporation. All rights reserved.
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

#include <stdio.h>
#include <stdlib.h>
#include <errno.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/mman.h>

#include "util.h"
#include "string.h"
#include "queue.h"
#include "settings.h"
#include "private.h"

struct setting_data {
	char *key;
	char *value;
};

struct group_data {
	char *name;
	struct l_queue *settings;
};

struct l_settings {
	l_settings_debug_cb_t debug_handler;
	l_settings_destroy_cb_t debug_destroy;
	void *debug_data;
	struct l_queue *groups;
};

static void setting_destroy(void *data)
{
	struct setting_data *pair = data;

	l_free(pair->key);
	memset(pair->value, 0, strlen(pair->value));
	l_free(pair->value);
	l_free(pair);
}

static void group_destroy(void *data)
{
	struct group_data *group = data;

	l_free(group->name);
	l_queue_destroy(group->settings, setting_destroy);

	l_free(group);
}

LIB_EXPORT struct l_settings *l_settings_new(void)
{
	struct l_settings *settings;

	settings = l_new(struct l_settings, 1);
	settings->groups = l_queue_new();

	return settings;
}

LIB_EXPORT void l_settings_free(struct l_settings *settings)
{
	if (unlikely(!settings))
		return;

	if (settings->debug_destroy)
		settings->debug_destroy(settings->debug_data);

	l_queue_destroy(settings->groups, group_destroy);

	l_free(settings);
}

static char *unescape_value(const char *value)
{
	char *ret;
	char *n;
	const char *o;

	ret = l_new(char, strlen(value) + 1);

	for (n = ret, o = value; *o; o++, n++) {
		if (*o != '\\') {
			*n = *o;
			continue;
		}

		o += 1;

		switch (*o) {
		case 's':
			*n = ' ';
			break;
		case 'n':
			*n = '\n';
			break;
		case 't':
			*n = '\t';
			break;
		case 'r':
			*n = '\r';
			break;
		case '\\':
			*n = '\\';
			break;
		default:
			l_free(ret);
			return NULL;
		}
	}

	return ret;
}

static char *escape_value(const char *value)
{
	size_t i;
	size_t j;
	char *ret;
	bool lead_whitespace;

	for (i = 0, j = 0, lead_whitespace = true; value[i]; i++) {
		switch (value[i]) {
		case ' ':
		case '\t':
			if (lead_whitespace)
				j += 1;

			break;
		case '\n':
		case '\r':
		case '\\':
			j += 1;
			/* fall through */
		default:
			lead_whitespace = false;
		}
	}

	ret = l_malloc(i + j + 1);

	for (i = 0, j = 0, lead_whitespace = true; value[i]; i++) {
		switch (value[i]) {
		case ' ':
			if (lead_whitespace) {
				ret[j++] = '\\';
				ret[j++] = 's';
			} else
				ret[j++] = value[i];

			break;
		case '\t':
			if (lead_whitespace) {
				ret[j++] = '\\';
				ret[j++] = 't';
			} else
				ret[j++] = value[i];

			break;
		case '\n':
			ret[j++] = '\\';
			ret[j++] = 'n';
			lead_whitespace = false;
			break;
		case '\r':
			ret[j++] = '\\';
			ret[j++] = 'r';
			lead_whitespace = false;
			break;
		case '\\':
			ret[j++] = '\\';
			ret[j++] = '\\';
			lead_whitespace = false;
			break;
		default:
			ret[j++] = value[i];
			lead_whitespace = false;
		}
	}

	ret[j] = '\0';

	return ret;
}

static bool parse_group(struct l_settings *settings, const char *data,
			size_t len, size_t line)
{
	size_t i = 1;
	size_t end;
	struct group_data *group;

	while (i < len && data[i] != ']') {
		if (l_ascii_isprint(data[i]) == false || data[i] == '[') {
			l_util_debug(settings->debug_handler,
					settings->debug_data,
					"Invalid group name at line %zd", line);
			return false;
		}

		i += 1;
	}

	if (i >= len) {
		l_util_debug(settings->debug_handler, settings->debug_data,
				"Unterminated group name at line %zd", line);
		return false;
	}

	end = i;
	i += 1;

	while (i < len && l_ascii_isblank(data[i]))
		i += 1;

	if (i != len) {
		l_util_debug(settings->debug_handler, settings->debug_data,
				"Junk characters at the end of line %zd", line);
		return false;
	}

	group = l_new(struct group_data, 1);
	group->name = l_strndup(data + 1, end - 1);
	group->settings = l_queue_new();

	l_queue_push_tail(settings->groups, group);

	return true;
}

static unsigned int parse_key(struct l_settings *settings, const char *data,
				size_t len, size_t line)
{
	unsigned int i;
	unsigned int end;
	struct group_data *group;
	struct setting_data *pair;

	for (i = 0; i < len; i++) {
		if (l_ascii_isalnum(data[i]))
			continue;

		if (data[i] == '_' || data[i] == '-')
			continue;

		if (l_ascii_isblank(data[i]))
			break;

		l_util_debug(settings->debug_handler, settings->debug_data,
				"Invalid character in Key on line %zd", line);

		return 0;
	}

	end = i;

	/* Make sure the rest of the characters are blanks */
	while (i < len) {
		if (l_ascii_isblank(data[i++]))
			continue;

		l_util_debug(settings->debug_handler, settings->debug_data,
					"Garbage after Key on line %zd", line);

		return 0;
	}

	group = l_queue_peek_tail(settings->groups);
	pair = l_new(struct setting_data, 1);
	pair->key = l_strndup(data, end);
	l_queue_push_head(group->settings, pair);

	return end;
}

static bool parse_value(struct l_settings *settings, const char *data,
			size_t len, size_t line)
{
	unsigned int end = len;
	struct group_data *group;
	struct setting_data *pair;

	group = l_queue_peek_tail(settings->groups);
	pair = l_queue_pop_head(group->settings);

	if (!l_utf8_validate(data, len, NULL)) {
		l_util_debug(settings->debug_handler, settings->debug_data,
				"Invalid UTF8 in value on line: %zd", line);

		l_free(pair->key);
		l_free(pair);

		return false;
	}

	pair->value = l_strndup(data, end);
	l_queue_push_tail(group->settings, pair);

	return true;
}

static bool parse_keyvalue(struct l_settings *settings, const char *data,
				size_t len, size_t line)
{
	const char *equal = memchr(data, '=', len);

	if (!equal) {
		l_util_debug(settings->debug_handler, settings->debug_data,
				"Delimiter '=' not found on line: %zd", line);
		return false;
	}

	if (equal == data) {
		l_util_debug(settings->debug_handler, settings->debug_data,
					"Empty key on line: %zd", line);
		return false;
	}

	if (parse_key(settings, data, equal - data, line) == false)
		return false;

	equal += 1;
	while (equal < data + len && l_ascii_isblank(*equal))
		equal += 1;

	return parse_value(settings, equal, len - (equal - data), line);
}

LIB_EXPORT bool l_settings_load_from_data(struct l_settings *settings,
						const char *data, size_t len)
{
	size_t pos = 0;
	bool r = true;
	const char *eol;
	size_t line = 1;
	size_t line_len;

	if (unlikely(!settings || !data || !len))
		return false;

	while (pos < len && r) {
		if (l_ascii_isblank(data[pos])) {
			pos += 1;
			continue;
		}

		if (data[pos] == '\n') {
			line += 1;
			pos += 1;
			continue;
		}

		eol = memchr(data + pos, '\n', len - pos);
		if (!eol)
			eol = data + len;

		line_len = eol - data - pos;

		if (data[pos] == '[')
			r = parse_group(settings, data + pos, line_len, line);
		else if (data[pos] != '#')
			r = parse_keyvalue(settings, data + pos, line_len,
						line);

		pos += line_len;
	}

	return r;
}

LIB_EXPORT char *l_settings_to_data(const struct l_settings *settings,
								size_t *len)
{
	struct l_string *buf;
	char *ret;
	const struct l_queue_entry *group_entry;

	if (unlikely(!settings))
		return NULL;

	buf = l_string_new(255);

	group_entry = l_queue_get_entries(settings->groups);
	while (group_entry) {
		struct group_data *group = group_entry->data;
		const struct l_queue_entry *setting_entry =
				l_queue_get_entries(group->settings);

		l_string_append_printf(buf, "[%s]\n", group->name);

		while (setting_entry) {
			struct setting_data *setting = setting_entry->data;

			l_string_append_printf(buf, "%s=%s\n",
						setting->key, setting->value);
			setting_entry = setting_entry->next;
		}

		if (group_entry->next)
			l_string_append_c(buf, '\n');

		group_entry = group_entry->next;
	}

	ret = l_string_unwrap(buf);

	if (len)
		*len = strlen(ret);

	return ret;
}

LIB_EXPORT bool l_settings_load_from_file(struct l_settings *settings,
						const char *filename)
{
	int fd;
	struct stat st;
	char *data;
	bool r;

	if (unlikely(!settings || !filename))
		return false;

	fd = open(filename, O_RDONLY);
	if (fd < 0) {
		l_util_debug(settings->debug_handler, settings->debug_data,
				"Could not open %s (%s)", filename,
				strerror(errno));
		return false;
	}

	if (fstat(fd, &st) < 0) {
		l_util_debug(settings->debug_handler, settings->debug_data,
				"Could not stat %s (%s)", filename,
				strerror(errno));
		close(fd);

		return false;
	}

	/* Nothing to do, assume success */
	if (st.st_size == 0) {
		close(fd);
		return true;
	}

	data = mmap(NULL, st.st_size, PROT_READ, MAP_SHARED, fd, 0);
	if (data == MAP_FAILED) {
		l_util_debug(settings->debug_handler, settings->debug_data,
				"Could not mmap %s (%s)", filename,
				strerror(errno));
		close(fd);

		return false;
	}

	r = l_settings_load_from_data(settings, data, st.st_size);

	munmap(data, st.st_size);
	close(fd);

	return r;
}

LIB_EXPORT bool l_settings_set_debug(struct l_settings *settings,
					l_settings_debug_cb_t callback,
					void *user_data,
					l_settings_destroy_cb_t destroy)
{
	if (unlikely(!settings))
		return false;

	if (settings->debug_destroy)
		settings->debug_destroy(settings->debug_data);

	settings->debug_handler = callback;
	settings->debug_destroy = destroy;
	settings->debug_data = user_data;

	return true;
}

static bool group_match(const void *a, const void *b)
{
	const struct group_data *group = a;
	const char *name = b;

	return !strcmp(group->name, name);
}

struct gather_data {
	int cur;
	char **v;
};

static void gather_groups(void *data, void *user_data)
{
	struct group_data *group_data = data;
	struct gather_data *gather = user_data;

	gather->v[gather->cur++] = l_strdup(group_data->name);
}

LIB_EXPORT char **l_settings_get_groups(const struct l_settings *settings)
{
	char **ret;
	struct gather_data gather;

	if (unlikely(!settings))
		return NULL;

	ret = l_new(char *, l_queue_length(settings->groups) + 1);
	gather.v = ret;
	gather.cur = 0;

	l_queue_foreach(settings->groups, gather_groups, &gather);

	return ret;
}

LIB_EXPORT bool l_settings_has_group(const struct l_settings *settings,
					const char *group_name)
{
	struct group_data *group;

	if (unlikely(!settings))
		return false;

	group = l_queue_find(settings->groups, group_match, group_name);

	return !!group;
}

static bool key_match(const void *a, const void *b)
{
	const struct setting_data *setting = a;
	const char *key = b;

	return !strcmp(setting->key, key);
}

static void gather_keys(void *data, void *user_data)
{
	struct setting_data *setting_data = data;
	struct gather_data *gather = user_data;

	gather->v[gather->cur++] = l_strdup(setting_data->key);
}

LIB_EXPORT char **l_settings_get_keys(const struct l_settings *settings,
					const char *group_name)
{
	char **ret;
	struct group_data *group_data;
	struct gather_data gather;

	if (unlikely(!settings))
		return NULL;

	group_data = l_queue_find(settings->groups, group_match, group_name);
	if (!group_data)
		return NULL;

	ret = l_new(char *, l_queue_length(group_data->settings) + 1);
	gather.v = ret;
	gather.cur = 0;

	l_queue_foreach(group_data->settings, gather_keys, &gather);

	return ret;
}

LIB_EXPORT bool l_settings_has_key(const struct l_settings *settings,
					const char *group_name, const char *key)
{
	struct group_data *group;
	struct setting_data *setting;

	if (unlikely(!settings))
		return false;

	group = l_queue_find(settings->groups, group_match, group_name);
	if (!group)
		return false;

	setting = l_queue_find(group->settings, key_match, key);

	return !!setting;
}

LIB_EXPORT const char *l_settings_get_value(const struct l_settings *settings,
						const char *group_name,
						const char *key)
{
	struct group_data *group;
	struct setting_data *setting;

	if (unlikely(!settings))
		return NULL;

	group = l_queue_find(settings->groups, group_match, group_name);
	if (!group)
		return NULL;

	setting = l_queue_find(group->settings, key_match, key);
	if (!setting)
		return NULL;

	return setting->value;
}

static bool validate_group_name(const char *group_name)
{
	int i;

	for (i = 0; group_name[i]; i++) {
		if (!l_ascii_isprint(group_name[i]))
			return false;

		if (group_name[i] == ']' || group_name[i] == '[')
			return false;
	}

	return true;
}

static bool validate_key(const char *key)
{
	int i;

	for (i = 0; key[i]; i++) {
		if (l_ascii_isalnum(key[i]))
			continue;

		if (key[i] == '_' || key[i] == '-')
			continue;

		return false;
	}

	return true;
}

static bool set_value(struct l_settings *settings, const char *group_name,
			const char *key, char *value)
{
	struct group_data *group;
	struct setting_data *pair;

	if (!validate_group_name(group_name)) {
		l_util_debug(settings->debug_handler, settings->debug_data,
				"Invalid group name %s", group_name);
		l_free(value);

		return false;
	}

	if (!validate_key(key)) {
		l_util_debug(settings->debug_handler, settings->debug_data,
				"Invalid key %s", key);
		l_free(value);

		return false;
	}

	group = l_queue_find(settings->groups, group_match, group_name);
	if (!group) {
		group = l_new(struct group_data, 1);
		group->name = l_strdup(group_name);
		group->settings = l_queue_new();

		l_queue_push_tail(settings->groups, group);
		goto add_pair;
	}

	pair = l_queue_find(group->settings, key_match, key);
	if (!pair) {
add_pair:
		pair = l_new(struct setting_data, 1);
		pair->key = l_strdup(key);
		pair->value = value;
		l_queue_push_tail(group->settings, pair);

		return true;
	}

	l_free(pair->value);
	pair->value = value;

	return true;
}

LIB_EXPORT bool l_settings_set_value(struct l_settings *settings,
					const char *group_name, const char *key,
					const char *value)
{
	if (unlikely(!settings || !value))
		return false;

	return set_value(settings, group_name, key, l_strdup(value));
}

LIB_EXPORT bool l_settings_get_bool(const struct l_settings *settings,
					const char *group_name, const char *key,
					bool *out)
{
	const char *value;

	value = l_settings_get_value(settings, group_name, key);
	if (!value)
		return false;

	if (!strcasecmp(value, "true") || !strcmp(value, "1")) {
		if (out)
			*out = true;

		return true;
	}

	if (!strcasecmp(value, "false") || !strcmp(value, "0")) {
		if (out)
			*out = false;

		return true;
	}

	l_util_debug(settings->debug_handler, settings->debug_data,
			"Could not interpret %s as a bool", value);

	return false;
}

LIB_EXPORT bool l_settings_set_bool(struct l_settings *settings,
					const char *group_name, const char *key,
					bool in)
{
	static const char *true_str = "true";
	static const char *false_str = "false";
	const char *v;

	if (in == false)
		v = false_str;
	else
		v = true_str;

	return l_settings_set_value(settings, group_name, key, v);
}

LIB_EXPORT bool l_settings_get_int(const struct l_settings *settings,
					const char *group_name,
					const char *key, int *out)
{
	const char *value = l_settings_get_value(settings, group_name, key);
	long int r;
	int t;
	char *endp;

	if (!value)
		return false;

	if (*value == '\0')
		goto error;

	errno = 0;

	t = r = strtol(value, &endp, 10);
	if (*endp != '\0')
		goto error;

	if (unlikely(errno == ERANGE || r != t))
		goto error;

	if (out)
		*out = r;

	return true;

error:
	l_util_debug(settings->debug_handler, settings->debug_data,
			"Could not interpret %s as an int", value);

	return false;
}

LIB_EXPORT bool l_settings_set_int(struct l_settings *settings,
					const char *group_name, const char *key,
					int in)
{
	char buf[64];

	snprintf(buf, sizeof(buf), "%d", in);

	return l_settings_set_value(settings, group_name, key, buf);
}

LIB_EXPORT bool l_settings_get_uint(const struct l_settings *settings,
					const char *group_name, const char *key,
					unsigned int *out)
{
	const char *value = l_settings_get_value(settings, group_name, key);
	unsigned long int r;
	unsigned int t;
	char *endp;

	if (!value)
		return false;

	if (*value == '\0')
		goto error;

	errno = 0;

	t = r = strtoul(value, &endp, 10);
	if (*endp != '\0')
		goto error;

	if (unlikely(errno == ERANGE || r != t))
		goto error;

	if (out)
		*out = r;

	return true;

error:
	l_util_debug(settings->debug_handler, settings->debug_data,
			"Could not interpret %s as a uint", value);

	return false;
}

LIB_EXPORT bool l_settings_set_uint(struct l_settings *settings,
					const char *group_name, const char *key,
					unsigned int in)
{
	char buf[64];

	snprintf(buf, sizeof(buf), "%u", in);

	return l_settings_set_value(settings, group_name, key, buf);
}

LIB_EXPORT bool l_settings_get_int64(const struct l_settings *settings,
					const char *group_name, const char *key,
					int64_t *out)
{
	const char *value = l_settings_get_value(settings, group_name, key);
	int64_t r;
	char *endp;

	if (!value)
		return false;

	if (*value == '\0')
		goto error;

	errno = 0;

	r = strtoll(value, &endp, 10);
	if (*endp != '\0')
		goto error;

	if (unlikely(errno == ERANGE))
		goto error;

	if (out)
		*out = r;

	return true;

error:
	l_util_debug(settings->debug_handler, settings->debug_data,
			"Could not interpret %s as an int64", value);

	return false;
}

LIB_EXPORT bool l_settings_set_int64(struct l_settings *settings,
					const char *group_name, const char *key,
					int64_t in)
{
	char buf[64];

	snprintf(buf, sizeof(buf), "%" PRId64, in);

	return l_settings_set_value(settings, group_name, key, buf);
}

LIB_EXPORT bool l_settings_get_uint64(const struct l_settings *settings,
					const char *group_name, const char *key,
					uint64_t *out)
{
	const char *value = l_settings_get_value(settings, group_name, key);
	uint64_t r;
	char *endp;

	if (!value)
		return false;

	if (*value == '\0')
		goto error;

	errno = 0;

	r = strtoull(value, &endp, 10);
	if (*endp != '\0')
		goto error;

	if (unlikely(errno == ERANGE))
		goto error;

	if (out)
		*out = r;

	return true;

error:
	l_util_debug(settings->debug_handler, settings->debug_data,
			"Could not interpret %s as a uint64", value);

	return false;
}

LIB_EXPORT bool l_settings_set_uint64(struct l_settings *settings,
					const char *group_name, const char *key,
					uint64_t in)
{
	char buf[64];

	snprintf(buf, sizeof(buf), "%" PRIu64, in);

	return l_settings_set_value(settings, group_name, key, buf);
}

LIB_EXPORT char *l_settings_get_string(const struct l_settings *settings,
					const char *group_name, const char *key)
{
	const char *value = l_settings_get_value(settings, group_name, key);

	if (!value)
		return NULL;

	return unescape_value(value);
}

LIB_EXPORT bool l_settings_set_string(struct l_settings *settings,
					const char *group_name, const char *key,
					const char *value)
{
	char *buf;

	if (unlikely(!settings || !value))
		return false;

	buf = escape_value(value);

	return set_value(settings, group_name, key, buf);
}

LIB_EXPORT char **l_settings_get_string_list(const struct l_settings *settings,
						const char *group_name,
						const char *key,
						const char delimiter)
{
	const char *value = l_settings_get_value(settings, group_name, key);
	char *str;
	char **ret;

	if (!value)
		return NULL;

	str = unescape_value(value);
	if (str == NULL)
		return NULL;

	ret = l_strsplit(str, delimiter);
	l_free(str);

	return ret;
}

LIB_EXPORT bool l_settings_set_string_list(struct l_settings *settings,
					const char *group_name, const char *key,
					char **value, char delimiter)
{
	char *buf;
	char *tmp;

	if (unlikely(!settings || !value))
		return false;

	tmp = l_strjoinv(value, delimiter);
	buf = escape_value(tmp);
	l_free(tmp);

	return set_value(settings, group_name, key, buf);
}

LIB_EXPORT bool l_settings_get_double(const struct l_settings *settings,
					const char *group_name, const char *key,
					double *out)
{
	const char *value = l_settings_get_value(settings, group_name, key);
	char *endp;
	double r;

	if (!value)
		return NULL;

	if (*value == '\0')
		goto error;

	errno = 0;

	r = strtod(value, &endp);
	if (*endp != '\0')
		goto error;

	if (unlikely(errno == ERANGE))
		goto error;

	if (out)
		*out = r;

	return true;

error:
	l_util_debug(settings->debug_handler, settings->debug_data,
			"Could not interpret %s as a double", value);

	return false;
}

LIB_EXPORT bool l_settings_set_double(struct l_settings *settings,
					const char *group_name, const char *key,
					double in)
{
	L_AUTO_FREE_VAR(char *, buf);

	buf = l_strdup_printf("%d", in);

	return l_settings_set_value(settings, group_name, key, buf);
}

LIB_EXPORT bool l_settings_get_float(const struct l_settings *settings,
					const char *group_name, const char *key,
					float *out)
{
	const char *value = l_settings_get_value(settings, group_name, key);
	char *endp;
	float r;

	if (!value)
		return NULL;

	if (*value == '\0')
		goto error;

	errno = 0;

	r = strtof(value, &endp);
	if (*endp != '\0')
		goto error;

	if (unlikely(errno == ERANGE))
		goto error;

	if (out)
		*out = r;

	return true;

error:
	l_util_debug(settings->debug_handler, settings->debug_data,
			"Could not interpret %s as a float", value);

	return false;
}

LIB_EXPORT bool l_settings_set_float(struct l_settings *settings,
					const char *group_name, const char *key,
					float in)
{
	L_AUTO_FREE_VAR(char *, buf);

	buf = l_strdup_printf("%f", in);

	return l_settings_set_value(settings, group_name, key, buf);
}

LIB_EXPORT bool l_settings_remove_group(struct l_settings *settings,
					const char *group_name)
{
	struct group_data *group;

	if (unlikely(!settings))
		return false;

	group = l_queue_remove_if(settings->groups, group_match, group_name);
	if (!group)
		return false;

	group_destroy(group);

	return true;
}

LIB_EXPORT bool l_settings_remove_key(struct l_settings *settings,
					const char *group_name,
					const char *key)
{
	struct group_data *group;
	struct setting_data *setting;

	if (unlikely(!settings))
		return false;

	group = l_queue_find(settings->groups, group_match, group_name);
	if (!group)
		return false;

	setting = l_queue_remove_if(group->settings, key_match, key);
	if (!setting)
		return false;

	setting_destroy(setting);

	return true;
}
