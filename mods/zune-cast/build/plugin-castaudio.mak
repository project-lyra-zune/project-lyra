# NMake makefile for zune-cast (Chromecast audio cast). Two CE-ARM artifacts
# from one shared engine core:
#   castd.exe             the shipped artifact, spawned at boot by the zune-cast
#                         mod (daemons capability).
#   plugin-castaudio.dll  the reboot-free iteration vehicle and the SDK's
#                         advanced RunDaemon plugin example, loaded by the
#                         nativeapp daemon (opcode 21, entry "RunDaemon"). Never
#                         packaged in the .zmod.
# Both link the ce-common static libs (wolfSSL + ce_image) and the canonical
# kerncore from the monorepo source tree (single source of truth; do not vendor).

CEC      = ..\..\..\src\ce-common
KC       = ..\..\..\src\kerncore
MR       = ..\..\..\src\mod-runtime
PL_ROOT  = ..\src\plugin-castaudio
OUT_DIR  = ..\out\plugin-castaudio
OBJ_DIR  = $(OUT_DIR)\obj
DLL_OUT  = $(OUT_DIR)\plugin-castaudio.dll
EXE_OUT  = $(OUT_DIR)\castd.exe
STAGED   = ..\castd.exe

CC   = $(CE_CC)
LINK = $(CE_LINK)

# Base flags for C++ TUs that don't touch wolfSSL.
BASE_CFLAGS = $(CE_CFLAGS) /EHsc /GR- /D_CRT_SECURE_NO_WARNINGS /D_CRT_SECURE_NO_DEPRECATE \
	/I"$(KC)" /I"$(MR)" /I"$(PL_ROOT)"

# wolfSSL-consuming TUs additionally force-include user_settings.h and the
# wolfSSL headers (config must match how the lib was built). Also the ce_image
# header (native imaging.dll decode/scale/encode for album-art thumbnails).
TLS_CFLAGS = $(BASE_CFLAGS) /DWOLFSSL_USER_SETTINGS /FI"user_settings.h" \
	/I"$(CEC)\deps" /I"$(CEC)\deps\wolfssl" /I"$(CEC)\deps\ce_image"

# Plain-C TUs (no C++ EH): mod_state.c (shared), cast_channel.c and kerncore.c.
C_CFLAGS = $(CE_CFLAGS) /D_CRT_SECURE_NO_WARNINGS /D_CRT_SECURE_NO_DEPRECATE \
	/I"$(KC)" /I"$(MR)" /I"$(PL_ROOT)"

LIBS = \
	$(CEC)\out\wolfssl\wolfssl_ce_arm.lib \
	$(CEC)\out\ce_image\ce_image_ce_arm.lib \
	coredll.lib corelibc.lib ws2.lib ole32.lib

# Orchestration + media/cast/capture core, shared by both entries. The core
# publishes the cast status (mod_state), so mod_state.obj is shared too.
CORE_OBJS = \
	$(OBJ_DIR)\cast_core.obj \
	$(OBJ_DIR)\castv2_client.obj \
	$(OBJ_DIR)\http_media.obj \
	$(OBJ_DIR)\avp_capture.obj \
	$(OBJ_DIR)\mdns.obj \
	$(OBJ_DIR)\zdk.obj \
	$(OBJ_DIR)\zme.obj \
	$(OBJ_DIR)\mod_state.obj \
	$(OBJ_DIR)\mod_list_channel.obj \
	$(OBJ_DIR)\cast_channel.obj \
	$(OBJ_DIR)\kerncore.obj

DLL_OBJS = $(CORE_OBJS) $(OBJ_DIR)\plugin-castaudio.obj
EXE_OBJS = $(CORE_OBJS) $(OBJ_DIR)\cast_main.obj

all: makedirs $(EXE_OUT) $(DLL_OUT)
	@copy /y "$(EXE_OUT)" "$(STAGED)" >nul
	@echo.
	@echo castd EXE staged:    $(STAGED)
	@echo plugin-castaudio DLL: $(DLL_OUT)

