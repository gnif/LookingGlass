/**
 * Looking Glass
 * Copyright © 2017-2026 The Looking Glass Authors
 * https://looking-glass.io
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the Free
 * Software Foundation; either version 2 of the License, or (at your option)
 * any later version.
 *
 * This program is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE. See the GNU General Public License for
 * more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with this program; if not, write to the Free Software Foundation, Inc., 59
 * Temple Place, Suite 330, Boston, MA 02111-1307 USA
 */

#define WIN32_LEAN_AND_MEAN

#include "NetworkIdentity.h"

#include <LGProtocol/NetworkIdentity.h>

#include <windows.h>
#include <ncrypt.h>
#include <sddl.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wincrypt.h>

#define LGIDD_REGKEY                          L"Software\\LookingGlass\\IDD"
#define LG_NETWORK_CERTIFICATE_LIFETIME_YEARS 30

static void networkIdentityError(const wchar_t *operation, DWORD status)
{
  fwprintf(stderr, L"%s failed with code 0x%08lx\n", operation, status);
}

static bool networkIdentitySetKeySecurity(NCRYPT_KEY_HANDLE key)
{
  static const wchar_t securityDescriptor[] =
    L"D:P(A;;GA;;;SY)(A;;GA;;;BA)(A;;GA;;;LS)"
    L"(A;;GA;;;S-1-5-84-0-0-0-0-0)";

  PSECURITY_DESCRIPTOR descriptor = NULL;
  if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(
      securityDescriptor, SDDL_REVISION_1, &descriptor, NULL))
  {
    networkIdentityError(
      L"ConvertStringSecurityDescriptorToSecurityDescriptorW",
      GetLastError());
    return false;
  }

  const SECURITY_STATUS status = NCryptSetProperty(
    key,
    NCRYPT_SECURITY_DESCR_PROPERTY,
    (PBYTE)descriptor,
    GetSecurityDescriptorLength(descriptor),
    DACL_SECURITY_INFORMATION | NCRYPT_PERSIST_FLAG);
  LocalFree(descriptor);

  if (status != ERROR_SUCCESS)
  {
    networkIdentityError(
      L"NCryptSetProperty(key security)", (DWORD)status);
    return false;
  }

  return true;
}

static bool networkIdentityOpenKey(
  NCRYPT_PROV_HANDLE *provider,
  NCRYPT_KEY_HANDLE  *key)
{
  bool            retry  = false;
  SECURITY_STATUS status = NCryptOpenStorageProvider(
    provider, MS_KEY_STORAGE_PROVIDER, 0);
  if (status != ERROR_SUCCESS)
  {
    networkIdentityError(L"NCryptOpenStorageProvider", (DWORD)status);
    return false;
  }

openKey:
  status = NCryptOpenKey(*provider, key,
    LG_NETWORK_CERTIFICATE_KEY_NAME, 0, NCRYPT_MACHINE_KEY_FLAG);
  if (status == NTE_BAD_KEYSET && !retry)
  {
    status = NCryptCreatePersistedKey(
      *provider,
      key,
      NCRYPT_ECDSA_P256_ALGORITHM,
      LG_NETWORK_CERTIFICATE_KEY_NAME,
      0,
      NCRYPT_MACHINE_KEY_FLAG);
    if (status == ERROR_SUCCESS)
    {
      const DWORD usage = NCRYPT_ALLOW_SIGNING_FLAG;
      status = NCryptSetProperty(
        *key,
        NCRYPT_KEY_USAGE_PROPERTY,
        (PBYTE)&usage,
        sizeof usage,
        0);
      if (status == ERROR_SUCCESS)
        status = NCryptFinalizeKey(*key, 0);
    }
    else if (status == NTE_EXISTS)
    {
      retry = true;
      goto openKey;
    }
  }

  if (status != ERROR_SUCCESS)
  {
    networkIdentityError(
      L"NCryptOpenKey/NCryptCreatePersistedKey", (DWORD)status);
    NCryptFreeObject(*provider);
    *provider = 0;
    return false;
  }

  if (!networkIdentitySetKeySecurity(*key))
  {
    NCryptFreeObject(*key);
    NCryptFreeObject(*provider);
    *key      = 0;
    *provider = 0;
    return false;
  }

  return true;
}

