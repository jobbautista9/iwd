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

#define _GNU_SOURCE
#include <stdio.h>
#include <ctype.h>
#include <stdarg.h>
#include <stdlib.h>
#include <limits.h>

#include "util.h"
#include "private.h"

/**
 * SECTION:util
 * @short_description: Utility functions
 *
 * Utility functions
 */

#define STRLOC __FILE__ ":" L_STRINGIFY(__LINE__)

/**
 * l_malloc:
 * @size: memory size to allocate
 *
 * If for any reason the memory allocation fails, then execution will be
 * halted via abort().
 *
 * In case @size is 0 then #NULL will be returned.
 *
 * Returns: pointer to allocated memory
 **/
LIB_EXPORT void *l_malloc(size_t size)
{
	if (likely(size)) {
		void *ptr;

		ptr = malloc(size);
		if (ptr)
			return ptr;

		fprintf(stderr, "%s:%s(): failed to allocate %zd bytes\n",
					STRLOC, __PRETTY_FUNCTION__, size);
		abort();
	}

	return NULL;
}

/**
 * l_realloc:
 * @mem: previously allocated memory, or NULL
 * @size: memory size to allocate
 *
 * If for any reason the memory allocation fails, then execution will be
 * halted via abort().
 *
 * In case @mem is NULL, this function acts like l_malloc.
 * In case @size is 0 then #NULL will be returned.
 *
 * Returns: pointer to allocated memory
 **/
LIB_EXPORT void *l_realloc(void *mem, size_t size)
{
	if (likely(size)) {
		void *ptr;

		ptr = realloc(mem, size);
		if (ptr)
			return ptr;

		fprintf(stderr, "%s:%s(): failed to re-allocate %zd bytes\n",
					STRLOC, __PRETTY_FUNCTION__, size);
		abort();
	} else
		l_free(mem);

	return NULL;
}

/**
 * l_memdup:
 * @mem: pointer to memory you want to duplicate
 * @size: memory size
 *
 * If for any reason the memory allocation fails, then execution will be
 * halted via abort().
 *
 * In case @size is 0 then #NULL will be returned.
 *
 * Returns: pointer to duplicated memory buffer
 **/
LIB_EXPORT void *l_memdup(const void *mem, size_t size)
{
	void *ptr;

	ptr = l_malloc(size);

	memcpy(ptr, mem, size);

	return ptr;
}

/**
 * l_free:
 * @ptr: memory pointer
 *
 * Free the allocated memory area.
 **/
LIB_EXPORT void l_free(void *ptr)
{
	free(ptr);
}

/**
 * l_strdup:
 * @str: string pointer
 *
 * Allocates and duplicates sring
 *
 * Returns: a newly allocated string
 **/
LIB_EXPORT char *l_strdup(const char *str)
{
	if (likely(str)) {
		char *tmp;

		tmp = strdup(str);
		if (tmp)
			return tmp;

		fprintf(stderr, "%s:%s(): failed to allocate string\n",
						STRLOC, __PRETTY_FUNCTION__);
		abort();
	}

	return NULL;
}

/**
 * l_strndup:
 * @str: string pointer
 * @max: Maximum number of characters to copy
 *
 * Allocates and duplicates sring.  If the string is longer than @max
 * characters, only @max are copied and a null terminating character
 * is added.
 *
 * Returns: a newly allocated string
 **/
LIB_EXPORT char *l_strndup(const char *str, size_t max)
{
	if (likely(str)) {
		char *tmp;

		tmp = strndup(str, max);
		if (tmp)
			return tmp;

		fprintf(stderr, "%s:%s(): failed to allocate string\n",
						STRLOC, __PRETTY_FUNCTION__);
		abort();
	}

	return NULL;
}

/**
 * l_strdup_printf:
 * @format: string format
 * @...: parameters to insert into format string
 *
 * Returns: a newly allocated string
 **/
LIB_EXPORT char *l_strdup_printf(const char *format, ...)
{
	va_list args;
	char *str;
	int len;

	va_start(args, format);
	len = vasprintf(&str, format, args);
	va_end(args);

	if (len < 0) {
		fprintf(stderr, "%s:%s(): failed to allocate string\n",
					STRLOC, __PRETTY_FUNCTION__);
		abort();

		return NULL;
	}

	return str;
}

/**
 * l_strdup_vprintf:
 * @format: string format
 * @args: parameters to insert into format string
 *
 * Returns: a newly allocated string
 **/
LIB_EXPORT char *l_strdup_vprintf(const char *format, va_list args)
{
	char *str;
	int len;

	len = vasprintf(&str, format, args);

	if (len < 0) {
		fprintf(stderr, "%s:%s(): failed to allocate string\n",
					STRLOC, __PRETTY_FUNCTION__);
		abort();

		return NULL;
	}

	return str;
}

