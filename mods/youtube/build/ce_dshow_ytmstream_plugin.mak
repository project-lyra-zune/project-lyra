# NMake makefile for ce_dshow_ytmstream_plugin (progressive YouTube Music).
# ANDROID_VR /player -> Range-walk moofs -> incremental moov -> PlaySongFromFile
# while streaming the mdat payloads. Links ce_https (keep-alive ranged GET) +
# ce_mp4_defrag (incremental builder) + wolfSSL + ws2; zdksystem at runtime.
# Loaded via opcode-18 (entry "Activate").

PL_ROOT   = ..\src
CEC       = ..\..\..\src\ce-common
HTTPS_DIR = $(CEC)\src\ce_https
MP4_DIR   = ..\src\ce_mp4
IT_DIR    = ..\src\ce_innertube
OUT_DIR   = ..\out\ce_dshow_ytmstream_plugin
OBJ_DIR   = $(OUT_DIR)\obj
DLL_OUT   = $(OUT_DIR)\ce_dshow_ytmstream_plugin.dll
STAGED    = ..\ce_ytmstream.dll

CC   = $(CE_CC)
LINK = $(CE_LINK)

PL_CFLAGS = $(CE_CFLAGS) \
	/DWOLFSSL_USER_SETTINGS \
	/FI"user_settings.h" \
	/I"$(CEC)\deps" \
	/I"$(CEC)\deps\wolfssl" \
	/I"$(HTTPS_DIR)" \
	/I"$(CEC)\src" \
	/I"$(MP4_DIR)" \
	/I"$(IT_DIR)"

LIBS = \
	$(CEC)\out\wolfssl\wolfssl_ce_arm.lib \
	coredll.lib corelibc.lib ws2.lib

ALL_OBJS = \
	$(OBJ_DIR)\ce_dshow_ytmstream_plugin.obj \
	$(OBJ_DIR)\ce_https.obj \
	$(OBJ_DIR)\ce_tls_ctx.obj \
	$(OBJ_DIR)\ce_ca_bundle.obj \
	$(OBJ_DIR)\ce_mp4_defrag.obj \
	$(OBJ_DIR)\ce_innertube.obj

all: makedirs $(DLL_OUT)
	@copy /y "$(DLL_OUT)" "$(STAGED)" >nul
	@echo.
	@echo ce_dshow_ytmstream_plugin DLL staged: $(STAGED)

makedirs:
	@if not exist "$(OUT_DIR)" mkdir "$(OUT_DIR)"
	@if not exist "$(OBJ_DIR)" mkdir "$(OBJ_DIR)"

$(DLL_OUT): $(ALL_OBJS)
	$(LINK) /nologo $(CE_LFLAGS) /DLL /DEF:$(PL_ROOT)\ce_dshow_ytmstream_plugin.def /OUT:$(DLL_OUT) $(ALL_OBJS) $(LIBS)

clean:
	@if exist "$(OUT_DIR)" rmdir /s /q "$(OUT_DIR)"
	@if exist "$(STAGED)" del /q "$(STAGED)"

$(OBJ_DIR)\ce_dshow_ytmstream_plugin.obj: $(PL_ROOT)\ce_dshow_ytmstream_plugin.cpp
	$(CC) $(PL_CFLAGS) /Fo"$(OBJ_DIR)\ce_dshow_ytmstream_plugin.obj" /c $(PL_ROOT)\ce_dshow_ytmstream_plugin.cpp

$(OBJ_DIR)\ce_https.obj: $(HTTPS_DIR)\ce_https.c
	$(CC) $(PL_CFLAGS) /Fo"$(OBJ_DIR)\ce_https.obj" /c $(HTTPS_DIR)\ce_https.c

$(OBJ_DIR)\ce_tls_ctx.obj: $(HTTPS_DIR)\ce_tls_ctx.c
	$(CC) $(PL_CFLAGS) /Fo"$(OBJ_DIR)\ce_tls_ctx.obj" /c $(HTTPS_DIR)\ce_tls_ctx.c

$(OBJ_DIR)\ce_ca_bundle.obj: $(CEC)\src\ce_ca_bundle.c
	$(CC) $(PL_CFLAGS) /Fo"$(OBJ_DIR)\ce_ca_bundle.obj" /c $(CEC)\src\ce_ca_bundle.c

$(OBJ_DIR)\ce_mp4_defrag.obj: $(MP4_DIR)\ce_mp4_defrag.cpp
	$(CC) $(PL_CFLAGS) /Fo"$(OBJ_DIR)\ce_mp4_defrag.obj" /c $(MP4_DIR)\ce_mp4_defrag.cpp

$(OBJ_DIR)\ce_innertube.obj: $(IT_DIR)\ce_innertube.c
	$(CC) $(PL_CFLAGS) /Fo"$(OBJ_DIR)\ce_innertube.obj" /c $(IT_DIR)\ce_innertube.c
