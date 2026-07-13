/* ce_tls_ctx.h: shared wolfSSL client context for standalone (non-NetSurf)
 * HTTPS on the Zune HD.
 *
 * Single source of truth for "how this project builds a verifying TLS client
 * context": wolfSSL_Init + a TLS 1.2/1.3 client CTX + the embedded Mozilla CA
 * bundle (the same `ce_ca_bundle_pem` the NetSurf fetcher's ce_http_tls.c
 * loads). No filesystem trust store (NO_FILESYSTEM build); no NetSurf coupling.
 */
#ifndef ZB_CE_TLS_CTX_H
#define ZB_CE_TLS_CTX_H

#include <wolfssl/ssl.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Build a verifying TLS client CTX (WOLFSSL_VERIFY_PEER) trusting the embedded
 * CA bundle. Calls wolfSSL_Init() on first use. Returns NULL on failure. The
 * caller owns the CTX and frees it with ce_tls_client_ctx_free(). */
WOLFSSL_CTX *ce_tls_client_ctx_create(void);

/* Free a CTX from ce_tls_client_ctx_create() and run wolfSSL_Cleanup(). */
void ce_tls_client_ctx_free(WOLFSSL_CTX *ctx);

#ifdef __cplusplus
}
#endif

#endif /* ZB_CE_TLS_CTX_H */
