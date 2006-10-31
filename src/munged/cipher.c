/*****************************************************************************
 *  $Id$
 *****************************************************************************
 *  Copyright (C) 2002-2006 The Regents of the University of California.
 *  Produced at Lawrence Livermore National Laboratory.
 *  Written by Chris Dunlap <cdunlap@llnl.gov>.
 *  UCRL-CODE-155910.
 *
 *  This file is part of the MUNGE Uid 'N' Gid Emporium (MUNGE).
 *  For details, see <http://home.gna.org/munge/>.
 *
 *  This is free software; you can redistribute it and/or modify it
 *  under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2 of the License, or
 *  (at your option) any later version.
 *
 *  This is distributed in the hope that it will be useful, but WITHOUT
 *  ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 *  FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License
 *  for more details.
 *
 *  You should have received a copy of the GNU General Public License along
 *  with this program; if not, write to the Free Software Foundation, Inc.,
 *  51 Franklin Street, Fifth Floor, Boston, MA  02110-1301  USA.
 *****************************************************************************/


#if HAVE_CONFIG_H
#  include <config.h>
#endif /* HAVE_CONFIG_H */

#include <assert.h>
#include <munge.h>
#include <string.h>
#include "cipher.h"


/*****************************************************************************
 *  Constants
 *****************************************************************************/

#define CIPHER_MAGIC 0xDEADACE1


/*****************************************************************************
 *  Private Prototypes
 *****************************************************************************/

static int _cipher_init (cipher_ctx *x, munge_cipher_t cipher,
    unsigned char *key, unsigned char *iv, int enc);
static int _cipher_update (cipher_ctx *x, void *dst, int *dstlen,
    const void *src, int srclen);
static int _cipher_final (cipher_ctx *x, void *dst, int *dstlen);
static int _cipher_cleanup (cipher_ctx *x);
static int _cipher_block_size (munge_cipher_t cipher);
static int _cipher_iv_size (munge_cipher_t cipher);
static int _cipher_key_size (munge_cipher_t cipher);
static int _cipher_map_enum (munge_cipher_t cipher, void *dst);


/*****************************************************************************
 *  Public Functions
 *****************************************************************************/

int
cipher_init (cipher_ctx *x, munge_cipher_t cipher,
             unsigned char *key, unsigned char *iv, int enc)
{
    int rc;

    assert (x != NULL);
    assert (key != NULL);
    assert (iv != NULL);
    assert ((enc == 0) || (enc == 1));

    rc = _cipher_init (x, cipher, key, iv, enc);
    if (rc >= 0) {
        assert (x->magic = CIPHER_MAGIC);
        assert (!(x->finalized = 0));
    }
    return (rc);
}


int
cipher_update (cipher_ctx *x, void *dst, int *dstlen,
               const void *src, int srclen)
{
    int rc;

    assert (x != NULL);
    assert (x->magic == CIPHER_MAGIC);
    assert (x->finalized != 1);
    assert (dst != NULL);
    assert (dstlen != NULL);
    assert (src != NULL);

    if (srclen <= 0) {
        return (0);
    }
    rc = _cipher_update (x, dst, dstlen, src, srclen);
    return (rc);
}


int
cipher_final (cipher_ctx *x, void *dst, int *dstlen)
{
    int rc;

    assert (x != NULL);
    assert (x->magic == CIPHER_MAGIC);
    assert (x->finalized != 1);
    assert (dst != NULL);
    assert (dstlen != NULL);

    rc = _cipher_final (x, dst, dstlen);
    assert (x->finalized = 1);
    return (rc);
}


int
cipher_cleanup (cipher_ctx *x)
{
    int rc;

    assert (x != NULL);
    assert (x->magic == CIPHER_MAGIC);

    rc = _cipher_cleanup (x);
    memset (x, 0, sizeof (*x));
    assert (x->magic = ~CIPHER_MAGIC);
    return (rc);
}


int
cipher_block_size (munge_cipher_t cipher)
{
    return (_cipher_block_size (cipher));
}


int
cipher_iv_size (munge_cipher_t cipher)
{
    return (_cipher_iv_size (cipher));
}


int
cipher_key_size (munge_cipher_t cipher)
{
    return (_cipher_key_size (cipher));
}


int
cipher_map_enum (munge_cipher_t cipher, void *dst)
{
    return (_cipher_map_enum (cipher, dst));
}


/*****************************************************************************
 *  Private Functions (Libgcrypt)
 *****************************************************************************/

