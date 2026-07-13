/* ce_tls_ctx.c: shared wolfSSL client context (see ce_tls_ctx.h). */

#include <stddef.h>

#include "ce_tls_ctx.h"
#include "ce_ca_bundle.h"

WOLFSSL_CTX *ce_tls_client_ctx_create(void)
{
    WOLFSSL_CTX *ctx;

    if (wolfSSL_Init() != WOLFSSL_SUCCESS) {
        return NULL;
    }
    /* wolfSSLv23_client_method negotiates the highest TLS the server offers;
     * NO_OLD_TLS in user_settings.h floors it at TLS 1.2. */
    ctx = wolfSSL_CTX_new(wolfSSLv23_client_method());
    if (ctx == NULL) {
        wolfSSL_Cleanup();
        return NULL;
    }

    /* IGNORE_ERR + DATE_ERR_OKAY so one unparseable/expired root among the
     * ~145 in the bundle doesn't abort the whole trust-store load (a single
     * date failure against a skewed device clock would otherwise sink it). */
    wolfSSL_CTX_load_verify_buffer_ex(ctx,
                                      ce_ca_bundle_pem,
                                      (long)ce_ca_bundle_pem_len,
                                      WOLFSSL_FILETYPE_PEM,
                                      0,
                                      WOLFSSL_LOAD_FLAG_IGNORE_ERR |
                                      WOLFSSL_LOAD_FLAG_DATE_ERR_OKAY);
    wolfSSL_CTX_set_verify(ctx, WOLFSSL_VERIFY_PEER, NULL);
    return ctx;
}

void ce_tls_client_ctx_free(WOLFSSL_CTX *ctx)
{
    if (ctx != NULL) {
        wolfSSL_CTX_free(ctx);
    }
    wolfSSL_Cleanup();
}
