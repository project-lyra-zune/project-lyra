# NMake makefile for the "Custom Background" mod's gemstone DLL.
# Loaded into gemstone via the modkit load_module action (init CustomBackgroundInstall).
# Output staged to the mod root as custom-background.dll.

SRC_DIR  = ..\src
# The Lyra SDK: lyra.h + lyra_client.c, all this mod needs from the platform.
# Override LYRA_SDK to build against an SDK unpacked elsewhere.
!IFNDEF LYRA_SDK
LYRA_SDK = ..\..\..\sdk
!ENDIF
CEC      = ..\..\..\src\ce-common
OUT_DIR  = ..\out\custom_background
OBJ_DIR  = $(OUT_DIR)\obj
DLL_OUT  = $(OUT_DIR)\custom-background.dll
STAGED   = ..\custom-background.dll

CC   = $(CE_CC)
LINK = $(CE_LINK)
INCS = /I"$(LYRA_SDK)\include" /I"$(CEC)\src\ce_log"
LIBS = coredll.lib corelibc.lib ole32.lib

ALL_OBJS = $(OBJ_DIR)\custom_background.obj $(OBJ_DIR)\ce_log.obj $(OBJ_DIR)\lyra_client.obj

all: makedirs $(DLL_OUT)
	@copy /y "$(DLL_OUT)" "$(STAGED)" >nul
	@echo custom_background DLL staged: $(STAGED)

makedirs:
	@if not exist "$(OUT_DIR)" mkdir "$(OUT_DIR)"
	@if not exist "$(OBJ_DIR)" mkdir "$(OBJ_DIR)"

$(DLL_OUT): $(ALL_OBJS)
	$(LINK) /nologo $(CE_LFLAGS) /DLL /OUT:$(DLL_OUT) $(ALL_OBJS) $(LIBS)

clean:
	@if exist "$(OUT_DIR)" rmdir /s /q "$(OUT_DIR)"
	@if exist "$(STAGED)" del /q "$(STAGED)"

$(OBJ_DIR)\custom_background.obj: $(SRC_DIR)\custom_background.cpp
	$(CC) $(CE_CFLAGS) $(INCS) /Fo"$(OBJ_DIR)\custom_background.obj" /c $(SRC_DIR)\custom_background.cpp

$(OBJ_DIR)\lyra_client.obj: $(LYRA_SDK)\src\lyra_client.c
	$(CC) $(CE_CFLAGS) $(INCS) /Fo"$(OBJ_DIR)\lyra_client.obj" /c $(LYRA_SDK)\src\lyra_client.c

$(OBJ_DIR)\ce_log.obj: $(CEC)\src\ce_log\ce_log.c
	$(CC) $(CE_CFLAGS) $(INCS) /Fo"$(OBJ_DIR)\ce_log.obj" /c $(CEC)\src\ce_log\ce_log.c
