# NMake makefile for ytsearchd.exe (the zune-yt search daemon).
# Spawned at boot by the youtube mod's `daemons` capability (CreateProcessW).
# Its own process + Winsock: runs ce_innertube_search off the gemstone UI thread,
# publishing results into the yt-search shared section. Links the ce-common stack
# (ce_innertube + ce_https + wolfSSL + ws2), like ce_dshow_ytmstream_plugin.

CEC       = ..\..\..\src\ce-common
HTTPS_DIR = $(CEC)\src\ce_https
IT_DIR    = ..\src\ce_innertube
PL_ROOT   = ..\src\ytsearchd
OUT_DIR   = ..\out\ytsearchd
OBJ_DIR   = $(OUT_DIR)\obj
EXE_OUT   = $(OUT_DIR)\ytsearchd.exe
STAGED    = ..\ytsearchd.exe

CC   = $(CE_CC)
LINK = $(CE_LINK)

PL_CFLAGS = $(CE_CFLAGS) \
	/DWOLFSSL_USER_SETTINGS \
	/FI"user_settings.h" \
	/I"$(CEC)\deps" \
	/I"$(CEC)\deps\wolfssl" \
	/I"$(HTTPS_DIR)" \
	/I"$(CEC)\src" \
	/I"$(IT_DIR)" \
	/I"$(PL_ROOT)"

LIBS = \
	$(CEC)\out\wolfssl\wolfssl_ce_arm.lib \
	coredll.lib corelibc.lib ws2.lib

ALL_OBJS = \
	$(OBJ_DIR)\ytsearchd.obj \
	$(OBJ_DIR)\ce_innertube.obj \
	$(OBJ_DIR)\ce_https.obj \
	$(OBJ_DIR)\ce_tls_ctx.obj \
	$(OBJ_DIR)\ce_ca_bundle.obj

# Project headers: every obj depends on them so a header-only change (e.g. the
# yt_search_ipc.h layout/version, or ce_innertube.h) forces recompilation. nmake
# has no implicit header tracking; without this a header edit silently relinks
# stale objs (it shipped a truncated-id build before this was added).
HDRS = \
	$(PL_ROOT)\yt_search_ipc.h \
	$(IT_DIR)\ce_innertube.h \
	$(HTTPS_DIR)\ce_https.h

all: makedirs $(EXE_OUT)
	@copy /y "$(EXE_OUT)" "$(STAGED)" >nul
	@echo.
	@echo ytsearchd EXE staged: $(STAGED)

makedirs:
	@if not exist "$(OUT_DIR)" mkdir "$(OUT_DIR)"
	@if not exist "$(OBJ_DIR)" mkdir "$(OBJ_DIR)"

$(EXE_OUT): $(ALL_OBJS)
	$(LINK) /nologo $(CE_LFLAGS) /ENTRY:wWinMainCRTStartup /OUT:$(EXE_OUT) $(ALL_OBJS) $(LIBS)

clean:
	@if exist "$(OUT_DIR)" rmdir /s /q "$(OUT_DIR)"
	@if exist "$(STAGED)" del /q "$(STAGED)"

$(OBJ_DIR)\ytsearchd.obj: $(PL_ROOT)\ytsearchd.cpp $(HDRS)
	$(CC) $(PL_CFLAGS) /Fo"$(OBJ_DIR)\ytsearchd.obj" /c $(PL_ROOT)\ytsearchd.cpp

$(OBJ_DIR)\ce_innertube.obj: $(IT_DIR)\ce_innertube.c $(HDRS)
	$(CC) $(PL_CFLAGS) /Fo"$(OBJ_DIR)\ce_innertube.obj" /c $(IT_DIR)\ce_innertube.c

$(OBJ_DIR)\ce_https.obj: $(HTTPS_DIR)\ce_https.c $(HDRS)
	$(CC) $(PL_CFLAGS) /Fo"$(OBJ_DIR)\ce_https.obj" /c $(HTTPS_DIR)\ce_https.c

$(OBJ_DIR)\ce_tls_ctx.obj: $(HTTPS_DIR)\ce_tls_ctx.c $(HDRS)
	$(CC) $(PL_CFLAGS) /Fo"$(OBJ_DIR)\ce_tls_ctx.obj" /c $(HTTPS_DIR)\ce_tls_ctx.c

$(OBJ_DIR)\ce_ca_bundle.obj: $(CEC)\src\ce_ca_bundle.c $(HDRS)
	$(CC) $(PL_CFLAGS) /Fo"$(OBJ_DIR)\ce_ca_bundle.obj" /c $(CEC)\src\ce_ca_bundle.c
