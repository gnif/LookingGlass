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
#include <string.h>

#if defined(USE_OPENSSL) && defined(USE_GNUTLS)
  #error "USE_OPENSSL and USE_GNUTLS are both defined"
#elif !defined(USE_OPENSSL) && !defined(USE_GNUTLS)
  #error "One of USE_OPENSSL or USE_GNUTLS must be defined"
#endif

#if defined(USE_OPENSSL)
#include <openssl/rsa.h>
#include <openssl/evp.h>
#include <openssl/x509.h>
#endif

#if defined(USE_GNUTLS)
#include <gnutls/gnutls.h>
#include <gnutls/abstract.h>
#endif

bool spice_rsa_encrypt_password(uint8_t * pub_key, char * password, struct spice_password * result)
{
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

#if defined(USE_GNUTLS)
  const gnutls_datum_t pubData =
  {
    .data = (void *)reply.pub_key,
    .size = SPICE_TICKET_PUBKEY_BYTES
  };

  gnutls_pubkey_t pubkey;
  if (gnutls_pubkey_init(&pubkey) < 0)
  {
    DEBUG_ERROR("gnutls_pubkey_init failed");
    return false;
  }

  if (gnutls_pubkey_import(pubkey, &pubData, GNUTLS_X509_FMT_DER) < 0)
  {
    gnutls_pubkey_deinit(pubkey);
    DEBUG_ERROR("gnutls_pubkey_import failed");
    return false;
  }

  const gnutls_datum_t input =
  {
    .data = (void *)spice.password,
    .size = strlen(spice.password) + 1
  };

  gnutls_datum_t out;
  if (gnutls_pubkey_encrypt_data(pubkey, 0, &input, &out) < 0)
  {
    gnutls_pubkey_deinit(pubkey);
    DEBUG_ERROR("gnutls_pubkey_encrypt_data failed");
    return false;
  }

  result->size = out.size;
  result->data = out.data;

  gnutls_pubkey_deinit(pubkey);
  return true;
#endif
}

void spice_rsa_free_password(struct spice_password * pass)
{
#if defined(USE_OPENSSL)
  free(pass->data);
#endif

#if defined(USE_GNUTLS)
  gnutls_free(pass->data);
#endif

  pass->size = 0;
  pass->data = NULL;
}