# NMake makefile: wolfSSL static library for Zune HD (ARM Thumb / CE 6.0)
# Usage (from build\ after running env_setup.bat):
#   nmake /f wolfssl.mak

WOLFSSL_ROOT = ..\deps\wolfssl
USER_SETTINGS = ..\deps\user_settings.h
OUT_DIR = ..\out\wolfssl
OBJ_DIR = $(OUT_DIR)\obj
LIB_OUT = $(OUT_DIR)\wolfssl_ce_arm.lib

CC  = $(CE_CC)
LIB = $(CE_LIB)

# /TP: compile all .c as C++ (sidesteps MSVC 2008 C99 gaps: //comments, mixed decls)
# /FI: force-include user_settings.h before every translation unit
WOLF_CFLAGS = $(CE_CFLAGS) /TP \
	/DWOLFSSL_USER_SETTINGS \
	/FI"user_settings.h" \
	/I"..\deps" \
	/I"$(WOLFSSL_ROOT)" \
	/I"$(WOLFSSL_ROOT)\wolfssl" \
	/I"$(WOLFSSL_ROOT)\wolfcrypt\src" \
	/wd4013 /wd4018 /wd4101 /wd4244 /wd4267 /wd4305 /wd4306 /wd4311 /wd4312

# ── TLS protocol layer ────────────────────────────────────────────────────

TLS_SRCS = \
	$(WOLFSSL_ROOT)\src\ssl.c \
	$(WOLFSSL_ROOT)\src\internal.c \
	$(WOLFSSL_ROOT)\src\keys.c \
	$(WOLFSSL_ROOT)\src\tls.c \
	$(WOLFSSL_ROOT)\src\tls13.c \
	$(WOLFSSL_ROOT)\src\wolfio.c

# ── Crypto primitives ─────────────────────────────────────────────────────

CRYPTO_SRCS = \
	$(WOLFSSL_ROOT)\wolfcrypt\src\aes.c \
	$(WOLFSSL_ROOT)\wolfcrypt\src\asn.c \
	$(WOLFSSL_ROOT)\wolfcrypt\src\coding.c \
	$(WOLFSSL_ROOT)\wolfcrypt\src\dh.c \
	$(WOLFSSL_ROOT)\wolfcrypt\src\ecc.c \
	$(WOLFSSL_ROOT)\wolfcrypt\src\error.c \
	$(WOLFSSL_ROOT)\wolfcrypt\src\hash.c \
	$(WOLFSSL_ROOT)\wolfcrypt\src\hmac.c \
	$(WOLFSSL_ROOT)\wolfcrypt\src\kdf.c \
	$(WOLFSSL_ROOT)\wolfcrypt\src\md5.c \
	$(WOLFSSL_ROOT)\wolfcrypt\src\memory.c \
	$(WOLFSSL_ROOT)\wolfcrypt\src\misc.c \
	$(WOLFSSL_ROOT)\wolfcrypt\src\random.c \
	$(WOLFSSL_ROOT)\wolfcrypt\src\rsa.c \
	$(WOLFSSL_ROOT)\wolfcrypt\src\sha.c \
	$(WOLFSSL_ROOT)\wolfcrypt\src\sha256.c \
	$(WOLFSSL_ROOT)\wolfcrypt\src\sha512.c \
	$(WOLFSSL_ROOT)\wolfcrypt\src\sha3.c \
	$(WOLFSSL_ROOT)\wolfcrypt\src\signature.c \
	$(WOLFSSL_ROOT)\wolfcrypt\src\tfm.c \
	$(WOLFSSL_ROOT)\wolfcrypt\src\wc_encrypt.c \
	$(WOLFSSL_ROOT)\wolfcrypt\src\wc_port.c \
	$(WOLFSSL_ROOT)\wolfcrypt\src\wolfmath.c \
	$(WOLFSSL_ROOT)\wolfcrypt\src\cmac.c

ALL_SRCS = $(TLS_SRCS) $(CRYPTO_SRCS)

# ── Object file list (manual expansion for NMake) ─────────────────────────

TLS_OBJS = \
	$(OBJ_DIR)\ssl.obj \
	$(OBJ_DIR)\internal.obj \
	$(OBJ_DIR)\keys.obj \
	$(OBJ_DIR)\tls.obj \
	$(OBJ_DIR)\tls13.obj \
	$(OBJ_DIR)\wolfio.obj

