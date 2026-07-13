# NMake makefile for test_innertube_search: device validation for ce_innertube_search.
# Links ce_https (transport) + ce_innertube (client) + wolfSSL + ws2. Daemon entry
# "RunDaemon" (nativeapp opcode 21); --arg "<query>".

PL_ROOT   = ..\src
CEC       = ..\..\..\src\ce-common
HTTPS_DIR = $(CEC)\src\ce_https
IT_DIR    = ..\src\ce_innertube
OUT_DIR   = ..\out\test_innertube_search
OBJ_DIR   = $(OUT_DIR)\obj
DLL_OUT   = $(OUT_DIR)\test_innertube_search.dll

CC   = $(CE_CC)
LINK = $(CE_LINK)

PL_CFLAGS = $(CE_CFLAGS) \
	/DWOLFSSL_USER_SETTINGS \
	/FI"user_settings.h" \
	/I"$(CEC)\deps" \
	/I"$(CEC)\deps\wolfssl" \
	/I"$(HTTPS_DIR)" \
	/I"$(CEC)\src" \
	/I"$(IT_DIR)"

LIBS = \
	$(CEC)\out\wolfssl\wolfssl_ce_arm.lib \
	coredll.lib corelibc.lib ws2.lib

ALL_OBJS = \
	$(OBJ_DIR)\test_innertube_search.obj \
	$(OBJ_DIR)\ce_https.obj \
	$(OBJ_DIR)\ce_tls_ctx.obj \
	$(OBJ_DIR)\ce_ca_bundle.obj \
	$(OBJ_DIR)\ce_innertube.obj

all: makedirs $(DLL_OUT)
	@echo.
	@echo test_innertube_search DLL: $(DLL_OUT)

makedirs:
	@if not exist "$(OUT_DIR)" mkdir "$(OUT_DIR)"
	@if not exist "$(OBJ_DIR)" mkdir "$(OBJ_DIR)"

$(DLL_OUT): $(ALL_OBJS)
	$(LINK) /nologo $(CE_LFLAGS) /DLL /OUT:$(DLL_OUT) $(ALL_OBJS) $(LIBS)

clean:
	@if exist "$(OUT_DIR)" rmdir /s /q "$(OUT_DIR)"

$(OBJ_DIR)\test_innertube_search.obj: $(PL_ROOT)\test_innertube_search.cpp
	$(CC) $(PL_CFLAGS) /Fo"$(OBJ_DIR)\test_innertube_search.obj" /c $(PL_ROOT)\test_innertube_search.cpp

$(OBJ_DIR)\ce_https.obj: $(HTTPS_DIR)\ce_https.c
	$(CC) $(PL_CFLAGS) /Fo"$(OBJ_DIR)\ce_https.obj" /c $(HTTPS_DIR)\ce_https.c

$(OBJ_DIR)\ce_tls_ctx.obj: $(HTTPS_DIR)\ce_tls_ctx.c
	$(CC) $(PL_CFLAGS) /Fo"$(OBJ_DIR)\ce_tls_ctx.obj" /c $(HTTPS_DIR)\ce_tls_ctx.c

$(OBJ_DIR)\ce_ca_bundle.obj: $(CEC)\src\ce_ca_bundle.c
	$(CC) $(PL_CFLAGS) /Fo"$(OBJ_DIR)\ce_ca_bundle.obj" /c $(CEC)\src\ce_ca_bundle.c

$(OBJ_DIR)\ce_innertube.obj: $(IT_DIR)\ce_innertube.c
	$(CC) $(PL_CFLAGS) /Fo"$(OBJ_DIR)\ce_innertube.obj" /c $(IT_DIR)\ce_innertube.c
