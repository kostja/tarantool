/**
 * PostgreSQL Database Management System
 * (formerly known as Postgres, then as Postgres95)
 *
 * Portions Copyright (c) 1996-2023, PostgreSQL Global Development Group
 *
 * Portions Copyright (c) 1994, The Regents of the University of California
 *
 * Permission to use, copy, modify, and distribute this software and its
 * documentation for any purpose, without fee, and without a written agreement
 * is hereby granted, provided that the above copyright notice and this
 * paragraph and the following two paragraphs appear in all copies.
 *
 * IN NO EVENT SHALL THE UNIVERSITY OF CALIFORNIA BE LIABLE TO ANY PARTY FOR
 * DIRECT, INDIRECT, SPECIAL, INCIDENTAL, OR CONSEQUENTIAL DAMAGES, INCLUDING
 * LOST PROFITS, ARISING OUT OF THE USE OF THIS SOFTWARE AND ITS
 * DOCUMENTATION, EVEN IF THE UNIVERSITY OF CALIFORNIA HAS BEEN ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
 *
 * THE UNIVERSITY OF CALIFORNIA SPECIFICALLY DISCLAIMS ANY WARRANTIES,
 * INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY
 * AND FITNESS FOR A PARTICULAR PURPOSE.  THE SOFTWARE PROVIDED HEREUNDER IS
 * ON AN "AS IS" BASIS, AND THE UNIVERSITY OF CALIFORNIA HAS NO OBLIGATIONS TO
 * PROVIDE MAINTENANCE, SUPPORT, UPDATES, ENHANCEMENTS, OR MODIFICATIONS.
 */
#include "crypt.h"
#include "md5.h"
#include "cryptohash.h"
#include "trivia/util.h"
#include "diag.h"

#include <string.h>

/**
 * Calculates the MD5 sum of the bytes in a buffer.
 *
 * @param buf the buffer containing the bytes that you want the MD5 sum of.
 * @param len number of bytes in the buffer.
 *
 * @param hexsum the resulting MD5 sum as a non-null-terminated string of
 *		 hexadecimal digits which is exactly 32 bytes long.
 *		 Each input byte is represented by two hexadecimal
 *		 characters.
 *
 * @note MD5 is described in RFC 1321.
 *
 * @author Sverre H. Huseby <sverrehu@online.no>
 *
 */
static void
md5_hash(const void *buf, size_t len, char out[static 32])
{
	uint8_t sum[MD5_DIGEST_LENGTH];

	struct cryptohash_ctx *ctx = cryptohash_create(CRYPTOHASH_MD5);
	cryptohash_init(ctx);
	cryptohash_update(ctx, buf, len);
	cryptohash_final(ctx, sum, sizeof(sum));
	tt_bin2hex(sum, MD5_DIGEST_LENGTH, out);
	cryptohash_free(ctx);
}

void
md5_encrypt(const char *password, size_t password_len,
	    const char *salt, size_t salt_len,
	    char out[static MD5_PASSWD_LEN])
{
	if (password_len + salt_len == 0) {
		memcpy(out, "md5", strlen("md5"));
		md5_hash("", 0, out + strlen("md5"));
		return;
	}

	char *crypt_buf = xmalloc(password_len + salt_len);

	/*
	 * Place salt at the end because it may be known by users trying to
	 * crack the MD5 output.
	 */
	memcpy(crypt_buf, password, password_len);
	memcpy(crypt_buf + password_len, salt, salt_len);

	memcpy(out, "md5", strlen("md5"));
	md5_hash(crypt_buf, password_len + salt_len, out + strlen("md5"));

	free(crypt_buf);
}
