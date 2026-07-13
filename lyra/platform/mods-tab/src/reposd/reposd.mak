# NMake makefile for reposd.exe (the Lyra mod-repository daemon).
# Spawned at boot by mods-tab's `daemons` capability. Its own process + Winsock:
# runs the blocking HTTPS feed fetch + package install off the gemstone UI thread,
# publishing state into the repo shared section (repo_ipc.h). Links the ce-common
# stack (ce_https + wolfSSL + ws2) like ytsearchd, plus zlib/minizip for .zmod
# (standard ZIP) extraction.

# This rich mod lives under lyra/platform/mods-tab/src/reposd; the platform + shared
# libs (ce-common) and the platform-owned enabled.json codec live in the tree's
# top-level src/, five levels up. zlib is reposd's own vendored dep under deps/ (only
# reposd uses it), mirroring ce-common/deps/wolfssl.
SRC       = ..\..\..\..\..\src
CEC       = $(SRC)\ce-common
HTTPS_DIR = $(CEC)\src\ce_https
ZLIB      = deps\zlib
MZ        = deps\zlib\contrib\minizip
MR        = $(SRC)\mod-runtime
OUT_DIR   = out
OBJ_DIR   = $(OUT_DIR)\obj
EXE_OUT   = $(OUT_DIR)\reposd.exe

CC   = $(CE_CC)
LINK = $(CE_LINK)

# reposd.cpp pulls unzip.h -> zconf.h, which wants <sys/types.h> (absent on
# OpenZDK); ceshim supplies it. NOT Z_SOLO. Z_SOLO drops zlib's default malloc
# allocator, and minizip's inflate (zalloc=0) then fails with Z_STREAM_ERROR.
# ce_* also need the wolfSSL config force-included (like ytsearchd).
WOLF_CFLAGS = $(CE_CFLAGS) \
	/DWOLFSSL_USER_SETTINGS \
	/FI"user_settings.h" \
	/I"$(CEC)\deps" \
	/I"$(CEC)\deps\wolfssl" \
	/I"$(HTTPS_DIR)" \
	/I"$(CEC)\src" \
	/I"$(ZLIB)" \
	/I"$(MZ)" \
	/I"$(MR)" \
	/I"ceshim" \
	/I"."

# zlib/minizip CE flags. ce_extras.h supplies the CE/MSVC compat shims
# (inline/restrict/strdup); ceshim supplies <sys/types.h>. USE_FILE32API maps
# ftello64/fseeko64 to 32-bit ftell/fseek (CE lacks _ftelli64/_fseeki64).
# USE_FILE32API maps minizip's ftello64/fseeko64 to 32-bit ftell/fseek (CE lacks
# _ftelli64/_fseeki64). reposd's packages are far under 2 GB, so 32-bit is fine,
# and the fopen path is dead anyway (reposd uses its own CE filefunc).
ZLIB_CFLAGS = $(CE_CFLAGS) \
	/DNO_FSEEKO /DZLIB_CONST /DSTDC /DUSE_FILE32API \
	/FI"ce_extras.h" /I"ceshim" \
	/I"$(ZLIB)" /I"$(MZ)" \
	/wd4244 /wd4267 /wd4127 /wd4146

LIBS = \
	$(CEC)\out\wolfssl\wolfssl_ce_arm.lib \
	coredll.lib corelibc.lib ws2.lib

ALL_OBJS = \
	$(OBJ_DIR)\reposd.obj \
	$(OBJ_DIR)\repo_feed.obj \
	$(OBJ_DIR)\ce_https.obj \
	$(OBJ_DIR)\ce_tls_ctx.obj \
	$(OBJ_DIR)\ce_ca_bundle.obj \
	$(OBJ_DIR)\unzip.obj \
	$(OBJ_DIR)\ioapi.obj \
	$(OBJ_DIR)\repo_ceio.obj \
	$(OBJ_DIR)\enabled_set.obj \
	$(OBJ_DIR)\mods_json.obj \
	$(OBJ_DIR)\mods_arena.obj \
	$(OBJ_DIR)\inflate.obj \
	$(OBJ_DIR)\inftrees.obj \
	$(OBJ_DIR)\inffast.obj \
	$(OBJ_DIR)\adler32.obj \
	$(OBJ_DIR)\crc32.obj \
	$(OBJ_DIR)\zutil.obj

HDRS = repo_ipc.h repo_feed.h $(HTTPS_DIR)\ce_https.h

all: makedirs $(EXE_OUT)
	@echo.
	@echo reposd EXE: $(EXE_OUT)

makedirs:
	@if not exist "$(OUT_DIR)" mkdir "$(OUT_DIR)"
	@if not exist "$(OBJ_DIR)" mkdir "$(OBJ_DIR)"

$(EXE_OUT): $(ALL_OBJS)
	$(LINK) /nologo $(CE_LFLAGS) /ENTRY:wWinMainCRTStartup /OUT:$(EXE_OUT) $(ALL_OBJS) $(LIBS)

