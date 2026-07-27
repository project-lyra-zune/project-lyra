# NMake makefile for lyraboot.exe. Links no TLS: the page carries this binary as
# base64, so size is the budget, and boot_pin.h's digest replaces the transport.

SRC     = ..
CEC     = $(SRC)\ce-common
ZLIB    = $(SRC)\zlib
MZ      = $(SRC)\zlib\contrib\minizip
ZMOD    = $(SRC)\zmod
MR      = $(SRC)\mod-runtime
SHARED  = $(SRC)\shared
OUT_DIR = out
OBJ_DIR = $(OUT_DIR)\obj
EXE_OUT = $(OUT_DIR)\lyraboot.exe

CC   = $(CE_CC)
LINK = $(CE_LINK)

APP_CFLAGS = $(CE_CFLAGS) \
	/I"$(CEC)\src\ce_log" \
	/I"$(ZLIB)" \
	/I"$(MZ)" \
	/I"$(ZMOD)" \
	/I"$(ZMOD)\ceshim" \
	/I"$(MR)" \
	/I"$(SHARED)" \
	/I"."

# NOT Z_SOLO: minizip's inflate depends on zlib's default allocator. See reposd.mak.
ZLIB_CFLAGS = $(CE_CFLAGS) \
	/DNO_FSEEKO /DZLIB_CONST /DSTDC /DUSE_FILE32API \
	/FI"ce_extras.h" /I"$(ZMOD)\ceshim" \
	/I"$(ZLIB)" /I"$(MZ)" /I"$(ZMOD)" /I"$(MR)" \
	/wd4244 /wd4267 /wd4127 /wd4146

LIBS = coredll.lib corelibc.lib ws2.lib

ALL_OBJS = \
	$(OBJ_DIR)\lyraboot.obj \
	$(OBJ_DIR)\boot_http.obj \
	$(OBJ_DIR)\device_reboot.obj \
	$(OBJ_DIR)\zmod_io.obj \
	$(OBJ_DIR)\zmod_extract.obj \
	$(OBJ_DIR)\zmod_sha256.obj \
	$(OBJ_DIR)\unzip.obj \
	$(OBJ_DIR)\ioapi.obj \
	$(OBJ_DIR)\inflate.obj \
	$(OBJ_DIR)\inftrees.obj \
	$(OBJ_DIR)\inffast.obj \
	$(OBJ_DIR)\adler32.obj \
	$(OBJ_DIR)\crc32.obj \
	$(OBJ_DIR)\zutil.obj \
	$(OBJ_DIR)\ce_log.obj

all: makedirs $(EXE_OUT)
	@echo.
	@echo lyraboot EXE: $(EXE_OUT)

makedirs:
	@if not exist "$(OUT_DIR)" mkdir "$(OUT_DIR)"
	@if not exist "$(OBJ_DIR)" mkdir "$(OBJ_DIR)"

$(EXE_OUT): $(ALL_OBJS)
	$(LINK) /nologo $(CE_LFLAGS) /ENTRY:wWinMainCRTStartup /OUT:$(EXE_OUT) $(ALL_OBJS) $(LIBS)

clean:
	@if exist "$(OUT_DIR)" rmdir /s /q "$(OUT_DIR)"

$(OBJ_DIR)\lyraboot.obj: lyraboot.cpp boot_http.h boot_pin.h
	$(CC) $(APP_CFLAGS) /Fo"$(OBJ_DIR)\lyraboot.obj" /c lyraboot.cpp

$(OBJ_DIR)\boot_http.obj: boot_http.c boot_http.h
	$(CC) $(APP_CFLAGS) /Fo"$(OBJ_DIR)\boot_http.obj" /c boot_http.c

$(OBJ_DIR)\device_reboot.obj: $(SHARED)\device_reboot.cpp $(SHARED)\device_reboot.h
	$(CC) $(APP_CFLAGS) /Fo"$(OBJ_DIR)\device_reboot.obj" /c $(SHARED)\device_reboot.cpp

$(OBJ_DIR)\zmod_io.obj: $(ZMOD)\zmod_io.c $(ZMOD)\zmod_io.h
	$(CC) $(ZLIB_CFLAGS) /Fo"$(OBJ_DIR)\zmod_io.obj" /c $(ZMOD)\zmod_io.c

$(OBJ_DIR)\zmod_extract.obj: $(ZMOD)\zmod_extract.c $(ZMOD)\zmod_extract.h
	$(CC) $(ZLIB_CFLAGS) /Fo"$(OBJ_DIR)\zmod_extract.obj" /c $(ZMOD)\zmod_extract.c

$(OBJ_DIR)\zmod_sha256.obj: $(ZMOD)\zmod_sha256.c $(ZMOD)\zmod_sha256.h
	$(CC) $(CE_CFLAGS) /I"$(ZMOD)" /Fo"$(OBJ_DIR)\zmod_sha256.obj" /c $(ZMOD)\zmod_sha256.c

$(OBJ_DIR)\unzip.obj: $(MZ)\unzip.c
	$(CC) $(ZLIB_CFLAGS) /Fo"$(OBJ_DIR)\unzip.obj" /c $(MZ)\unzip.c

$(OBJ_DIR)\ioapi.obj: $(MZ)\ioapi.c
	$(CC) $(ZLIB_CFLAGS) /Fo"$(OBJ_DIR)\ioapi.obj" /c $(MZ)\ioapi.c

$(OBJ_DIR)\inflate.obj: $(ZLIB)\inflate.c
	$(CC) $(ZLIB_CFLAGS) /Fo"$(OBJ_DIR)\inflate.obj" /c $(ZLIB)\inflate.c

$(OBJ_DIR)\inftrees.obj: $(ZLIB)\inftrees.c
	$(CC) $(ZLIB_CFLAGS) /Fo"$(OBJ_DIR)\inftrees.obj" /c $(ZLIB)\inftrees.c

$(OBJ_DIR)\inffast.obj: $(ZLIB)\inffast.c
	$(CC) $(ZLIB_CFLAGS) /Fo"$(OBJ_DIR)\inffast.obj" /c $(ZLIB)\inffast.c

$(OBJ_DIR)\adler32.obj: $(ZLIB)\adler32.c
	$(CC) $(ZLIB_CFLAGS) /Fo"$(OBJ_DIR)\adler32.obj" /c $(ZLIB)\adler32.c

$(OBJ_DIR)\crc32.obj: $(ZLIB)\crc32.c
	$(CC) $(ZLIB_CFLAGS) /Fo"$(OBJ_DIR)\crc32.obj" /c $(ZLIB)\crc32.c

$(OBJ_DIR)\zutil.obj: $(ZLIB)\zutil.c
	$(CC) $(ZLIB_CFLAGS) /Fo"$(OBJ_DIR)\zutil.obj" /c $(ZLIB)\zutil.c

$(OBJ_DIR)\ce_log.obj: $(CEC)\src\ce_log\ce_log.c
	$(CC) $(APP_CFLAGS) /Fo"$(OBJ_DIR)\ce_log.obj" /c $(CEC)\src\ce_log\ce_log.c
