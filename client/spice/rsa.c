/*
Looking Glass - KVM FrameRelay (KVMFR) Client
Copyright (C) 2017 Geoffrey McRae <geoff@hostfission.com>
https://looking-glass.hostfission.com

This program is free software; you can redistribute it and/or modify it under
the terms of the GNU General Public License as published by the Free Software
Foundation; either version 2 of the License, or (at your option) any later
version.

This program is distributed in the hope that it will be useful, but WITHOUT ANY
WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR A
PARTICULAR PURPOSE. See the GNU General Public License for more details.

You should have received a copy of the GNU General Public License along with
this program; if not, write to the Free Software Foundation, Inc., 59 Temple
Place, Suite 330, Boston, MA 02111-1307 USA
*/

#include "rsa.h"
#include "debug.h"

#include <spice/protocol.h>
#include <malloc.h>
#include <string.h>

#if defined(USE_OPENSSL) && defined(USE_NETTLE)
  #error "USE_OPENSSL and USE_NETTLE are both defined"
#elif !defined(USE_OPENSSL) && !defined(USE_NETTLE)
  #error "One of USE_OPENSSL or USE_NETTLE must be defined"
#endif

#if defined(USE_OPENSSL)
#include <openssl/rsa.h>
#include <openssl/evp.h>
#include <openssl/x509.h>
#endif

#if defined(USE_NETTLE)
#include <stdlib.h>
#include <nettle/asn1.h>
#include <nettle/sha1.h>
#include <nettle/rsa.h>
#include <nettle/bignum.h>
#include <gmp.h>

#define SHA1_HASH_LEN 20
#endif

#if defined(USE_NETTLE)
/* the below OAEP implementation is derived from the FreeTDS project */
static void memxor(uint8_t * a, const uint8_t * b, const unsigned int len)
{
  for(unsigned int i = 0; i < len; ++i)
    a[i] = a[i] ^ b[i];
}

static void sha1(uint8_t * hash, const uint8_t *data, unsigned int len)
{
  struct sha1_ctx ctx;

  sha1_init(&ctx);
  sha1_update(&ctx, len, data);
  sha1_digest(&ctx, SHA1_HASH_LEN, hash);
}

static void oaep_mask(uint8_t * dest, size_t dest_len, const uint8_t * mask, size_t mask_len)
{
  uint8_t hash[SHA1_HASH_LEN];
  uint8_t seed[mask_len + 4 ];
  memcpy(seed, mask, mask_len);

  for(unsigned int n = 0;; ++n)
  {
    (seed+mask_len)[0] = n >> 24;
    (seed+mask_len)[1] = n >> 16;
    (seed+mask_len)[2] = n >> 8;
    (seed+mask_len)[3] = n >> 0;

    sha1(hash, seed, sizeof(seed));
    if (dest_len <= SHA1_HASH_LEN)
    {
      memxor(dest, hash, dest_len);
      break;
    }

    memxor(dest, hash, SHA1_HASH_LEN);
    dest     += SHA1_HASH_LEN;
    dest_len -= SHA1_HASH_LEN;
  }
}

static bool oaep_pad(mpz_t m, unsigned int key_size, const uint8_t * message, unsigned int len)
{
  if (len + SHA1_HASH_LEN * 2 + 2 > key_size)
  {
    DEBUG_ERROR("Message too long");
    return false;
  }

  struct
  {
    uint8_t all[1];
    uint8_t ros[SHA1_HASH_LEN];
    uint8_t db [key_size - SHA1_HASH_LEN - 1];
  } em;

  memset(&em, 0, sizeof(em));
  sha1(em.db, (uint8_t *)"", 0);
  em.all[key_size - len - 1] = 0x1;
  memcpy(em.all + (key_size - len), message, len);

  /* we are not too worried about randomness since we are just making a local
   * connection, should anyone use this code outside of LookingGlass please be
   * sure to use something better such as `gnutls_rnd` */
  for(int i = 0; i < SHA1_HASH_LEN; ++i)
    em.ros[i] = rand() % 255;

  const int db_len = key_size - SHA1_HASH_LEN - 1;
  oaep_mask(em.db , db_len       , em.ros, SHA1_HASH_LEN);
  oaep_mask(em.ros, SHA1_HASH_LEN, em.db , db_len       );

  nettle_mpz_set_str_256_u(m, key_size, em.all);
  return true;
}
#endif