makedirs:
	@if not exist "$(OUT_DIR)" mkdir "$(OUT_DIR)"
	@if not exist "$(OBJ_DIR)" mkdir "$(OBJ_DIR)"

$(EXE_OUT): $(EXE_OBJS)
	$(LINK) /nologo $(CE_LFLAGS) /ENTRY:wWinMainCRTStartup /OUT:$(EXE_OUT) $(EXE_OBJS) $(LIBS)

$(DLL_OUT): $(DLL_OBJS)
	$(LINK) /nologo $(CE_LFLAGS) /DLL /OUT:$(DLL_OUT) $(DLL_OBJS) $(LIBS)

clean:
	@if exist "$(OUT_DIR)" rmdir /s /q "$(OUT_DIR)"
	@if exist "$(STAGED)" del /q "$(STAGED)"

$(OBJ_DIR)\cast_core.obj: $(PL_ROOT)\cast_core.cpp
	$(CC) $(BASE_CFLAGS) /Fo"$(OBJ_DIR)\cast_core.obj" /c $(PL_ROOT)\cast_core.cpp

$(OBJ_DIR)\cast_main.obj: $(PL_ROOT)\cast_main.cpp
	$(CC) $(TLS_CFLAGS) /Fo"$(OBJ_DIR)\cast_main.obj" /c $(PL_ROOT)\cast_main.cpp

$(OBJ_DIR)\plugin-castaudio.obj: $(PL_ROOT)\plugin-castaudio.cpp
	$(CC) $(TLS_CFLAGS) /Fo"$(OBJ_DIR)\plugin-castaudio.obj" /c $(PL_ROOT)\plugin-castaudio.cpp

$(OBJ_DIR)\mod_state.obj: $(MR)\mod_state.c
	$(CC) $(C_CFLAGS) /Fo"$(OBJ_DIR)\mod_state.obj" /c $(MR)\mod_state.c

$(OBJ_DIR)\mod_list_channel.obj: $(MR)\mod_list_channel.c
	$(CC) $(C_CFLAGS) /Fo"$(OBJ_DIR)\mod_list_channel.obj" /c $(MR)\mod_list_channel.c

$(OBJ_DIR)\cast_channel.obj: $(PL_ROOT)\cast_channel.c
	$(CC) $(C_CFLAGS) /Fo"$(OBJ_DIR)\cast_channel.obj" /c $(PL_ROOT)\cast_channel.c

$(OBJ_DIR)\castv2_client.obj: $(PL_ROOT)\castv2_client.cpp
	$(CC) $(TLS_CFLAGS) /Fo"$(OBJ_DIR)\castv2_client.obj" /c $(PL_ROOT)\castv2_client.cpp

$(OBJ_DIR)\http_media.obj: $(PL_ROOT)\http_media.cpp
	$(CC) $(BASE_CFLAGS) /Fo"$(OBJ_DIR)\http_media.obj" /c $(PL_ROOT)\http_media.cpp

$(OBJ_DIR)\avp_capture.obj: $(PL_ROOT)\avp_capture.cpp
	$(CC) $(BASE_CFLAGS) /Fo"$(OBJ_DIR)\avp_capture.obj" /c $(PL_ROOT)\avp_capture.cpp

$(OBJ_DIR)\mdns.obj: $(PL_ROOT)\mdns.cpp
	$(CC) $(BASE_CFLAGS) /Fo"$(OBJ_DIR)\mdns.obj" /c $(PL_ROOT)\mdns.cpp

$(OBJ_DIR)\zdk.obj: $(PL_ROOT)\zdk.cpp
	$(CC) $(BASE_CFLAGS) /Fo"$(OBJ_DIR)\zdk.obj" /c $(PL_ROOT)\zdk.cpp

$(OBJ_DIR)\zme.obj: $(PL_ROOT)\zme.cpp
	$(CC) $(BASE_CFLAGS) /Fo"$(OBJ_DIR)\zme.obj" /c $(PL_ROOT)\zme.cpp

$(OBJ_DIR)\kerncore.obj: $(KC)\kerncore.c
	$(CC) $(C_CFLAGS) /Fo"$(OBJ_DIR)\kerncore.obj" /c $(KC)\kerncore.c