CRYPTO_OBJS = \
	$(OBJ_DIR)\aes.obj \
	$(OBJ_DIR)\asn.obj \
	$(OBJ_DIR)\coding.obj \
	$(OBJ_DIR)\dh.obj \
	$(OBJ_DIR)\ecc.obj \
	$(OBJ_DIR)\error.obj \
	$(OBJ_DIR)\hash.obj \
	$(OBJ_DIR)\hmac.obj \
	$(OBJ_DIR)\kdf.obj \
	$(OBJ_DIR)\md5.obj \
	$(OBJ_DIR)\memory.obj \
	$(OBJ_DIR)\misc.obj \
	$(OBJ_DIR)\random.obj \
	$(OBJ_DIR)\rsa.obj \
	$(OBJ_DIR)\sha.obj \
	$(OBJ_DIR)\sha256.obj \
	$(OBJ_DIR)\sha512.obj \
	$(OBJ_DIR)\sha3.obj \
	$(OBJ_DIR)\signature.obj \
	$(OBJ_DIR)\tfm.obj \
	$(OBJ_DIR)\wc_encrypt.obj \
	$(OBJ_DIR)\wc_port.obj \
	$(OBJ_DIR)\wolfmath.obj \
	$(OBJ_DIR)\cmac.obj

ALL_OBJS = $(TLS_OBJS) $(CRYPTO_OBJS)

# ── Targets ───────────────────────────────────────────────────────────────

all: makedirs $(LIB_OUT)
	@echo.
	@echo wolfSSL ARM CE library: $(LIB_OUT)

makedirs:
	@if not exist "$(OUT_DIR)" mkdir "$(OUT_DIR)"
	@if not exist "$(OBJ_DIR)" mkdir "$(OBJ_DIR)"

$(LIB_OUT): $(ALL_OBJS)
	$(LIB) /nologo /OUT:$(LIB_OUT) $(ALL_OBJS)

clean:
	@if exist "$(OUT_DIR)" rmdir /s /q "$(OUT_DIR)"

# ── TLS layer compile rules ───────────────────────────────────────────────

$(OBJ_DIR)\ssl.obj: $(WOLFSSL_ROOT)\src\ssl.c
	$(CC) $(WOLF_CFLAGS) /Fo"$(OBJ_DIR)\ssl.obj" /c $**

$(OBJ_DIR)\internal.obj: $(WOLFSSL_ROOT)\src\internal.c
	$(CC) $(WOLF_CFLAGS) /Fo"$(OBJ_DIR)\internal.obj" /c $**

$(OBJ_DIR)\keys.obj: $(WOLFSSL_ROOT)\src\keys.c
	$(CC) $(WOLF_CFLAGS) /Fo"$(OBJ_DIR)\keys.obj" /c $**

$(OBJ_DIR)\tls.obj: $(WOLFSSL_ROOT)\src\tls.c
	$(CC) $(WOLF_CFLAGS) /Fo"$(OBJ_DIR)\tls.obj" /c $**

$(OBJ_DIR)\tls13.obj: $(WOLFSSL_ROOT)\src\tls13.c
	$(CC) $(WOLF_CFLAGS) /Fo"$(OBJ_DIR)\tls13.obj" /c $**

$(OBJ_DIR)\wolfio.obj: $(WOLFSSL_ROOT)\src\wolfio.c
	$(CC) $(WOLF_CFLAGS) /Fo"$(OBJ_DIR)\wolfio.obj" /c $**

# ── Crypto layer compile rules ────────────────────────────────────────────

$(OBJ_DIR)\aes.obj: $(WOLFSSL_ROOT)\wolfcrypt\src\aes.c
	$(CC) $(WOLF_CFLAGS) /Fo"$(OBJ_DIR)\aes.obj" /c $**

$(OBJ_DIR)\asn.obj: $(WOLFSSL_ROOT)\wolfcrypt\src\asn.c
	$(CC) $(WOLF_CFLAGS) /Fo"$(OBJ_DIR)\asn.obj" /c $**

$(OBJ_DIR)\coding.obj: $(WOLFSSL_ROOT)\wolfcrypt\src\coding.c
	$(CC) $(WOLF_CFLAGS) /Fo"$(OBJ_DIR)\coding.obj" /c $**

$(OBJ_DIR)\dh.obj: $(WOLFSSL_ROOT)\wolfcrypt\src\dh.c
	$(CC) $(WOLF_CFLAGS) /Fo"$(OBJ_DIR)\dh.obj" /c $**

$(OBJ_DIR)\ecc.obj: $(WOLFSSL_ROOT)\wolfcrypt\src\ecc.c
	$(CC) $(WOLF_CFLAGS) /Fo"$(OBJ_DIR)\ecc.obj" /c $**

$(OBJ_DIR)\error.obj: $(WOLFSSL_ROOT)\wolfcrypt\src\error.c
	$(CC) $(WOLF_CFLAGS) /Fo"$(OBJ_DIR)\error.obj" /c $**