bool spice_rsa_encrypt_password(uint8_t * pub_key, char * password, struct spice_password * result)
{
  result->size = 0;
  result->data = NULL;

#if defined(USE_OPENSSL)
  BIO *bioKey = BIO_new(BIO_s_mem());
  if (!bioKey)
  {
    DEBUG_ERROR("failed to allocate bioKey");
    return false;
  }

  BIO_write(bioKey, pub_key, SPICE_TICKET_PUBKEY_BYTES);
  EVP_PKEY *rsaKey = d2i_PUBKEY_bio(bioKey, NULL);
  RSA *rsa = EVP_PKEY_get1_RSA(rsaKey);

  result->size = RSA_size(rsa);
  result->data = (char *)malloc(result->size);

  if (RSA_public_encrypt(
        strlen(password) + 1,
        (uint8_t*)password,
        (uint8_t*)result->data,
        rsa,
        RSA_PKCS1_OAEP_PADDING
  ) <= 0)
  {
    free(result->data);
    result->size = 0;
    result->data = NULL;

    DEBUG_ERROR("rsa public encrypt failed");
    EVP_PKEY_free(rsaKey);
    BIO_free(bioKey);
    return false;
  }

  EVP_PKEY_free(rsaKey);
  BIO_free(bioKey);
  return true;
#endif

#if defined(USE_NETTLE)
  struct asn1_der_iterator der;
  struct asn1_der_iterator j;
  struct rsa_public_key    pub;

  if (asn1_der_iterator_first(&der, SPICE_TICKET_PUBKEY_BYTES, pub_key) == ASN1_ITERATOR_CONSTRUCTED
      && der.type == ASN1_SEQUENCE
      && asn1_der_decode_constructed_last(&der) == ASN1_ITERATOR_CONSTRUCTED
      && der.type == ASN1_SEQUENCE
      && asn1_der_decode_constructed(&der, &j) == ASN1_ITERATOR_PRIMITIVE
      && j.type == ASN1_IDENTIFIER
      && asn1_der_iterator_next(&der) == ASN1_ITERATOR_PRIMITIVE
      && der.type == ASN1_BITSTRING
      && asn1_der_decode_bitstring_last(&der))
  {
    if (j.length != 9)
    {
      DEBUG_ERROR("Invalid key, not RSA");
      return false;
    }

    if (asn1_der_iterator_next(&j) == ASN1_ITERATOR_PRIMITIVE
        && j.type == ASN1_NULL
        && j.length == 0
        && asn1_der_iterator_next(&j) == ASN1_ITERATOR_END)
    {
      rsa_public_key_init(&pub);
      if (!rsa_public_key_from_der_iterator(&pub, 0, &der))
      {
        DEBUG_ERROR("Unable to load public key from DER iterator");
        rsa_public_key_clear(&pub);
        return false;
      }
    }
  }

  mpz_t p;
  mpz_init(p);
  oaep_pad(p, pub.size, (uint8_t *)password, strlen(password)+1);
  mpz_powm(p, p, pub.e, pub.n);

  result->size = pub.size;
  result->data = malloc(pub.size);
  nettle_mpz_get_str_256(pub.size, (uint8_t *)result->data, p);

  rsa_public_key_clear(&pub);
  mpz_clear(p);
  return true;
#endif
}

void spice_rsa_free_password(struct spice_password * pass)
{
  free(pass->data);
  pass->size = 0;
  pass->data = NULL;
}