#if HAVE_LIBGCRYPT

#include <gcrypt.h>
#include <string.h>
#include "common.h"

static int _cipher_update_aux (cipher_ctx *x, void *dst, int *dstlen,
    const void *src, int srclen);

static int
_cipher_init (cipher_ctx *x, munge_cipher_t cipher,
              unsigned char *key, unsigned char *iv, int enc)
{
    int     algo;
    int     len;
    size_t *len_ptr = (size_t *) &len;

    if (_cipher_map_enum (cipher, &algo) < 0) {
        return (-1);
    }
    if (gcry_cipher_open (&(x->ctx), algo, GCRY_CIPHER_MODE_CBC, 0) != 0) {
        return (-1);
    }
    if (gcry_cipher_algo_info (algo, GCRYCTL_GET_KEYLEN, NULL, len_ptr) != 0) {
        return (-1);
    }
    if (gcry_cipher_setkey (x->ctx, key, len) != 0) {
        return (-1);
    }
    if (gcry_cipher_algo_info (algo, GCRYCTL_GET_BLKLEN, NULL, len_ptr) != 0) {
        return (-1);
    }
    if (gcry_cipher_setiv (x->ctx, iv, len) != 0) {
        return (-1);
    }
    x->do_encrypt = enc;
    x->len = 0;
    x->blklen = len;
    return (0);
}


static int
_cipher_update (cipher_ctx *x, void *vdst, int *dstlen,
                const void *vsrc, int srclen)
{
/*  During encryption, any remaining src data that is not a multiple of the
 *    cipher block size is saved in the context's partial block buffer.
 *    This buffer will be padded when the encryption is finalized
 *    (cf, PKCS #5, rfc2898).
 *  During decryption, the partial block buffer will always contain data at
 *    the end of each update to ensure the padding is properly removed when
 *    the decryption is finalized.
 */
    int            n;
    int            n_written;
    int            n_partial;
    int            n_complete;
    unsigned char *dst = vdst;
    unsigned char *src = vsrc;

    if (*dstlen < 0) {
        goto err;
    }
    n_written = 0;
    /*
     *  Continue processing a partial block if one exists.
     */
    if (x->len > 0) {
        assert (x->len <= x->blklen);
        n_partial = MIN (srclen, x->blklen - x->len);
        memcpy (&(x->buf[x->len]), src, n_partial);
        x->len += n_partial;
        src += n_partial;
        srclen -= n_partial;

        if (x->len == x->blklen) {
            if ((x->do_encrypt) || (srclen > 0)) {
                n = *dstlen;
                if (_cipher_update_aux (x, dst, &n, x->buf, x->blklen) < 0) {
                    goto err;
                }
                assert (n == x->blklen);
                dst += n;
                n_written += n;
                x->len = 0;
            }
        }
    }
    /*  Compute the number of bytes for complete blocks, and the remainder
     *    that will be saved in the partial block buffer.
     */
    n_partial = srclen % x->blklen;
    if ((!x->do_encrypt) && (n_partial == 0)) {
        n_partial = x->blklen;
    }
    n_complete = srclen - n_partial;

    /*  Process complete blocks.
     */
    if (n_complete > 0) {
        assert (x->len == 0);
        assert (n_complete % x->blklen == 0);
        n = *dstlen - n_written;
        if (_cipher_update_aux (x, dst, &n, src, n_complete) < 0) {
            goto err;
        }
        assert (n == n_complete);
        src += n;
        srclen -= n;
        n_written += n;
    }
    /*  Copy src leftovers to the partial block buf.
     */
    if (n_partial > 0) {
        assert (x->len == 0);
        assert (n_partial <= x->blklen);
        memcpy (x->buf, src, n_partial);
        x->len = n_partial;
    }
    /*  Ensure the partial block buffer is never empty during decryption.
     */
    if (!x->do_encrypt) {
        assert (x->len > 0);
    }
    /*  Set the number of bytes written.
     */
    *dstlen = n_written;
    return (0);

err:
    *dstlen = 0;
    return (-1);
}


static int
_cipher_update_aux (cipher_ctx *x, void *dst, int *dstlen_ptr,
                    const void *src, int srclen)
{
    gcry_error_t e;
    int          dstlen = *dstlen_ptr;

    if (x->do_encrypt) {
        e = gcry_cipher_encrypt (x->ctx, dst, dstlen, src, srclen);
    }
    else {
        e = gcry_cipher_decrypt (x->ctx, dst, dstlen, src, srclen);
    }
    if (e != 0) {
        *dstlen_ptr = 0;
        return (-1);
    }
    if ((src != NULL) || (srclen != 0)) {
        *dstlen_ptr = srclen;
    }
    return (0);
}