static bool networkIdentityCanUsePrivateKey(PCCERT_CONTEXT certificate)
{
  HCRYPTPROV_OR_NCRYPT_KEY_HANDLE key        = 0;
  DWORD                           keySpec    = 0;
  BOOL                            callerFree = FALSE;
  const DWORD                     flags      =
    CRYPT_ACQUIRE_COMPARE_KEY_FLAG     |
    CRYPT_ACQUIRE_ONLY_NCRYPT_KEY_FLAG |
    CRYPT_ACQUIRE_SILENT_FLAG;
  const bool acquired = CryptAcquireCertificatePrivateKey(
    certificate,
    flags,
    NULL,
    &key,
    &keySpec,
    &callerFree);
  const bool result   = acquired &&
    key &&
    keySpec == CERT_NCRYPT_KEY_SPEC;

  if (callerFree && key)
  {
    if (keySpec == CERT_NCRYPT_KEY_SPEC)
      NCryptFreeObject((NCRYPT_HANDLE)key);
    else
      CryptReleaseContext((HCRYPTPROV)key, 0);
  }

  return result;
}

static bool networkIdentityRepairKeyProvider(
  PCCERT_CONTEXT      certificate,
  CRYPT_KEY_PROV_INFO *info)
{
  const DWORD flags = NCRYPT_MACHINE_KEY_FLAG | NCRYPT_SILENT_FLAG;
  if (info->dwFlags == flags && info->dwKeySpec == 0)
    return true;

  /* This CNG key has no legacy CAPI key specification. */
  info->dwFlags   = flags;
  info->dwKeySpec = 0;
  if (!CertSetCertificateContextProperty(
      certificate,
      CERT_KEY_PROV_INFO_PROP_ID,
      0,
      info))
  {
    networkIdentityError(
      L"CertSetCertificateContextProperty(key provider)",
      GetLastError());
    return false;
  }

  return true;
}

static bool networkIdentityMatchesKey(PCCERT_CONTEXT certificate)
{
  DWORD size = 0;
  if (!CertGetCertificateContextProperty(
      certificate, CERT_KEY_PROV_INFO_PROP_ID, NULL, &size))
    return false;

  CRYPT_KEY_PROV_INFO *info = malloc(size);
  if (!info)
    return false;

  bool result = CertGetCertificateContextProperty(
      certificate, CERT_KEY_PROV_INFO_PROP_ID, info, &size) &&
    info->pwszContainerName &&
    wcscmp(info->pwszContainerName,
      LG_NETWORK_CERTIFICATE_KEY_NAME) == 0 &&
    info->pwszProvName &&
    wcscmp(info->pwszProvName, MS_KEY_STORAGE_PROVIDER) == 0 &&
    info->dwProvType == 0 &&
    CertVerifyTimeValidity(NULL, certificate->pCertInfo) == 0;
  if (result)
    result = networkIdentityRepairKeyProvider(certificate, info) &&
      networkIdentityCanUsePrivateKey(certificate);

  free(info);
  return result;
}

static PCCERT_CONTEXT networkIdentityFindCertificate(HCERTSTORE store)
{
  PCCERT_CONTEXT certificate = NULL;
  while ((certificate = CertEnumCertificatesInStore(store, certificate)))
    if (networkIdentityMatchesKey(certificate))
      return certificate;

  return NULL;
}

static bool networkIdentityEncodeExtension(
  LPCSTR          objectId,
  LPCSTR          structureType,
  const void     *value,
  bool            critical,
  CERT_EXTENSION *extension)
{
  extension->pszObjId  = (LPSTR)objectId;
  extension->fCritical = critical;

  if (!CryptEncodeObjectEx(
      X509_ASN_ENCODING,
      structureType,
      value,
      CRYPT_ENCODE_ALLOC_FLAG,
      NULL,
      &extension->Value.pbData,
      &extension->Value.cbData))
  {
    networkIdentityError(L"CryptEncodeObjectEx", GetLastError());
    return false;
  }

  return true;
}

static void networkIdentityFreeExtensions(
  CERT_EXTENSION *extensions,
  size_t          count)
{
  for (size_t i = 0; i < count; ++i)
    if (extensions[i].Value.pbData)
      LocalFree(extensions[i].Value.pbData);
}

