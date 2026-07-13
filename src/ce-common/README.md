# ce-common

Shared Windows CE 6.0 / ARMv4I (Zune HD) native runtime. Single source of truth
for the components every Zune CE deliverable links: wolfSSL TLS, the standalone
HTTPS transport, the embedded CA bundle, the `imaging.dll` adapter, and the
ARM-CE toolchain environment.

Consumed as a git submodule (at `deps/ce-common`) by **zune-browser**,
**zune-cast**, and **zune-yt**.

## Layout

```
deps/
  wolfssl/            wolfSSL (submodule, pinned v5.9.1-stable)
  user_settings.h     wolfSSL CE build config, force-included via /FI by every wolfSSL-consuming TU
  ce_image/           imaging.dll COM adapter (decode/scale/encode)
src/
  ce_https/           ce_https.{c,h} (keep-alive ranged HTTPS) + ce_tls_ctx.{c,h} (verifying client CTX)
  ce_ca_bundle.{c,h}  embedded Mozilla root CA PEM bundle (pure data)
build/
  env_setup.bat       ARM CE 6 toolchain env (VS2008 CE-ARM + OpenZDK). `call` it before nmake.
  wolfssl.mak         builds out/wolfssl/wolfssl_ce_arm.lib
  ce_image.mak        builds out/ce_image/ce_image_ce_arm.lib
out/
  wolfssl/*.lib       committed prebuilt static libs (rebuilt only when the source changes)
  ce_image/*.lib
tools/
  gen_ca_bundle.py    regenerates src/ce_ca_bundle.c from an upstream PEM bundle
```

## Building the prebuilt libs

From `build/` on the ARM-CE build host:

```
call env_setup.bat
nmake /f wolfssl.mak
nmake /f ce_image.mak
```

The libs are committed, so consumers normally just link them; rebuild only when
the wolfSSL submodule SHA or `ce_image` source changes.

## Consumer contract

A consumer's nmake makefile links the prebuilt libs and (for the source TUs)
compiles them directly. With the submodule at `deps/ce-common`:

- **wolfSSL** (all consumers): `/DWOLFSSL_USER_SETTINGS /FI"user_settings.h"`,
  `/I"deps/ce-common/deps"` (for `user_settings.h`) + `/I"deps/ce-common/deps/wolfssl"`,
  link `deps/ce-common/out/wolfssl/wolfssl_ce_arm.lib`.
- **HTTPS transport** (browser, yt): compile `src/ce_https/ce_https.c`,
  `src/ce_https/ce_tls_ctx.c`, `src/ce_ca_bundle.c`; add `/I"deps/ce-common/src"`
  (for `ce_ca_bundle.h`) + `/I"deps/ce-common/src/ce_https"`.
- **Image** (browser, cast): `/I"deps/ce-common/deps/ce_image"`, link
  `deps/ce-common/out/ce_image/ce_image_ce_arm.lib`.
- **Toolchain**: `call ..\..\deps\ce-common\build\env_setup.bat` (path relative to the
  consumer's `build/` dir).