static int
_cipher_final (cipher_ctx *x, void *dst, int *dstlen)
{
    int n;
    int i;
    int pad;

    if (x->do_encrypt) {
        assert (x->len <= x->blklen);
        n = x->blklen - x->len;
        for (i = x->len; i < x->blklen; i++) {
            x->buf[i] = n;
        }
        if (_cipher_update_aux (x, dst, dstlen, x->buf, x->blklen) < 0) {
            return (-1);
        }
    }
    else {
        /*  Final cipher block should always be full due to padding.
         */
        if (x->len != x->blklen) {
            return (-1);
        }
        /*  Perform in-place decryption of final cipher block.
         */
        n = x->blklen;
        if (_cipher_update_aux (x, x->buf, &n, NULL, 0) < 0) {
            return (-1);
        }
        /*  Validate block padding.
         */
        pad = x->buf[x->blklen - 1];
        if ((pad <= 0) || (pad > x->blklen)) {
            return (-1);
        }
        for (i = x->blklen - pad; i < x->blklen; i++) {
            if (x->buf[i] != pad) {
                return (-1);
            }
        }
        n = x->blklen - pad;
        /*
         *  Copy decrypted plaintext to dst.
         */
        if (*dstlen < n) {
            return (-1);
        }
        memcpy (dst, x->buf, n);
        *dstlen = n;
    }
    return (0);
}


static int
_cipher_cleanup (cipher_ctx *x)
{
    gcry_cipher_close (x->ctx);
    return (0);
}


static int
_cipher_block_size (munge_cipher_t cipher)
{
    int    algo;
    size_t nbytes;

    if (_cipher_map_enum (cipher, &algo) < 0) {
        return (-1);
    }
    if (gcry_cipher_algo_info (algo, GCRYCTL_GET_BLKLEN, NULL, &nbytes) != 0) {
        return (-1);
    }
    return (nbytes);
}


static int
_cipher_iv_size (munge_cipher_t cipher)
{
    return (_cipher_block_size (cipher));
}


static int
_cipher_key_size (munge_cipher_t cipher)
{
    int    algo;
    size_t nbytes;

    if (_cipher_map_enum (cipher, &algo) < 0) {
        return (-1);
    }
    if (gcry_cipher_algo_info (algo, GCRYCTL_GET_KEYLEN, NULL, &nbytes) != 0) {
        return (-1);
    }
    return (nbytes);
}


static int
_cipher_map_enum (munge_cipher_t cipher, void *dst)
{
    int algo;
    int rc = 0;

    switch (cipher) {
        case MUNGE_CIPHER_BLOWFISH:
            algo = GCRY_CIPHER_BLOWFISH;
            break;
        case MUNGE_CIPHER_CAST5:
            algo = GCRY_CIPHER_CAST5;
            break;
        case MUNGE_CIPHER_AES128:
            algo = GCRY_CIPHER_AES128;
            break;
        case MUNGE_CIPHER_AES256:
            algo = GCRY_CIPHER_AES256;
            break;
        default:
            rc = -1;
            break;
    }
    if ((dst != NULL) && (rc == 0)) {
        * (int *) dst = algo;
    }
    return (rc);
}

#endif /* HAVE_LIBGCRYPT */


/*****************************************************************************
 *  Private Functions (OpenSSL)
 *****************************************************************************/
/*
 *  EVP_CipherInit() implicitly initializes the EVP_CIPHER_CTX.
 *    This call has been deprecated as of OpenSSL 0.9.7.
 *  EVP_CipherInit(), EVP_CipherUpdate(), and EVP_CIPHER_CTX_cleanup()
 *    return void in OpenSSL 0.9.5a and earlier versions, and int in later
 *    versions.  I'm using EVP_CipherInit_ex() as my test for this behavior.
 *    This probably isn't the best test since it fails for OpenSSL 0.9.6b.
 *    But this isn't as bad as it sounds since software versions of these
 *    functions will never return errors (unless there is a programming error),
 *    and hardware versions require the EVP_CipherInit_ex() interface provided
 *    by OpenSSL 0.9.7.
 *  If EVP_CipherInit_ex() exists, so should EVP_CIPHER_CTX_init().
 *    But EVP_CIPHER_CTX_cleanup() exists in the versions of which I'm aware.
 */

