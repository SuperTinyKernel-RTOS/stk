#ifndef MBEDTLS_CONFIG_H
#define MBEDTLS_CONFIG_H

/* ========================================================================= */
/* System & Architecture Options                                            */
/* ========================================================================= */
/* Disables inline assembly to fix GCC register allocation errors on M33 */
#undef MBEDTLS_HAVE_ASM
#define MBEDTLS_PLATFORM_C
#define MBEDTLS_PLATFORM_MEMORY

/* Memory reduction: Downsize TLS buffers to 4KB (Saves ~24KB RAM) */
#define MBEDTLS_SSL_IN_CONTENT_LEN              (4096*2)
#define MBEDTLS_SSL_OUT_CONTENT_LEN             (4096*2)

/* Enable Maximum Fragment Length negotiation for constrained devices */
#define MBEDTLS_SSL_MAX_FRAGMENT_LENGTH

/* ========================================================================= */
/* TLS Protocol Support                                                      */
/* ========================================================================= */
#define MBEDTLS_SSL_TLS_C
#define MBEDTLS_SSL_CLI_C
#define MBEDTLS_SSL_SRV_C
#define MBEDTLS_SSL_PROTO_TLS1_3
#define MBEDTLS_SSL_PROTO_TLS1_2

/* TLS 1.3 Key Exchange Mode (Required for TLS 1.3 Certificate Authentication) */
#define MBEDTLS_SSL_TLS1_3_KEY_EXCHANGE_MODE_EPHEMERAL_ENABLED

/* Key Exchange Modes for TLS 1.2 & Extensions */
#define MBEDTLS_KEY_EXCHANGE_ECDHE_ECDSA_ENABLED
#define MBEDTLS_KEY_EXCHANGE_ECDHE_RSA_ENABLED
#define MBEDTLS_KEY_EXCHANGE_RSA_ENABLED
#define MBEDTLS_KEY_EXCHANGE_ECDSA_CERT_REQ_ANY_ALLOWED_ENABLED

#define MBEDTLS_SSL_SERVER_NAME_INDICATION
#define MBEDTLS_SSL_ALPN
#define MBEDTLS_SSL_KEEP_PEER_CERTIFICATE
#define MBEDTLS_SSL_RENEGOTIATION
#define MBEDTLS_SSL_HANDSHAKE_WITH_CERT_ENABLED

/* ========================================================================= */
/* Cryptographic Algorithms (Dual RSA + ECC Stack)                           */
/* ========================================================================= */
/* Symmetric Ciphers */
#define MBEDTLS_AES_C
#define MBEDTLS_CIPHER_C
#define MBEDTLS_GCM_C

/* Elliptic Curves & Asymmetric */
#define MBEDTLS_ECP_C
#define MBEDTLS_ECDH_C
#define MBEDTLS_ECDSA_C
#define MBEDTLS_ECP_DP_SECP256R1_ENABLED    /* NIST P-256 / prime256v1 */
#define MBEDTLS_BIGNUM_C
#define MBEDTLS_OID_C
#define MBEDTLS_PK_C
#define MBEDTLS_PK_PARSE_C

/* RSA Cryptography */
#define MBEDTLS_RSA_C
#define MBEDTLS_PKCS1_V15
#define MBEDTLS_PKCS1_V21                   /* REQUIRED for TLS 1.3 RSA-PSS signatures */
#define MBEDTLS_PKCS8_C
#define MBEDTLS_PK_WRITE_C                  /* Recommended for internal key/signature operations */

/* Hashing & RNG */
#define MBEDTLS_SHA1_C                      /* Required by X.509 SKI parsing */
#define MBEDTLS_SHA256_C
#define MBEDTLS_SHA384_C
#define MBEDTLS_SHA512_C
#define MBEDTLS_MD_C
#define MBEDTLS_CTR_DRBG_C
#define MBEDTLS_ENTROPY_C

/* X.509 Certificate Parsing */
#define MBEDTLS_X509_USE_C
#define MBEDTLS_X509_CRT_PARSE_C
#define MBEDTLS_X509_RSASSA_PSS_SUPPORT

/* PSA Layer Support */
#define MBEDTLS_PSA_CRYPTO_C
#define MBEDTLS_PSA_CRYPTO_CLIENT
#define MBEDTLS_PSA_ACCEL_ALG_RSA_PSS
#define MBEDTLS_PSA_ACCEL_ALG_ECDSA
#define MBEDTLS_PSA_ACCEL_ALG_SHA_256
#define MBEDTLS_PSA_ACCEL_ALG_SHA_384

/* ASN.1 Parsing */
#define MBEDTLS_ASN1_PARSE_C
#define MBEDTLS_ASN1_WRITE_C

/* Enable PSA Built-in Hash Dispatching */
#define MBEDTLS_PSA_BUILTIN_HASH
#define MBEDTLS_PSA_BUILTIN_ALG_SHA_256
#define MBEDTLS_PSA_BUILTIN_ALG_SHA_384
#define MBEDTLS_PSA_BUILTIN_ALG_SHA_1        /* Needed for legacy X.509 SKI/AKIs */

/* Enable PSA Built-in Key & Signature Algorithms */
#define MBEDTLS_PSA_BUILTIN_ALG_RSA_PSS
#define MBEDTLS_PSA_BUILTIN_ALG_RSA_PKCS1V15
#define MBEDTLS_PSA_BUILTIN_ALG_ECDSA
#define MBEDTLS_PSA_BUILTIN_KEY_TYPE_RSA_KEY_PAIR
#define MBEDTLS_PSA_BUILTIN_KEY_TYPE_ECC_KEY_PAIR

/* ========================================================================= */
/* Optimization & System Overrides                                          */
/* ========================================================================= */
#undef MBEDTLS_ERROR_C
//#undef MBEDTLS_DEBUG_C
#undef MBEDTLS_VERSION_C
#undef MBEDTLS_DES_C
#undef MBEDTLS_ARC4_C
#undef MBEDTLS_MD5_C

#define MBEDTLS_HAVE_TIME
#define MBEDTLS_PLATFORM_MS_TIME_ALT

#define MBEDTLS_THREADING_C
#define MBEDTLS_THREADING_ALT

#define MBEDTLS_PSA_DRIVER_GET_ENTROPY
#define MBEDTLS_PSA_CRYPTO_EXTERNAL_RNG

#define MBEDTLS_ALLOW_PRIVATE_ACCESS
#define MBEDTLS_PRIVATE(X) X
#define MBEDTLS_DECLARE_PRIVATE_IDENTIFIERS

#ifdef DEBUG
    #define MBEDTLS_DEBUG_C
#endif

#endif /* MBEDTLS_CONFIG_H */
