# NMake makefile for ce_dshow_enum_plugin: drive DirectShow (CLSID_FilterGraph ->
# RenderFile -> Run) directly. ole32 loaded at runtime, so no import libs beyond
# coredll. Daemon entry "RunDaemon" (nativeapp opcode 21).

PL_ROOT = ..\src
OUT_DIR = ..\out\ce_dshow_enum_plugin
OBJ_DIR = $(OUT_DIR)\obj
DLL_OUT = $(OUT_DIR)\ce_dshow_enum_plugin.dll

CC   = $(CE_CC)
LINK = $(CE_LINK)

PL_CFLAGS = $(CE_CFLAGS)
LIBS = coredll.lib corelibc.lib

all: makedirs $(DLL_OUT)
	@echo.
	@echo ce_dshow_enum_plugin DLL: $(DLL_OUT)

makedirs:
	@if not exist "$(OUT_DIR)" mkdir "$(OUT_DIR)"
	@if not exist "$(OBJ_DIR)" mkdir "$(OBJ_DIR)"

$(DLL_OUT): $(OBJ_DIR)\ce_dshow_enum_plugin.obj
	$(LINK) /nologo $(CE_LFLAGS) /DLL /OUT:$(DLL_OUT) $(OBJ_DIR)\ce_dshow_enum_plugin.obj $(LIBS)

clean:
	@if exist "$(OUT_DIR)" rmdir /s /q "$(OUT_DIR)"

$(OBJ_DIR)\ce_dshow_enum_plugin.obj: $(PL_ROOT)\ce_dshow_enum_plugin.cpp
	$(CC) $(PL_CFLAGS) /Fo"$(OBJ_DIR)\ce_dshow_enum_plugin.obj" /c $(PL_ROOT)\ce_dshow_enum_plugin.cpp