/**
 * l_strfreev:
 * @strlist: String list to free
 *
 * Frees a list of strings
 **/
LIB_EXPORT void l_strfreev(char **strlist)
{
	if (likely(strlist)) {
		int i;

		for (i = 0; strlist[i]; i++)
			l_free(strlist[i]);

		l_free(strlist);
	}
}

/**
 * l_strsplit:
 * @str: String to split
 * @sep: The delimiter character
 *
 * Splits a string into pieces which do not contain the delimiter character.
 * As a special case, an empty string is returned as an empty array, e.g.
 * an array with just the NULL element.
 *
 * Note that this function only works with ASCII delimiters.
 *
 * Returns: A newly allocated %NULL terminated string array.  This array
 * should be freed using l_strfreev().
 **/
LIB_EXPORT char **l_strsplit(const char *str, const char sep)
{
	int len;
	int i;
	const char *p;
	char **ret;

	if (unlikely(!str))
		return NULL;

	if (str[0] == '\0')
		return l_new(char *, 1);

	for (p = str, len = 1; *p; p++)
		if (*p == sep)
			len += 1;

	ret = l_new(char *, len + 1);

	i = 0;
	p = str;
	len = 0;

	while (p[len]) {
		if (p[len] != sep) {
			len += 1;
			continue;
		}

		ret[i++] = l_strndup(p, len);
		p += len + 1;
		len = 0;
	}

	ret[i++] = l_strndup(p, len);

	return ret;
}

/**
 * l_strsplit_set:
 * @str: String to split
 * @separators: A set of delimiters
 *
 * Splits a string into pieces which do not contain the delimiter characters
 * that can be found in @separators.
 * As a special case, an empty string is returned as an empty array, e.g.
 * an array with just the NULL element.
 *
 * Note that this function only works with ASCII delimiters.
 *
 * Returns: A newly allocated %NULL terminated string array.  This array
 * should be freed using l_strfreev().
 **/
LIB_EXPORT char **l_strsplit_set(const char *str, const char *separators)
{
	int len;
	int i;
	const char *p;
	char **ret;
	bool sep_table[256];

	if (unlikely(!str))
		return NULL;

	if (str[0] == '\0')
		return l_new(char *, 1);

	memset(sep_table, 0, sizeof(sep_table));

	for (p = separators; *p; p++)
		sep_table[(unsigned char) *p] = true;

	for (p = str, len = 1; *p; p++)
		if (sep_table[(unsigned char) *p] == true)
			len += 1;

	ret = l_new(char *, len + 1);

	i = 0;
	p = str;
	len = 0;

	while (p[len]) {
		if (sep_table[(unsigned char) p[len]] != true) {
			len += 1;
			continue;
		}

		ret[i++] = l_strndup(p, len);
		p += len + 1;
		len = 0;
	}

	ret[i++] = l_strndup(p, len);

	return ret;
}

/**
 * l_strjoinv:
 * @str_array: a %NULL terminated array of strings to join
 * @delim: Delimiting character
 *
 * Joins strings contanied in the @str_array into one long string delimited
 * by @delim.
 *
 * Returns: A newly allocated string that should be freed using l_free()
 */
LIB_EXPORT char *l_strjoinv(char **str_array, const char delim)
{
	size_t len = 0;
	unsigned int i;
	char *ret;
	char *p;

	if (unlikely(!str_array))
		return NULL;

	if (!str_array[0])
		return l_strdup("");

	for (i = 0; str_array[i]; i++)
		len += strlen(str_array[i]);

	len += 1 + i - 1;

	ret = l_malloc(len);

	p = stpcpy(ret, str_array[0]);

	for (i = 1; str_array[i]; i++) {
		*p++ = delim;
		p = stpcpy(p, str_array[i]);
	}

	return ret;
}

/**
 * l_strv_length:
 * @str_array: a %NULL terminated array of strings
 *
 * Returns: the number of strings in @str_array
 */
LIB_EXPORT unsigned int l_strv_length(char **str_array)
{
	unsigned int i = 0;

	if (unlikely(!str_array))
		return 0;

	while (str_array[i])
		i += 1;

	return i;
}

/**
 * l_strv_contains:
 * @str_array: a %NULL terminated array of strings
 * @item: An item to search for, must be not %NULL
 *
 * Returns: #true if @str_array contains item
 */