#if HAVE_OPENSSL

#include <openssl/evp.h>

static int
_cipher_init (cipher_ctx *x, munge_cipher_t cipher,
              unsigned char *key, unsigned char *iv, int enc)
{
    EVP_CIPHER *algo;

    if (_cipher_map_enum (cipher, &algo) < 0) {
        return (-1);
    }
#if HAVE_EVP_CIPHERINIT_EX
    EVP_CIPHER_CTX_init (&(x->ctx));
    if (!(EVP_CipherInit_ex (&(x->ctx), algo, NULL, key, iv, enc))) {
        return (-1);
    }
#else  /* !HAVE_EVP_CIPHERINIT_EX */
    EVP_CipherInit (&(x->ctx), algo, key, iv, enc);
#endif /* !HAVE_EVP_CIPHERINIT_EX */
    return (0);
}


static int
_cipher_update (cipher_ctx *x, void *dst, int *dstlen,
                const void *src, int srclen)
{
#if HAVE_EVP_CIPHERINIT_EX
    if (!(EVP_CipherUpdate (&(x->ctx), dst, dstlen, (void *) src, srclen))) {
        return (-1);
    }
#else  /* !HAVE_EVP_CIPHERINIT_EX */
    EVP_CipherUpdate (&(x->ctx), dst, dstlen, (void *) src, srclen);
#endif /* !HAVE_EVP_CIPHERINIT_EX */
    return (0);
}


static int
_cipher_final (cipher_ctx *x, void *dst, int *dstlen)
{
#if HAVE_EVP_CIPHERINIT_EX
    if (!(EVP_CipherFinal_ex (&(x->ctx), dst, dstlen))) {
        return (-1);
    }
#else  /* !HAVE_EVP_CIPHERINIT_EX */
    if (!(EVP_CipherFinal (&(x->ctx), dst, dstlen))) {
        return (-1);
    }
#endif /* !HAVE_EVP_CIPHERINIT_EX */
    return (0);
}


static int
_cipher_cleanup (cipher_ctx *x)
{
    int rc = 0;

#if HAVE_EVP_CIPHERINIT_EX
    if (!(EVP_CIPHER_CTX_cleanup (&(x->ctx)))) {
        rc = -1;
    }
#else  /* !HAVE_EVP_CIPHERINIT_EX */
    EVP_CIPHER_CTX_cleanup (&(x->ctx));
#endif /* !HAVE_EVP_CIPHERINIT_EX */
    return (rc);
}


static int
_cipher_block_size (munge_cipher_t cipher)
{
    EVP_CIPHER *algo;

    if (_cipher_map_enum (cipher, &algo) < 0) {
        return (-1);
    }
    return (EVP_CIPHER_block_size (algo));
}


static int
_cipher_iv_size (munge_cipher_t cipher)
{
    EVP_CIPHER *algo;

    if (_cipher_map_enum (cipher, &algo) < 0) {
        return (-1);
    }
    return (EVP_CIPHER_iv_length (algo));
}


static int
_cipher_key_size (munge_cipher_t cipher)
{
    EVP_CIPHER *algo;

    if (_cipher_map_enum (cipher, &algo) < 0) {
        return (-1);
    }
    return (EVP_CIPHER_key_length (algo));
}


static int
_cipher_map_enum (munge_cipher_t cipher, void *dst)
{
    EVP_CIPHER *algo;
    int         rc = 0;

    switch (cipher) {
        case MUNGE_CIPHER_BLOWFISH:
            algo = EVP_bf_cbc ();
            break;
        case MUNGE_CIPHER_CAST5:
            algo = EVP_cast5_cbc ();
            break;
#if HAVE_EVP_AES_128_CBC
        case MUNGE_CIPHER_AES128:
            algo = EVP_aes_128_cbc ();
            break;
#endif /* HAVE_EVP_AES_128_CBC */
#if HAVE_EVP_AES_256_CBC
        case MUNGE_CIPHER_AES256:
            algo = EVP_aes_256_cbc ();
            break;
#endif /* HAVE_EVP_AES_256_CBC */
        default:
            rc = -1;
            break;
    }
    if ((dst != NULL) && (rc == 0)) {
        * (EVP_CIPHER **) dst = algo;
    }
    return (rc);
}

#endif /* HAVE_OPENSSL */
