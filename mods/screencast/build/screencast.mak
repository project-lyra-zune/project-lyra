# NMake makefile for screencast (WiFi screen mirror). Two CE-ARM artifacts from
# one shared engine core:
#   screencastd.exe        the shipped artifact, boot-spawned by the screencast
#                          mod (daemons capability). Picker-driven, toggle-gated.
#   plugin-screencast.dll  the reboot-free iteration vehicle and the SDK's
#                          screen-mirror plugin example (nativeapp RunDaemon).
#                          Runs both frontends; never packaged in the .zmod.
# Links the ce-common ce_image lib (imaging.dll JPEG encode), the shared
# mod-runtime (mod_state + mod_list_channel picker) and the canonical kerncore.

CEC      = ..\..\..\src\ce-common
KC       = ..\..\..\src\kerncore
MR       = ..\..\..\src\mod-runtime
PL_ROOT  = ..\src\screencast
OUT_DIR  = ..\out\screencast
OBJ_DIR  = $(OUT_DIR)\obj
DLL_OUT  = $(OUT_DIR)\plugin-screencast.dll
EXE_OUT  = $(OUT_DIR)\screencastd.exe
STAGED   = ..\screencastd.exe

CC   = $(CE_CC)
LINK = $(CE_LINK)

INCS = /I"$(KC)" /I"$(MR)" /I"$(CEC)\deps\ce_image" /I"$(PL_ROOT)"

# C++ TUs (engine + the two entries): no RTTI, EH on for the imaging COM paths.
CPP_CFLAGS = $(CE_CFLAGS) /EHsc /GR- /D_CRT_SECURE_NO_WARNINGS /D_CRT_SECURE_NO_DEPRECATE $(INCS)

# Plain-C TUs (frontends, mod-runtime, kerncore).
C_CFLAGS = $(CE_CFLAGS) /D_CRT_SECURE_NO_WARNINGS /D_CRT_SECURE_NO_DEPRECATE $(INCS)

LIBS = \
	$(CEC)\out\ce_image\ce_image_ce_arm.lib \
	coredll.lib corelibc.lib ws2.lib ole32.lib

# Engine + frontends + shared runtime, linked into both entries.
CORE_OBJS = \
	$(OBJ_DIR)\screencast_engine.obj \
	$(OBJ_DIR)\screencast_http.obj \
	$(OBJ_DIR)\screencast_delta.obj \
	$(OBJ_DIR)\screencast_serve.obj \
	$(OBJ_DIR)\mod_state.obj \
	$(OBJ_DIR)\mod_list_channel.obj \
	$(OBJ_DIR)\kerncore.obj

EXE_OBJS = $(CORE_OBJS) $(OBJ_DIR)\screencast_main.obj
DLL_OBJS = $(CORE_OBJS) $(OBJ_DIR)\plugin_screencast.obj

all: makedirs $(EXE_OUT) $(DLL_OUT)
	@copy /y "$(EXE_OUT)" "$(STAGED)" >nul
	@echo.
	@echo screencastd EXE staged: $(STAGED)
	@echo plugin-screencast DLL:  $(DLL_OUT)

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

$(OBJ_DIR)\screencast_engine.obj: $(PL_ROOT)\screencast_engine.cpp
	$(CC) $(CPP_CFLAGS) /Fo"$(OBJ_DIR)\screencast_engine.obj" /c $(PL_ROOT)\screencast_engine.cpp

$(OBJ_DIR)\screencast_main.obj: $(PL_ROOT)\screencast_main.cpp
	$(CC) $(CPP_CFLAGS) /Fo"$(OBJ_DIR)\screencast_main.obj" /c $(PL_ROOT)\screencast_main.cpp

$(OBJ_DIR)\plugin_screencast.obj: $(PL_ROOT)\plugin_screencast.cpp
	$(CC) $(CPP_CFLAGS) /Fo"$(OBJ_DIR)\plugin_screencast.obj" /c $(PL_ROOT)\plugin_screencast.cpp

$(OBJ_DIR)\screencast_http.obj: $(PL_ROOT)\screencast_http.cpp
	$(CC) $(CPP_CFLAGS) /Fo"$(OBJ_DIR)\screencast_http.obj" /c $(PL_ROOT)\screencast_http.cpp

$(OBJ_DIR)\screencast_delta.obj: $(PL_ROOT)\screencast_delta.cpp
	$(CC) $(CPP_CFLAGS) /Fo"$(OBJ_DIR)\screencast_delta.obj" /c $(PL_ROOT)\screencast_delta.cpp

$(OBJ_DIR)\screencast_serve.obj: $(PL_ROOT)\screencast_serve.cpp
	$(CC) $(CPP_CFLAGS) /Fo"$(OBJ_DIR)\screencast_serve.obj" /c $(PL_ROOT)\screencast_serve.cpp

$(OBJ_DIR)\mod_state.obj: $(MR)\mod_state.c
	$(CC) $(C_CFLAGS) /Fo"$(OBJ_DIR)\mod_state.obj" /c $(MR)\mod_state.c

$(OBJ_DIR)\mod_list_channel.obj: $(MR)\mod_list_channel.c
	$(CC) $(C_CFLAGS) /Fo"$(OBJ_DIR)\mod_list_channel.obj" /c $(MR)\mod_list_channel.c

$(OBJ_DIR)\kerncore.obj: $(KC)\kerncore.c
	$(CC) $(C_CFLAGS) /Fo"$(OBJ_DIR)\kerncore.obj" /c $(KC)\kerncore.c
