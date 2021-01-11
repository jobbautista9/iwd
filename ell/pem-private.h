/*
 *
 *  Embedded Linux library
 *
 *  Copyright (C) 2019  Intel Corporation. All rights reserved.
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
#include <sys/types.h>

struct l_certchain;

const char *pem_next(const void *buf, size_t buf_len, char **type_label,
				size_t *base64_len,
				const char **endp, bool strict);

struct l_key *pem_key_from_pkcs8_private_key_info(const uint8_t *der,
							size_t der_len);

struct l_key *pem_key_from_pkcs8_encrypted_private_key_info(const uint8_t *der,
							size_t der_len,
							const char *passphrase);

int pem_write_certificate_chain(const struct l_certchain *cert,
				const char *filename);