static PCCERT_CONTEXT networkIdentityCreateCertificate(
  HCERTSTORE        store,
  NCRYPT_KEY_HANDLE key)
{
  BYTE  *encodedName     = NULL;
  DWORD  encodedNameSize = 0;
  PCCERT_CONTEXT result  = NULL;

  if (!CertStrToNameW(
      X509_ASN_ENCODING,
      LG_NETWORK_CERTIFICATE_SUBJECT,
      CERT_X500_NAME_STR,
      NULL,
      NULL,
      &encodedNameSize,
      NULL))
  {
    networkIdentityError(L"CertStrToNameW(size)", GetLastError());
    return NULL;
  }

  encodedName = malloc(encodedNameSize);
  if (!encodedName)
  {
    networkIdentityError(L"malloc(certificate name)", ERROR_OUTOFMEMORY);
    return NULL;
  }

  if (!CertStrToNameW(
      X509_ASN_ENCODING,
      LG_NETWORK_CERTIFICATE_SUBJECT,
      CERT_X500_NAME_STR,
      NULL,
      encodedName,
      &encodedNameSize,
      NULL))
  {
    networkIdentityError(L"CertStrToNameW", GetLastError());
    goto cleanup;
  }

  CERT_BASIC_CONSTRAINTS2_INFO constraints =
  {
    .fCA                 = FALSE,
    .fPathLenConstraint  = FALSE,
    .dwPathLenConstraint = 0,
  };
  LPSTR             usages[] = { szOID_PKIX_KP_SERVER_AUTH };
  CERT_ENHKEY_USAGE enhancedKeyUsage =
  {
    .cUsageIdentifier     = ARRAYSIZE(usages),
    .rgpszUsageIdentifier = usages,
  };
  BYTE           keyUsageByte = CERT_DIGITAL_SIGNATURE_KEY_USAGE;
  CRYPT_BIT_BLOB keyUsage =
  {
    .cbData      = sizeof keyUsageByte,
    .pbData      = &keyUsageByte,
    .cUnusedBits = 0,
  };
  CERT_EXTENSION extensions[3] = { 0 };
  if (!networkIdentityEncodeExtension(
      szOID_BASIC_CONSTRAINTS2,
      X509_BASIC_CONSTRAINTS2,
      &constraints,
      true,
      &extensions[0]) ||
      !networkIdentityEncodeExtension(
        szOID_ENHANCED_KEY_USAGE,
        X509_ENHANCED_KEY_USAGE,
        &enhancedKeyUsage,
        false,
        &extensions[1]) ||
      !networkIdentityEncodeExtension(
        szOID_KEY_USAGE,
        X509_KEY_USAGE,
        &keyUsage,
        true,
        &extensions[2]))
  {
    networkIdentityFreeExtensions(extensions, ARRAYSIZE(extensions));
    goto cleanup;
  }

  CERT_EXTENSIONS certificateExtensions =
  {
    .cExtension  = (DWORD)ARRAYSIZE(extensions),
    .rgExtension = extensions,
  };
  CRYPT_KEY_PROV_INFO keyProvider =
  {
    .pwszContainerName = LG_NETWORK_CERTIFICATE_KEY_NAME,
    .pwszProvName      = MS_KEY_STORAGE_PROVIDER,
    .dwProvType        = 0,
    .dwFlags           = NCRYPT_MACHINE_KEY_FLAG | NCRYPT_SILENT_FLAG,
    .cProvParam        = 0,
    .rgProvParam       = NULL,
    .dwKeySpec         = 0,
  };
  CRYPT_ALGORITHM_IDENTIFIER signature =
  {
    .pszObjId = szOID_ECDSA_SHA256,
  };
  CERT_NAME_BLOB subject =
  {
    .cbData = encodedNameSize,
    .pbData = encodedName,
  };
  SYSTEMTIME start;
  SYSTEMTIME end;
  FILETIME   expiration;
  GetSystemTime(&start);
  if (!SystemTimeToFileTime(&start, &expiration))
  {
    networkIdentityError(L"SystemTimeToFileTime", GetLastError());
    networkIdentityFreeExtensions(extensions, ARRAYSIZE(extensions));
    goto cleanup;
  }
  ULARGE_INTEGER expirationValue;
  expirationValue.LowPart  = expiration.dwLowDateTime;
  expirationValue.HighPart = expiration.dwHighDateTime;
  expirationValue.QuadPart +=
    (ULONGLONG)LG_NETWORK_CERTIFICATE_LIFETIME_YEARS * 365U * 24U *
    60U * 60U * 10000000U;
  expiration.dwLowDateTime  = expirationValue.LowPart;
  expiration.dwHighDateTime = expirationValue.HighPart;
  if (!FileTimeToSystemTime(&expiration, &end))
  {
    networkIdentityError(L"FileTimeToSystemTime", GetLastError());
    networkIdentityFreeExtensions(extensions, ARRAYSIZE(extensions));
    goto cleanup;
  }

  PCCERT_CONTEXT certificate = CertCreateSelfSignCertificate(
    (HCRYPTPROV_OR_NCRYPT_KEY_HANDLE)key,
    &subject,
    0,
    &keyProvider,
    &signature,
    &start,
    &end,
    &certificateExtensions);
  networkIdentityFreeExtensions(extensions, ARRAYSIZE(extensions));
  if (!certificate)
  {
    networkIdentityError(L"CertCreateSelfSignCertificate", GetLastError());
    goto cleanup;
  }

  CRYPT_DATA_BLOB friendlyName =
  {
    .cbData = (DWORD)sizeof LG_NETWORK_CERTIFICATE_FRIENDLY_NAME,
    .pbData = (BYTE *)LG_NETWORK_CERTIFICATE_FRIENDLY_NAME,
  };
  if (!CertSetCertificateContextProperty(
      certificate,
      CERT_FRIENDLY_NAME_PROP_ID,
      0,
      &friendlyName))
  {
    networkIdentityError(
      L"CertSetCertificateContextProperty(friendly name)",
      GetLastError());
    CertFreeCertificateContext(certificate);
    goto cleanup;
  }

  if (!CertAddCertificateContextToStore(
      store,
      certificate,
      CERT_STORE_ADD_REPLACE_EXISTING,
      &result))
  {
    networkIdentityError(L"CertAddCertificateContextToStore", GetLastError());
  }
  CertFreeCertificateContext(certificate);

cleanup:
  free(encodedName);
  return result;
}