LIB_EXPORT bool l_strv_contains(char **str_array, const char *item)
{
	unsigned int i = 0;

	if (unlikely(!str_array || !item))
		return false;

	while (str_array[i]) {
		if (!strcmp(str_array[i], item))
			return true;

		i += 1;
	}

	return false;
}

/**
 * l_str_has_prefix:
 * @str: A string to be examined
 * @delim: Prefix string
 *
 * Determines if the string given by @str is prefixed by string given by
 * @prefix.
 *
 * Returns: True if @str was prefixed by @prefix.  False otherwise.
 */
LIB_EXPORT bool l_str_has_prefix(const char *str, const char *prefix)
{
	size_t str_len;
	size_t prefix_len;

	if (unlikely(!str))
		return false;

	if (unlikely(!prefix))
		return false;

	str_len = strlen(str);
	prefix_len = strlen(prefix);

	if (str_len < prefix_len)
		return false;

	return !strncmp(str, prefix, prefix_len);
}

/**
 * l_strlcpy:
 * @dst: Destination buffer for string
 * @src: Source buffer containing null-terminated string to copy
 * @len: Maximum destination buffer space to use
 *
 * Copies a string from the @src buffer to the @dst buffer, using no
 * more than @len bytes in @dst. @dst is guaranteed to be
 * null-terminated. The caller can determine if the copy was truncated by
 * checking if the return value is greater than or equal to @len.
 *
 * Returns: The length of the @src string, not including the null
 * terminator.
 */
LIB_EXPORT size_t l_strlcpy(char* dst, const char *src, size_t len)
{
	size_t src_len = strlen(src);

	if (len) {
		if (src_len < len) {
			len = src_len + 1;
		} else {
			len -= 1;
			dst[len] = '\0';
		}

		memcpy(dst, src, len);
	}

	return src_len;
}

/**
 * l_str_has_suffix:
 * @str: A string to be examined
 * @suffix: Suffix string
 *
 * Determines if the string given by @str ends with the specified @suffix.
 *
 * Returns: True if @str ends with the specified @suffix.  False otherwise.
 */
LIB_EXPORT bool l_str_has_suffix(const char *str, const char *suffix)
{
	size_t str_len;
	size_t suffix_len;
	size_t len_diff;

	if (unlikely(!str))
		return false;

	if (unlikely(!suffix))
		return false;

	str_len = strlen(str);
	suffix_len = strlen(suffix);

	if (str_len < suffix_len)
		return false;

	len_diff = str_len - suffix_len;

	return !strcmp(&str[len_diff], suffix);
}

/**
 * l_util_hexstring:
 * @buf: buffer pointer
 * @len: length of buffer
 *
 * Returns: a newly allocated hex string
 **/
LIB_EXPORT char *l_util_hexstring(const unsigned char *buf, size_t len)
{
	static const char hexdigits[] = "0123456789abcdef";
	char *str;
	size_t i;

	if (unlikely(!buf) || unlikely(!len))
		return NULL;

	str = l_malloc(len * 2 + 1);

	for (i = 0; i < len; i++) {
		str[(i * 2) + 0] = hexdigits[buf[i] >> 4];
		str[(i * 2) + 1] = hexdigits[buf[i] & 0xf];
	}

	str[len * 2] = '\0';

	return str;
}

/**
 * l_util_from_hexstring:
 * @str: Null-terminated string containing the hex-encoded bytes
 * @out_len: Number of bytes decoded
 *
 * Returns: a newly allocated byte array
 **/
LIB_EXPORT unsigned char *l_util_from_hexstring(const char *str,
							size_t *out_len)
{
	size_t i, j;
	size_t len;
	char c;
	unsigned char *buf;

	if (unlikely(!str))
		return NULL;

	for (i = 0; str[i]; i++) {
		c = toupper(str[i]);

		if ((c >= '0' && c <= '9') || (c >= 'A' && c <= 'F'))
			continue;

		return NULL;
	}

	if ((i % 2) != 0)
		return NULL;

	len = i;
	buf = l_malloc(i >> 1);

	for (i = 0, j = 0; i < len; i++, j++) {
		c = toupper(str[i]);

		if (c >= '0' && c <= '9')
			buf[j] = c - '0';
		else if (c >= 'A' && c <= 'F')
			buf[j] = 10 + c - 'A';

		i += 1;

		c = toupper(str[i]);

		if (c >= '0' && c <= '9')
			buf[j] = buf[j] * 16 + c - '0';
		else if (c >= 'A' && c <= 'F')
			buf[j] = buf[j] * 16 + 10 + c - 'A';
	}

	if (out_len)
		*out_len = j;

	return buf;
}

