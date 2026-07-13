# NMake makefile: ce_image (CE imaging.dll adapter) for Zune HD.
# Usage (from build\ after running env_setup.bat):
#   nmake /f ce_image.mak

CI_ROOT  = ..\deps\ce_image
OUT_DIR  = ..\out\ce_image
OBJ_DIR  = $(OUT_DIR)\obj
LIB_OUT  = $(OUT_DIR)\ce_image_ce_arm.lib

CC  = $(CE_CC)
LIB = $(CE_LIB)

# Compile as C++ (/TP) so imaging.h's COM interface declarations work.
# /EHsc enables standard C++ exception unwinding; cl.exe wants it whenever
# C++ code calls into Win32 functions that may emit SEH frames.
CI_CFLAGS = $(CE_CFLAGS) /TP /EHsc \
	/I"..\deps\ce_image" \
	/wd4100 /wd4127 /wd4189

ALL_OBJS = $(OBJ_DIR)\ce_image.obj

all: makedirs $(LIB_OUT)
	@echo.
	@echo ce_image ARM CE library: $(LIB_OUT)

makedirs:
	@if not exist "$(OUT_DIR)" mkdir "$(OUT_DIR)"
	@if not exist "$(OBJ_DIR)" mkdir "$(OBJ_DIR)"

$(LIB_OUT): $(ALL_OBJS)
	$(LIB) /nologo /OUT:$(LIB_OUT) $(ALL_OBJS)

clean:
	@if exist "$(OUT_DIR)" rmdir /s /q "$(OUT_DIR)"

$(OBJ_DIR)\ce_image.obj: $(CI_ROOT)\ce_image.cpp $(CI_ROOT)\ce_image.h
	$(CC) $(CI_CFLAGS) /Fo"$(OBJ_DIR)\ce_image.obj" /c $(CI_ROOT)\ce_image.cpp