clean:
	@if exist "$(OUT_DIR)" rmdir /s /q "$(OUT_DIR)"

$(OBJ_DIR)\reposd.obj: reposd.cpp $(HDRS)
	$(CC) $(WOLF_CFLAGS) /Fo"$(OBJ_DIR)\reposd.obj" /c reposd.cpp

$(OBJ_DIR)\repo_feed.obj: repo_feed.c $(HDRS)
	$(CC) $(WOLF_CFLAGS) /Fo"$(OBJ_DIR)\repo_feed.obj" /c repo_feed.c

$(OBJ_DIR)\ce_https.obj: $(HTTPS_DIR)\ce_https.c $(HDRS)
	$(CC) $(WOLF_CFLAGS) /Fo"$(OBJ_DIR)\ce_https.obj" /c $(HTTPS_DIR)\ce_https.c

$(OBJ_DIR)\ce_tls_ctx.obj: $(HTTPS_DIR)\ce_tls_ctx.c $(HDRS)
	$(CC) $(WOLF_CFLAGS) /Fo"$(OBJ_DIR)\ce_tls_ctx.obj" /c $(HTTPS_DIR)\ce_tls_ctx.c

$(OBJ_DIR)\ce_ca_bundle.obj: $(CEC)\src\ce_ca_bundle.c $(HDRS)
	$(CC) $(WOLF_CFLAGS) /Fo"$(OBJ_DIR)\ce_ca_bundle.obj" /c $(CEC)\src\ce_ca_bundle.c

$(OBJ_DIR)\unzip.obj: $(MZ)\unzip.c
	$(CC) $(ZLIB_CFLAGS) /Fo"$(OBJ_DIR)\unzip.obj" /c $(MZ)\unzip.c

$(OBJ_DIR)\ioapi.obj: $(MZ)\ioapi.c
	$(CC) $(ZLIB_CFLAGS) /Fo"$(OBJ_DIR)\ioapi.obj" /c $(MZ)\ioapi.c

$(OBJ_DIR)\repo_ceio.obj: repo_ceio.c repo_ceio.h
	$(CC) $(ZLIB_CFLAGS) /Fo"$(OBJ_DIR)\repo_ceio.obj" /c repo_ceio.c

# The canonical enabled.json codec (mod-runtime), shared with zuxhook.dll.
# Self-contained (windows.h + CRT only), so it needs no ce/wolf/zlib flags.
$(OBJ_DIR)\enabled_set.obj: $(MR)\enabled_set.c $(MR)\enabled_set.h
	$(CC) $(CE_CFLAGS) /I"$(MR)" /Fo"$(OBJ_DIR)\enabled_set.obj" /c $(MR)\enabled_set.c

# The shared ModsJson tokenizer + arena (mod-runtime), the same parser zuxhook uses
# for manifests; repo_feed.c parses the feed with it. Self-contained (windows.h/CRT).
$(OBJ_DIR)\mods_json.obj: $(MR)\mods_json.c $(MR)\mods_json.h $(MR)\mods_arena.h
	$(CC) $(CE_CFLAGS) /I"$(MR)" /Fo"$(OBJ_DIR)\mods_json.obj" /c $(MR)\mods_json.c

$(OBJ_DIR)\mods_arena.obj: $(MR)\mods_arena.c $(MR)\mods_arena.h
	$(CC) $(CE_CFLAGS) /I"$(MR)" /Fo"$(OBJ_DIR)\mods_arena.obj" /c $(MR)\mods_arena.c

$(OBJ_DIR)\inflate.obj: $(ZLIB)\inflate.c
	$(CC) $(ZLIB_CFLAGS) /Fo"$(OBJ_DIR)\inflate.obj" /c $(ZLIB)\inflate.c

$(OBJ_DIR)\inftrees.obj: $(ZLIB)\inftrees.c
	$(CC) $(ZLIB_CFLAGS) /Fo"$(OBJ_DIR)\inftrees.obj" /c $(ZLIB)\inftrees.c

$(OBJ_DIR)\inffast.obj: $(ZLIB)\inffast.c
	$(CC) $(ZLIB_CFLAGS) /Fo"$(OBJ_DIR)\inffast.obj" /c $(ZLIB)\inffast.c

$(OBJ_DIR)\adler32.obj: $(ZLIB)\adler32.c
	$(CC) $(ZLIB_CFLAGS) /Fo"$(OBJ_DIR)\adler32.obj" /c $(ZLIB)\adler32.c

$(OBJ_DIR)\crc32.obj: $(ZLIB)\crc32.c
	$(CC) $(ZLIB_CFLAGS) /Fo"$(OBJ_DIR)\crc32.obj" /c $(ZLIB)\crc32.c

$(OBJ_DIR)\zutil.obj: $(ZLIB)\zutil.c
	$(CC) $(ZLIB_CFLAGS) /Fo"$(OBJ_DIR)\zutil.obj" /c $(ZLIB)\zutil.c
