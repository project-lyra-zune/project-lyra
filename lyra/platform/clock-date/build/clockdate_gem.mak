# NMake makefile for clockdate_gem (the clock-date platform mod's gemstone DLL).
# Loaded into gemstone via the modkit load_module action (init ClockDateInstall);
# registers the GemSettingSetDateScene XUI scene class. Output staged to
# lyra/platform/clock-date/clockdate.dll.

SRC_DIR  = ..\src
OUT_DIR  = ..\out\clockdate_gem
OBJ_DIR  = $(OUT_DIR)\obj
DLL_OUT  = $(OUT_DIR)\clockdate.dll
STAGED   = ..\clockdate.dll

CC   = $(CE_CC)
LINK = $(CE_LINK)
LIBS = coredll.lib corelibc.lib

ALL_OBJS = $(OBJ_DIR)\clockdate_gem.obj

all: makedirs $(DLL_OUT)
	@copy /y "$(DLL_OUT)" "$(STAGED)" >nul
	@echo clockdate_gem DLL staged: $(STAGED)

makedirs:
	@if not exist "$(OUT_DIR)" mkdir "$(OUT_DIR)"
	@if not exist "$(OBJ_DIR)" mkdir "$(OBJ_DIR)"

$(DLL_OUT): $(ALL_OBJS)
	$(LINK) /nologo $(CE_LFLAGS) /DLL /OUT:$(DLL_OUT) $(ALL_OBJS) $(LIBS)

clean:
	@if exist "$(OUT_DIR)" rmdir /s /q "$(OUT_DIR)"
	@if exist "$(STAGED)" del /q "$(STAGED)"

$(OBJ_DIR)\clockdate_gem.obj: $(SRC_DIR)\clockdate_gem.cpp
	$(CC) $(CE_CFLAGS) /Fo"$(OBJ_DIR)\clockdate_gem.obj" /c $(SRC_DIR)\clockdate_gem.cpp
