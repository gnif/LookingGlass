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

#include <openssl/rsa.h>
#include <openssl/evp.h>
#include <openssl/x509.h>

bool spice_rsa_encrypt_password(uint8_t * pub_key, char * password, struct spice_password * result)
{
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
}

void spice_rsa_free_password(struct spice_password * pass)
{
  free(pass->data);
  pass->size = 0;
  pass->data = NULL;
}