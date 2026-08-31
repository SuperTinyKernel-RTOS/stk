from pathlib import Path
from cryptography.hazmat.primitives import serialization
from cryptography.x509 import load_pem_x509_certificate

# Create server.key and .cert with:
# openssl req -x509 -newkey rsa:2048 -keyout server.key -out server.crt -days 365 -nodes -subj "/CN=localhost"
# openssl req -x509 -newkey ec -pkeyopt ec_paramgen_curve:prime256v1 -keyout server_key.pem -out server_cert.pem -days 365 -nodes
# then run this script.

KEY_FILE = "server.key"
CERT_FILE = "server.crt"
OUTPUT_HEADER = "example_certs.h"

def load_key(filename: str) -> bytes:
    data = Path(filename).read_bytes()
    if b"-----BEGIN" in data:
        key = serialization.load_pem_private_key(data, password=None)
        return key.private_bytes(
            encoding=serialization.Encoding.DER,
            # Use TraditionalOpenSSL instead of PKCS8 for standard PKCS#1 / SEC1 DER
            format=serialization.PrivateFormat.TraditionalOpenSSL,
            # Otherwise you will need to enable PK parser: #define MBEDTLS_PKCS8_C, #define MBEDTLS_PK_PARSE_C
            #format=serialization.PrivateFormat.PKCS8,
            encryption_algorithm=serialization.NoEncryption()
        )
    return data

def load_cert(filename: str) -> bytes:
    data = Path(filename).read_bytes()
    if b"-----BEGIN" in data:
        cert = load_pem_x509_certificate(data)
        return cert.public_bytes(serialization.Encoding.DER)
    return data

def format_c_array(name: str, data: bytes) -> str:
    hex_bytes = [f"0x{b:02x}" for b in data]
    lines = [", ".join(hex_bytes[i:i + 12]) for i in range(0, len(hex_bytes), 12)]
    formatted_body = ",\n    ".join(lines)
    
    return (
        f"const unsigned char {name}[] = {{\n"
        f"    {formatted_body}\n"
        f"}};\n"
        f"const unsigned int {name}Len = {len(data)};\n"
    )

def main():
    key = load_key(KEY_FILE)
    cert = load_cert(CERT_FILE)

    header_content = (
        "#ifndef EXAMPLE_CERTS_H_\n"
        "#define EXAMPLE_CERTS_H_\n\n"
        "#include <stdint.h>\n\n"
        f"{format_c_array('s_TlsKey', key)}\n"
        f"{format_c_array('s_TlsCert', cert)}\n"
        "#endif /* EXAMPLE_CERTS_H_ */\n"
    )

    Path(OUTPUT_HEADER).write_text(header_content, encoding="utf-8")
    print(f"Successfully generated {OUTPUT_HEADER}")

if __name__ == "__main__":
    main()