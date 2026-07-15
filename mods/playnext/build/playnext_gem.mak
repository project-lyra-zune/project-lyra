# NMake makefile for the "Play Next" platform mod's gemstone DLL.
# Loaded into gemstone via the modkit load_module action (init PlayNextInstall).
# Output staged to lyra/platform/playnext/playnext.dll.

SRC_DIR  = ..\src
KC_DIR   = ..\..\..\src\kerncore
OUT_DIR  = ..\out\playnext_gem
OBJ_DIR  = $(OUT_DIR)\obj
DLL_OUT  = $(OUT_DIR)\playnext.dll
STAGED   = ..\playnext.dll

CC   = $(CE_CC)
LINK = $(CE_LINK)
INCS = /I"$(KC_DIR)"
LIBS = coredll.lib corelibc.lib toolhelp.lib

ALL_OBJS = $(OBJ_DIR)\playnext_gem.obj $(OBJ_DIR)\playnext_queue.obj $(OBJ_DIR)\kerncore.obj

all: makedirs $(DLL_OUT)
	@copy /y "$(DLL_OUT)" "$(STAGED)" >nul
	@echo playnext_gem DLL staged: $(STAGED)

makedirs:
	@if not exist "$(OUT_DIR)" mkdir "$(OUT_DIR)"
	@if not exist "$(OBJ_DIR)" mkdir "$(OBJ_DIR)"

$(DLL_OUT): $(ALL_OBJS)
	$(LINK) /nologo $(CE_LFLAGS) /DLL /OUT:$(DLL_OUT) $(ALL_OBJS) $(LIBS)

clean:
	@if exist "$(OUT_DIR)" rmdir /s /q "$(OUT_DIR)"
	@if exist "$(STAGED)" del /q "$(STAGED)"

$(OBJ_DIR)\playnext_gem.obj: $(SRC_DIR)\playnext_gem.cpp
	$(CC) $(CE_CFLAGS) $(INCS) /Fo"$(OBJ_DIR)\playnext_gem.obj" /c $(SRC_DIR)\playnext_gem.cpp

$(OBJ_DIR)\playnext_queue.obj: $(SRC_DIR)\playnext_queue.c
	$(CC) $(CE_CFLAGS) $(INCS) /Fo"$(OBJ_DIR)\playnext_queue.obj" /c $(SRC_DIR)\playnext_queue.c

$(OBJ_DIR)\kerncore.obj: $(KC_DIR)\kerncore.c
	$(CC) $(CE_CFLAGS) $(INCS) /Fo"$(OBJ_DIR)\kerncore.obj" /c $(KC_DIR)\kerncore.c
