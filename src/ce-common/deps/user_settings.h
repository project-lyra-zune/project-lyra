/* wolfSSL user settings for Zune HD (Windows CE 6.0, ARMv4I, TLS client) */

#ifndef WOLFSSL_USER_SETTINGS_H
#define WOLFSSL_USER_SETTINGS_H

/* ── Platform ────────────────────────────────────────────────────────────── */

#define WOLFSSL_GENERAL_ALIGNMENT   4

/* CE 6 specifics: _WIN32_WCE is defined by the compiler */
#define NO_WOLFSSL_DIR
#define WOLFSSL_NO_ATOMICS
#define WC_NO_ASYNC_THREADING
#define USE_WINDOWS_API
#define WOLFSSL_SMALL_STACK
#define SINGLE_THREADED
#define NO_THREAD_LS
#define NO_WRITE_TEMP_FILES

/* misc.c must be compiled as a standalone TU, not inlined into callers */
#define NO_INLINE

/* CE coredll has _vsnprintf but not vsnprintf */
#define XVSNPRINTF _vsnprintf

/* windows.h defines min/max as macros; suppress wolfSSL's own declarations */
#define WOLFSSL_HAVE_MIN
#define WOLFSSL_HAVE_MAX

/* Client only: we never act as a TLS server */
#define NO_WOLFSSL_SERVER

/* ── Math ────────────────────────────────────────────────────────────────── */

#define USE_FAST_MATH
#define TFM_TIMING_RESISTANT
#define TFM_NO_ASM          /* no inline ASM: portable C, optimise later */
#define FP_MAX_BITS         8192
#define ALT_ECC_SIZE
#define ECC_TIMING_RESISTANT

/* ── Crypto primitives ───────────────────────────────────────────────────── */

/* RSA */
#define WC_RSA_BLINDING
#define WC_RSA_PSS

/* ECC: needed for ECDHE key exchange */
#define HAVE_ECC
#define ECC_SHAMIR

/* DH: needed for DHE cipher suites */
#define HAVE_DH_DEFAULT_PARAMS
#define WOLFSSL_DH_CONST
#define HAVE_FFDHE_2048
#define HAVE_FFDHE_3072

/* AES */
#define HAVE_AES_CBC
#define HAVE_AESGCM
#define GCM_TABLE_4BIT
#define WOLFSSL_AES_DIRECT
#define WOLFSSL_AES_COUNTER

/* Hashing */
#define WOLFSSL_SHA384
#define WOLFSSL_SHA512
/* NO_SHA256, NO_SHA, NO_MD5 intentionally not set: all needed for TLS 1.2 */

#define HAVE_HKDF
#define WOLFSSL_HAVE_PRF

/* Disable algorithms we don't need */
#define NO_DES3
#define NO_RC4
#define NO_DSA
#define NO_MD4
#define NO_PSK
#define NO_PWDBASED

/* ── TLS protocol ────────────────────────────────────────────────────────── */

#define WOLFSSL_TLS13
#define NO_OLD_TLS          /* no SSL 2/3, TLS 1.0/1.1 */

#define HAVE_TLS_EXTENSIONS
#define HAVE_SUPPORTED_CURVES
#define HAVE_EXTENDED_MASTER
#define HAVE_SESSION_TICKET
#define HAVE_ENCRYPT_THEN_MAC
#define HAVE_SERVER_RENEGOTIATION_INFO

/* SNI: required for virtual-hosted HTTPS servers (modern web default). */
#define HAVE_SNI

/* ALPN: optional but cheap; lets servers select http/1.1 explicitly so
 * we don't accidentally end up speaking to an HTTP/2-only endpoint. */
#define HAVE_ALPN

/* Session cache: small footprint */
#define SMALL_SESSION_CACHE

/* ── Certificate handling ────────────────────────────────────────────────── */

#define WOLFSSL_ASN_TEMPLATE
#define WOLFSSL_BASE64_ENCODE
#define WOLFSSL_DER_LOAD

/* ARM CE 6 traps unaligned 32-bit loads with STATUS_DATATYPE_MISALIGNMENT
 * (0x80000002). wolfSSL's default ato32/c32toa fast path does *(word32*)c
 * on byte-oriented ASN.1 streams, which is misaligned by construction.
 * WOLFSSL_USE_ALIGN forces byte-wise composition. Slight perf hit;
 * correctness on the device. */
#define WOLFSSL_USE_ALIGN

/* Many production sites still serve cert chains that terminate at a
 * legacy cross-signing root (Google -> GlobalSign Root CA, IANA ->
 * AAA Certificate Services, etc.) which Mozilla has pruned from its
 * modern trust list. With ALT_CERT_CHAINS, wolfSSL tries alternate
 * paths: if the chain contains an intermediate whose self-signed
 * version IS in our trust store, the validation succeeds without
 * needing the legacy cross-signer root. Closes the "no trust anchor"
 * class of failures for chains we'd otherwise have to anchor by
 * appending each retired root one-at-a-time. */
#define WOLFSSL_ALT_CERT_CHAINS

/* Use bundled CA cert buffers (no filesystem read needed) */
#define NO_FILESYSTEM
#define USE_CERT_BUFFERS_2048
#define USE_CERT_BUFFERS_256

/* ── RNG ─────────────────────────────────────────────────────────────────── */

#define HAVE_HASHDRBG       /* SHA-256 based DRBG, seeded from CryptGenRandom */

/* ── Memory ──────────────────────────────────────────────────────────────── */

#define USE_WOLFSSL_MEMORY

/* ── Misc ────────────────────────────────────────────────────────────────── */

#define NO_MAIN_DRIVER
#define NO_DO178
#define WOLFSSL_NO_SHAKE128
#define WOLFSSL_NO_SHAKE256

#endif /* WOLFSSL_USER_SETTINGS_H */
