/*
 * lib/crypto/pbkdf2.c
 *
 * Copyright 2002, 2008 by the Massachusetts Institute of Technology.
 * All Rights Reserved.
 *
 * Export of this software from the United States of America may
 *   require a specific license from the United States Government.
 *   It is the responsibility of any person or organization contemplating
 *   export to obtain such a license before exporting.
 *
 * WITHIN THAT CONSTRAINT, permission to use, copy, modify, and
 * distribute this software and its documentation for any purpose and
 * without fee is hereby granted, provided that the above copyright
 * notice appear in all copies and that both that copyright notice and
 * this permission notice appear in supporting documentation, and that
 * the name of M.I.T. not be used in advertising or publicity pertaining
 * to distribution of the software without specific, written prior
 * permission.  Furthermore if you modify this software you must label
 * your software as modified software and not distribute it in such a
 * fashion that it might be confused with the original M.I.T. software.
 * M.I.T. makes no representations about the suitability of
 * this software for any purpose.  It is provided "as is" without express
 * or implied warranty.
 *
 *
 * Implementation of PBKDF2 from RFC 2898.
 */

#include <ctype.h>
#include "k5-int.h"
#include "hash_provider.h"

/*
 * RFC 2898 specifies PBKDF2 in terms of an underlying pseudo-random
 * function with two arguments (password and salt||blockindex).  Right
 * now we only use PBKDF2 with the hmac-sha1 PRF, also specified in
 * RFC 2898, which invokes HMAC with the password as the key and the
 * second argument as the text.  (HMAC accepts any key size up to the
 * block size; the password is pre-hashed with unkeyed SHA1 if it is
 * longer than the block size.)
 *
 * For efficiency, it is better to generate the key from the password
 * once at the beginning, so we specify prf_func in terms of a
 * krb5_key first argument.  That might not be convenient for a PRF
 * which uses the password in some other way, so this might need to be
 * adjusted in the future.
 */

typedef krb5_error_code (*prf_func)(krb5_key pass, krb5_data *salt,
				    krb5_data *out);

/* Not exported, for now.  */
static krb5_error_code
krb5int_pbkdf2 (prf_func prf, size_t hlen, krb5_key pass,
		const krb5_data *salt, unsigned long count,
		const krb5_data *output);

static int debug_hmac = 0;

static void printd (const char *descr, krb5_data *d) {
    unsigned int i, j;
    const int r = 16;

    printf("%s:", descr);

    for (i = 0; i < d->length; i += r) {
	printf("\n  %04x: ", i);
	for (j = i; j < i + r && j < d->length; j++)
	    printf(" %02x", 0xff & d->data[j]);
	for (; j < i + r; j++)
	    printf("   ");
	printf("   ");
	for (j = i; j < i + r && j < d->length; j++) {
	    int c = 0xff & d->data[j];
	    printf("%c", isprint(c) ? c : '.');
	}
    }
    printf("\n");
}

static krb5_error_code
F(char *output, char *u_tmp1, char *u_tmp2, prf_func prf, size_t hlen,
  krb5_key pass, const krb5_data *salt, unsigned long count, int i)
{
    unsigned char ibytes[4];
    size_t tlen;
    unsigned int j, k;
    krb5_data sdata;
    krb5_data out;
    krb5_error_code err;

#if 0
    printf("F(i=%d, count=%lu, pass=%d:%s)\n", i, count,
	   pass->length, pass->data);
#endif

    /* Compute U_1.  */
    store_32_be(i, ibytes);

    tlen = salt->length;
    memcpy(u_tmp2, salt->data, tlen);
    memcpy(u_tmp2 + tlen, ibytes, 4);
    tlen += 4;
    sdata.data = u_tmp2;
    sdata.length = tlen;

#if 0
    printd("initial salt", &sdata);
#endif

    out.data = u_tmp1;
    out.length = hlen;

#if 0
    printf("F: computing hmac #1 (U_1) with %s\n", pdata.contents);
#endif
    err = (*prf)(pass, &sdata, &out);
    if (err)
	return err;
#if 0
    printd("F: prf return value", &out);
#endif
    memcpy(output, u_tmp1, hlen);

    /* Compute U_2, .. U_c.  */
    sdata.length = hlen;
    for (j = 2; j <= count; j++) {
#if 0
	printf("F: computing hmac #%d (U_%d)\n", j, j);
#endif
	memcpy(u_tmp2, u_tmp1, hlen);
	err = (*prf)(pass, &sdata, &out);
	if (err)
	    return err;
#if 0
	printd("F: prf return value", &out);
#endif
	/* And xor them together.  */
	for (k = 0; k < hlen; k++)
	    output[k] ^= u_tmp1[k];
#if 0
	printf("F: xor result:\n");
	for (k = 0; k < hlen; k++)
	    printf(" %02x", 0xff & output[k]);
	printf("\n");
#endif
    }
    return 0;
}

