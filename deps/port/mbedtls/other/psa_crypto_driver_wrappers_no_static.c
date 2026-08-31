/*
 * Hand-edited static conversion of psa_crypto_driver_wrappers.jinja
 * to solve missing symbol link errors in custom/embedded toolchains.
 */

#include "tf_psa_crypto_common.h"
#include "psa_crypto_aead.h"
#include "psa_crypto_cipher.h"
#include "psa_crypto_core.h"
#include "psa_crypto_driver_wrappers_no_static.h"
#include "psa_crypto_hash.h"
#include "psa_crypto_mac.h"
#include "psa_crypto_pake.h"
#include "psa_crypto_rsa.h"

#if defined(TF_PSA_CRYPTO_PQCP_MLDSA_ENABLED)
#include "psa_crypto_mldsa.h"
#endif

#include "mbedtls/platform.h"

#if defined(MBEDTLS_PSA_CRYPTO_C)

#define PSA_CRYPTO_MBED_TLS_DRIVER_ID (1)

psa_status_t psa_driver_wrapper_get_key_buffer_size(
    const psa_key_attributes_t *attributes,
    size_t *key_buffer_size )
{
    psa_key_location_t location = PSA_KEY_LIFETIME_GET_LOCATION( psa_get_key_lifetime(attributes) );
    psa_key_type_t key_type = psa_get_key_type(attributes);
    size_t key_bits = psa_get_key_bits(attributes);

    *key_buffer_size = 0;
    switch( location )
    {
#if defined(PSA_CRYPTO_DRIVER_TEST)
        case PSA_CRYPTO_TEST_DRIVER_LOCATION:
#if defined(MBEDTLS_PSA_CRYPTO_BUILTIN_KEYS)
            if( psa_key_id_is_builtin(
                    MBEDTLS_SVC_KEY_ID_GET_KEY_ID(
                        psa_get_key_id( attributes ) ) ) )
            {
                *key_buffer_size = sizeof( psa_drv_slot_number_t );
                return( PSA_SUCCESS );
            }
#endif /* MBEDTLS_PSA_CRYPTO_BUILTIN_KEYS */
            *key_buffer_size = mbedtls_test_opaque_size_function( key_type, key_bits );
            return( ( *key_buffer_size != 0 ) ? PSA_SUCCESS : PSA_ERROR_NOT_SUPPORTED );
#endif /* PSA_CRYPTO_DRIVER_TEST */

        case PSA_KEY_LOCATION_LOCAL_STORAGE:
            *key_buffer_size = PSA_EXPORT_KEY_OUTPUT_SIZE(key_type, key_bits);
            return( PSA_SUCCESS );

        default:
            (void)key_type;
            (void)key_bits;
            return( PSA_ERROR_INVALID_ARGUMENT );
    }
}

psa_status_t psa_driver_wrapper_export_public_key(
    const psa_key_attributes_t *attributes,
    const uint8_t *key_buffer, size_t key_buffer_size,
    uint8_t *data, size_t data_size, size_t *data_length )
{
    psa_key_location_t location = PSA_KEY_LIFETIME_GET_LOCATION(
                                      psa_get_key_lifetime( attributes ) );

    switch( location )
    {
        case PSA_KEY_LOCATION_LOCAL_STORAGE:
#if defined(TF_PSA_CRYPTO_PQCP_MLDSA_ENABLED)
            if (psa_get_key_type(attributes) == PSA_KEY_TYPE_ML_DSA_KEY_PAIR) {
                return tf_psa_crypto_mldsa_export_public_key(
                            attributes,
                            key_buffer, key_buffer_size,
                            data, data_size, data_length);
            }
#endif
            return psa_export_public_key_internal( attributes,
                                                   key_buffer,
                                                   key_buffer_size,
                                                   data,
                                                   data_size,
                                                   data_length );

        default:
            return PSA_ERROR_INVALID_ARGUMENT;
    }
}

psa_status_t psa_driver_wrapper_get_builtin_key(
    psa_drv_slot_number_t slot_number,
    psa_key_attributes_t *attributes,
    uint8_t *key_buffer, size_t key_buffer_size, size_t *key_buffer_length )
{
    (void) slot_number;
    (void) attributes;
    (void) key_buffer;
    (void) key_buffer_size;
    (void) key_buffer_length;
    return PSA_ERROR_DOES_NOT_EXIST;
}

#endif /* MBEDTLS_PSA_CRYPTO_C */
