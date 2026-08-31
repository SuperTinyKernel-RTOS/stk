#ifndef PSA_CRYPTO_CONFIG_H
#define PSA_CRYPTO_CONFIG_H

#include "mbedtls_config.h"

/* AEAD & Ciphers */
#define PSA_WANT_ALG_GCM                        1
#define PSA_WANT_KEY_TYPE_AES                   1

/* Hashes */
#define PSA_WANT_ALG_SHA_1                      1
#define PSA_WANT_ALG_SHA_256                    1
#define PSA_WANT_ALG_SHA_384                    1
#define PSA_WANT_ALG_SHA_512                    1
#define PSA_WANT_ALG_HMAC                       1

/* Asymmetric & Key Exchange */
#define PSA_WANT_ALG_ECDH                       1
#define PSA_WANT_ALG_ECDSA                      1
#define PSA_WANT_KEY_TYPE_ECC_KEY_PAIR          1
#define PSA_WANT_KEY_TYPE_ECC_PUBLIC_KEY        1
#define PSA_WANT_ECC_FAMILY_SECP_R1             1
#define PSA_WANT_ECC_SECP_R1_256                1
#define PSA_WANT_ECC_SECP_R1_384                1
#define PSA_WANT_ECC_SECP_R1_521                1

/* Key Derivation & TLS 1.3 Key Schedule */
#define PSA_WANT_ALG_HKDF                       1
#define PSA_WANT_ALG_HKDF_EXTRACT               1
#define PSA_WANT_ALG_HKDF_EXPAND                1
#define PSA_WANT_ALG_TLS12_PRF                  1

/* Random Number Generation */
#define PSA_WANT_ALG_DETERMINISTIC_ECDSA        1

#define PSA_CRYPTO_MBED_TLS_DRIVER_ID (1)

/* RSA Support */
#define PSA_WANT_KEY_TYPE_RSA_KEY_PAIR_BASIC   1
#define PSA_WANT_KEY_TYPE_RSA_KEY_PAIR         1
#define PSA_WANT_KEY_TYPE_RSA_PUBLIC_KEY       1
#define PSA_WANT_ALG_RSA_PKCS1V15_SIGN         1
#define PSA_WANT_ALG_RSA_PKCS1V15_CRYPT        1
#define PSA_WANT_ALG_RSA_OAEP                  1
#define PSA_WANT_ALG_RSA_PSS                   1
#define PSA_WANT_KEY_TYPE_RSA_KEY_PAIR_IMPORT  1
#define PSA_WANT_KEY_TYPE_RSA_KEY_PAIR_EXPORT  1

/* ECC Support */
#define PSA_WANT_KEY_TYPE_ECC_KEY_PAIR_BASIC   1
#define PSA_WANT_KEY_TYPE_ECC_KEY_PAIR_IMPORT  1
#define PSA_WANT_KEY_TYPE_ECC_KEY_PAIR_EXPORT  1
#define PSA_WANT_KEY_TYPE_ECC_KEY_PAIR_DERIVE  1
#define PSA_WANT_KEY_TYPE_ECC_KEY_PAIR_GENERATE 1

#endif /* PSA_CRYPTO_CONFIG_H */
