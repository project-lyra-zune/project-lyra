# NMake makefile for youtube_gem (the YouTube feature mod's gemstone DLL).
# Loaded into gemstone via the modkit load_module action (init YtInstall);
# registers the GemYtHub / GemYtResultsContentScene XUI scene classes. Search is
# out-of-process (ytsearchd.exe owns ce_innertube + the HTTPS stack), so this DLL
# does no networking: it talks to the daemon over the yt_search_ipc.h shared
# section + named events. Output staged to mods/youtube/youtube.dll.

PL_ROOT  = ..\src
IPC_DIR  = ..\src\ytsearchd
OUT_DIR  = ..\out\youtube_gem
OBJ_DIR  = $(OUT_DIR)\obj
DLL_OUT  = $(OUT_DIR)\youtube.dll
STAGED   = ..\youtube.dll

CC   = $(CE_CC)
LINK = $(CE_LINK)

PL_CFLAGS = $(CE_CFLAGS) \
	/I"$(IPC_DIR)"

LIBS = coredll.lib corelibc.lib

ALL_OBJS = \
	$(OBJ_DIR)\youtube_gem.obj

all: makedirs $(DLL_OUT)
	@copy /y "$(DLL_OUT)" "$(STAGED)" >nul
	@echo.
	@echo youtube_gem DLL staged: $(STAGED)

makedirs:
	@if not exist "$(OUT_DIR)" mkdir "$(OUT_DIR)"
	@if not exist "$(OBJ_DIR)" mkdir "$(OBJ_DIR)"

$(DLL_OUT): $(ALL_OBJS)
	$(LINK) /nologo $(CE_LFLAGS) /DLL /OUT:$(DLL_OUT) $(ALL_OBJS) $(LIBS)

clean:
	@if exist "$(OUT_DIR)" rmdir /s /q "$(OUT_DIR)"
	@if exist "$(STAGED)" del /q "$(STAGED)"

# Depend on the IPC header so a yt_search_ipc.h layout/version change forces a
# recompile; nmake does no implicit header tracking, and a stale relink once
# shipped a mismatched (old-layout) DLL against an updated daemon.
$(OBJ_DIR)\youtube_gem.obj: $(PL_ROOT)\youtube_gem.cpp $(IPC_DIR)\yt_search_ipc.h
	$(CC) $(PL_CFLAGS) /Fo"$(OBJ_DIR)\youtube_gem.obj" /c $(PL_ROOT)\youtube_gem.cpp