$(OBJ_DIR)\hash.obj: $(WOLFSSL_ROOT)\wolfcrypt\src\hash.c
	$(CC) $(WOLF_CFLAGS) /Fo"$(OBJ_DIR)\hash.obj" /c $**

$(OBJ_DIR)\hmac.obj: $(WOLFSSL_ROOT)\wolfcrypt\src\hmac.c
	$(CC) $(WOLF_CFLAGS) /Fo"$(OBJ_DIR)\hmac.obj" /c $**

$(OBJ_DIR)\kdf.obj: $(WOLFSSL_ROOT)\wolfcrypt\src\kdf.c
	$(CC) $(WOLF_CFLAGS) /Fo"$(OBJ_DIR)\kdf.obj" /c $**

$(OBJ_DIR)\md5.obj: $(WOLFSSL_ROOT)\wolfcrypt\src\md5.c
	$(CC) $(WOLF_CFLAGS) /Fo"$(OBJ_DIR)\md5.obj" /c $**

$(OBJ_DIR)\memory.obj: $(WOLFSSL_ROOT)\wolfcrypt\src\memory.c
	$(CC) $(WOLF_CFLAGS) /Fo"$(OBJ_DIR)\memory.obj" /c $**

$(OBJ_DIR)\misc.obj: $(WOLFSSL_ROOT)\wolfcrypt\src\misc.c
	$(CC) $(WOLF_CFLAGS) /Fo"$(OBJ_DIR)\misc.obj" /c $**

$(OBJ_DIR)\random.obj: $(WOLFSSL_ROOT)\wolfcrypt\src\random.c
	$(CC) $(WOLF_CFLAGS) /Fo"$(OBJ_DIR)\random.obj" /c $**

$(OBJ_DIR)\rsa.obj: $(WOLFSSL_ROOT)\wolfcrypt\src\rsa.c
	$(CC) $(WOLF_CFLAGS) /Fo"$(OBJ_DIR)\rsa.obj" /c $**

$(OBJ_DIR)\sha.obj: $(WOLFSSL_ROOT)\wolfcrypt\src\sha.c
	$(CC) $(WOLF_CFLAGS) /Fo"$(OBJ_DIR)\sha.obj" /c $**

$(OBJ_DIR)\sha256.obj: $(WOLFSSL_ROOT)\wolfcrypt\src\sha256.c
	$(CC) $(WOLF_CFLAGS) /Fo"$(OBJ_DIR)\sha256.obj" /c $**

$(OBJ_DIR)\sha512.obj: $(WOLFSSL_ROOT)\wolfcrypt\src\sha512.c
	$(CC) $(WOLF_CFLAGS) /Fo"$(OBJ_DIR)\sha512.obj" /c $**

$(OBJ_DIR)\sha3.obj: $(WOLFSSL_ROOT)\wolfcrypt\src\sha3.c
	$(CC) $(WOLF_CFLAGS) /Fo"$(OBJ_DIR)\sha3.obj" /c $**

$(OBJ_DIR)\signature.obj: $(WOLFSSL_ROOT)\wolfcrypt\src\signature.c
	$(CC) $(WOLF_CFLAGS) /Fo"$(OBJ_DIR)\signature.obj" /c $**

$(OBJ_DIR)\tfm.obj: $(WOLFSSL_ROOT)\wolfcrypt\src\tfm.c
	$(CC) $(WOLF_CFLAGS) /Fo"$(OBJ_DIR)\tfm.obj" /c $**

$(OBJ_DIR)\wc_encrypt.obj: $(WOLFSSL_ROOT)\wolfcrypt\src\wc_encrypt.c
	$(CC) $(WOLF_CFLAGS) /Fo"$(OBJ_DIR)\wc_encrypt.obj" /c $**

$(OBJ_DIR)\wc_port.obj: $(WOLFSSL_ROOT)\wolfcrypt\src\wc_port.c
	$(CC) $(WOLF_CFLAGS) /Fo"$(OBJ_DIR)\wc_port.obj" /c $**

$(OBJ_DIR)\wolfmath.obj: $(WOLFSSL_ROOT)\wolfcrypt\src\wolfmath.c
	$(CC) $(WOLF_CFLAGS) /Fo"$(OBJ_DIR)\wolfmath.obj" /c $**

$(OBJ_DIR)\cmac.obj: $(WOLFSSL_ROOT)\wolfcrypt\src\cmac.c
	$(CC) $(WOLF_CFLAGS) /Fo"$(OBJ_DIR)\cmac.obj" /c $**