static krb5_error_code
krb5int_pbkdf2 (prf_func prf, size_t hlen, krb5_key pass,
		const krb5_data *salt, unsigned long count,
		const krb5_data *output)
{
    int l, r, i;
    char *utmp1, *utmp2;
    char utmp3[20];		/* XXX length shouldn't be hardcoded! */

    if (output->length == 0 || hlen == 0)
	abort();
    /* Step 1 & 2.  */
    if (output->length / hlen > 0xffffffff)
	abort();
    /* Step 2.  */
    l = (output->length + hlen - 1) / hlen;
    r = output->length - (l - 1) * hlen;

    utmp1 = /*output + dklen; */ malloc(hlen);
    if (utmp1 == NULL)
	return ENOMEM;
    utmp2 = /*utmp1 + hlen; */ malloc(salt->length + 4 + hlen);
    if (utmp2 == NULL) {
	free(utmp1);
	return ENOMEM;
    }

    /* Step 3.  */
    for (i = 1; i <= l; i++) {
#if 0
	int j;
#endif
	krb5_error_code err;
	char *out;

	if (i == l)
	    out = utmp3;
	else
	    out = output->data + (i-1) * hlen;
	err = F(out, utmp1, utmp2, prf, hlen, pass, salt, count, i);
	if (err) {
	    free(utmp1);
	    free(utmp2);
	    return err;
	}
	if (i == l)
	    memcpy(output->data + (i-1) * hlen, utmp3,
		   output->length - (i-1) * hlen);

#if 0
	printf("after F(%d), @%p:\n", i, output->data);
	for (j = (i-1) * hlen; j < i * hlen; j++)
	    printf(" %02x", 0xff & output->data[j]);
	printf ("\n");
#endif
    }
    free(utmp1);
    free(utmp2);
    return 0;
}

/*
 * Implements the hmac-sha1 PRF.  pass has been pre-hashed (if
 * necessary) and converted to a key already; salt has had the block
 * index appended to the original salt.
 */
static krb5_error_code
hmac_sha1(krb5_key pass, krb5_data *salt, krb5_data *out)
{
    const struct krb5_hash_provider *h = &krb5int_hash_sha1;
    krb5_error_code err;

    if (debug_hmac)
	printd(" hmac input", salt);
    err = krb5int_hmac(h, pass, 1, salt, out);
    if (err == 0 && debug_hmac)
	printd(" hmac output", out);
    return err;
}

krb5_error_code
krb5int_pbkdf2_hmac_sha1 (const krb5_data *out, unsigned long count,
			  const krb5_data *pass, const krb5_data *salt)
{
    const struct krb5_hash_provider *h = &krb5int_hash_sha1;
    krb5_keyblock keyblock;
    krb5_key key;
    char tmp[40];
    krb5_data d;
    krb5_error_code err;

    assert(h->hashsize <= sizeof(tmp));
    if (pass->length > h->blocksize) {
	d.data = tmp;
	d.length = h->hashsize;
	err = h->hash (1, pass, &d);
	if (err)
	    return err;
	keyblock.length = d.length;
	keyblock.contents = (krb5_octet *) d.data;
    } else {
	keyblock.length = pass->length;
	keyblock.contents = (krb5_octet *) pass->data;
    }

    err = krb5_k_create_key(NULL, &keyblock, &key);
    if (err)
	return err;

    err = krb5int_pbkdf2(hmac_sha1, 20, key, salt, count, out);
    krb5_k_free_key(NULL, key);
    return err;
}