static void hexdump(const char dir, const unsigned char *buf, size_t len,
			l_util_hexdump_func_t function, void *user_data)
{
	static const char hexdigits[] = "0123456789abcdef";
	char str[68];
	size_t i;

	if (unlikely(!len))
		return;

	str[0] = dir;

	for (i = 0; i < len; i++) {
		str[((i % 16) * 3) + 1] = ' ';
		str[((i % 16) * 3) + 2] = hexdigits[buf[i] >> 4];
		str[((i % 16) * 3) + 3] = hexdigits[buf[i] & 0xf];
		str[(i % 16) + 51] = isprint(buf[i]) ? buf[i] : '.';

		if ((i + 1) % 16 == 0) {
			str[49] = ' ';
			str[50] = ' ';
			str[67] = '\0';
			function(str, user_data);
			str[0] = ' ';
		}
	}

	if (i % 16 > 0) {
		size_t j;
		for (j = (i % 16); j < 16; j++) {
			str[(j * 3) + 1] = ' ';
			str[(j * 3) + 2] = ' ';
			str[(j * 3) + 3] = ' ';
			str[j + 51] = ' ';
		}
		str[49] = ' ';
		str[50] = ' ';
		str[67] = '\0';
		function(str, user_data);
	}
}

LIB_EXPORT void l_util_hexdump(bool in, const void *buf, size_t len,
			l_util_hexdump_func_t function, void *user_data)
{
	if (likely(!function))
		return;

	hexdump(in ? '<' : '>', buf, len, function, user_data);
}

LIB_EXPORT void l_util_hexdump_two(bool in, const void *buf1, size_t len1,
			const void *buf2, size_t len2,
			l_util_hexdump_func_t function, void *user_data)
{
	if (likely(!function))
		return;

	hexdump(in ? '<' : '>', buf1, len1, function, user_data);
	hexdump(' ', buf2, len2, function, user_data);
}

LIB_EXPORT void l_util_hexdumpv(bool in, const struct iovec *iov,
					size_t n_iov,
					l_util_hexdump_func_t function,
					void *user_data)
{
	static const char hexdigits[] = "0123456789abcdef";
	char str[68];
	size_t i;
	size_t len;
	size_t c;
	const uint8_t *buf;

	if (unlikely(!iov || !n_iov))
		return;

	str[0] = in ? '<' : '>';

	for (i = 0, len = 0; i < n_iov; i++)
		len += iov[i].iov_len;

	c = 0;
	buf = iov[0].iov_base;

	for (i = 0; i < len; i++, c++) {
		if (c == iov[0].iov_len) {
			c = 0;
			iov += 1;
			buf = iov[0].iov_base;
		}

		str[((i % 16) * 3) + 1] = ' ';
		str[((i % 16) * 3) + 2] = hexdigits[buf[c] >> 4];
		str[((i % 16) * 3) + 3] = hexdigits[buf[c] & 0xf];
		str[(i % 16) + 51] = isprint(buf[c]) ? buf[c] : '.';

		if ((i + 1) % 16 == 0) {
			str[49] = ' ';
			str[50] = ' ';
			str[67] = '\0';
			function(str, user_data);
			str[0] = ' ';
		}
	}

	if (i % 16 > 0) {
		size_t j;
		for (j = (i % 16); j < 16; j++) {
			str[(j * 3) + 1] = ' ';
			str[(j * 3) + 2] = ' ';
			str[(j * 3) + 3] = ' ';
			str[j + 51] = ' ';
		}
		str[49] = ' ';
		str[50] = ' ';
		str[67] = '\0';
		function(str, user_data);
	}
}

LIB_EXPORT void l_util_debug(l_util_hexdump_func_t function, void *user_data,
						const char *format, ...)
{
	va_list args;
	char *str;
	int len;

	if (likely(!function))
		return;

	if (unlikely(!format))
		return;

	va_start(args, format);
	len = vasprintf(&str, format, args);
	va_end(args);

	if (unlikely(len < 0))
		return;

	function(str, user_data);

	free(str);
}

/**
 * l_util_get_debugfs_path:
 *
 * Returns: a pointer to mount point of debugfs
 **/
LIB_EXPORT const char *l_util_get_debugfs_path(void)
{
	static char path[PATH_MAX + 1];
	static bool found = false;
	char type[100];
	FILE *fp;

	if (found)
		return path;

	fp = fopen("/proc/mounts", "r");
	if (!fp)
		return NULL;

	while (fscanf(fp, "%*s %" L_STRINGIFY(PATH_MAX) "s %99s %*s %*d %*d\n",
							path, type) == 2) {
		if (!strcmp(type, "debugfs")) {
			found = true;
			break;
		}
	}

	fclose(fp);

	if (!found)
		return NULL;

	return path;
}