static bool networkIdentityStoreHashes(PCCERT_CONTEXT certificate)
{
  BYTE  certificateHash[20];
  DWORD certificateHashSize = sizeof certificateHash;
  if (!CertGetCertificateContextProperty(
      certificate,
      CERT_HASH_PROP_ID,
      certificateHash,
      &certificateHashSize))
  {
    networkIdentityError(
      L"CertGetCertificateContextProperty(CERT_HASH_PROP_ID)",
      GetLastError());
    return false;
  }

  BYTE  fingerprint[32];
  DWORD fingerprintSize = sizeof fingerprint;
  if (!CryptHashCertificate2(
      L"SHA256",
      0,
      NULL,
      certificate->pbCertEncoded,
      certificate->cbCertEncoded,
      fingerprint,
      &fingerprintSize))
  {
    networkIdentityError(L"CryptHashCertificate2(SHA256)", GetLastError());
    return false;
  }

  HKEY key = NULL;
  const LSTATUS openStatus = RegOpenKeyExW(
    HKEY_LOCAL_MACHINE,
    LGIDD_REGKEY,
    0,
    KEY_SET_VALUE | KEY_WOW64_64KEY,
    &key);
  if (openStatus != ERROR_SUCCESS)
  {
    networkIdentityError(
      L"RegOpenKeyExW(network identity)", (DWORD)openStatus);
    return false;
  }

  LSTATUS status = RegSetValueExW(
    key,
    LG_NETWORK_CERTIFICATE_HASH_VALUE,
    0,
    REG_BINARY,
    certificateHash,
    certificateHashSize);
  if (status == ERROR_SUCCESS)
  {
    status = RegSetValueExW(
      key,
      LG_NETWORK_CERTIFICATE_FINGERPRINT_VALUE,
      0,
      REG_BINARY,
      fingerprint,
      fingerprintSize);
  }
  RegCloseKey(key);

  if (status != ERROR_SUCCESS)
  {
    networkIdentityError(
      L"RegSetValueExW(network identity)", (DWORD)status);
    return false;
  }

  wprintf(L"Network identity SHA256 fingerprint: ");
  for (DWORD i = 0; i < fingerprintSize; ++i)
  {
    if (i)
      wprintf(L":");
    wprintf(L"%02X", (unsigned)fingerprint[i]);
  }
  wprintf(L"\n");
  return true;
}

bool networkIdentityProvision(void)
{
  bool                result      = false;
  NCRYPT_PROV_HANDLE  provider    = 0;
  NCRYPT_KEY_HANDLE   key         = 0;
  HCERTSTORE          store       = NULL;
  PCCERT_CONTEXT      certificate = NULL;

  if (!networkIdentityOpenKey(&provider, &key))
    goto cleanup;

  store = CertOpenStore(
    CERT_STORE_PROV_SYSTEM_W,
    0,
    0,
    CERT_SYSTEM_STORE_LOCAL_MACHINE | CERT_STORE_OPEN_EXISTING_FLAG,
    L"MY");
  if (!store)
  {
    networkIdentityError(L"CertOpenStore(LocalMachine\\MY)", GetLastError());
    goto cleanup;
  }

  certificate = networkIdentityFindCertificate(store);
  if (!certificate)
    certificate = networkIdentityCreateCertificate(store, key);
  if (!certificate)
    goto cleanup;

  result = networkIdentityStoreHashes(certificate);

cleanup:
  if (certificate)
    CertFreeCertificateContext(certificate);
  if (store)
    CertCloseStore(store, 0);
  if (key)
    NCryptFreeObject(key);
  if (provider)
    NCryptFreeObject(provider);
  return result;
}
