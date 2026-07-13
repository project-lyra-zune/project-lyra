/* ce_ca_bundle.h: embedded Mozilla root CA PEM bundle (pure data).
 *
 * Single declaration of the trust store both TLS consumers load: the NetSurf
 * fetcher (ce_http_tls.c) and the standalone client context (ce_tls_ctx.c).
 * The data TU (ce_ca_bundle.c) has no NetSurf coupling.
 */
#ifndef CE_CA_BUNDLE_H
#define CE_CA_BUNDLE_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

extern const unsigned char ce_ca_bundle_pem[];
extern const size_t        ce_ca_bundle_pem_len;

#ifdef __cplusplus
}
#endif

#endif /* CE_CA_BUNDLE_H */
