
!if "$(BUILD)" == ""
BUILD=release
!endif

!if "$(VCPKG_TRIPLET)" == ""
VCPKG_TRIPLET=$(VSCMD_ARG_TGT_ARCH)-windows
!endif

!if "$(BUILD_DIR)" == ""
BUILD_DIR=win32\build_$(VCPKG_TRIPLET)
!endif
SRC_DIR=.

!if "$(VCPKG_DIR)" == ""
VCPKG_DIR=win32\vcpkg
!endif
PATH=$(PATH);$(VCPKG_DIR)\installed\$(VCPKG_TRIPLET)\bin

CFLAGS=/nologo /utf-8 /W3 /EHsc /Zi /I$(BUILD_DIR) /I$(VCPKG_DIR)\installed\$(VCPKG_TRIPLET)\include
LDFLAGS=/nologo
!if "$(BUILD)" == "release"
CFLAGS=$(CFLAGS) /MT /Ox
!else
CFLAGS=$(CFLAGS) /MTd /Od /DEBUG /D_DEBUG /DIR_DEBUG=1
LDFLAGS=$(LDFLAGS) /DEBUG
!endif

!if "$(CC)" == ""
CC=cl.exe
!endif

!if "$(LD)" == ""
LD=link.exe
!endif

!if "$(TARGET)" == ""
TARGET = $(VSCMD_ARG_TGT_ARCH)
!endif
!if "$(TARGET)" == "x64"
CFLAGS=$(CFLAGS) /DIR_TARGET_X64
DASM_ARCH=x86
DASM_FLAGS=-D X64=1
!endif
!if "$(TARGET)" == "x86"
CFLAGS=$(CFLAGS) /DIR_TARGET_X86
DASM_ARCH=x86
DASM_FLAGS=
!endif

LDFLAGS=$(LDFLAGS) /libpath:$(VCPKG_DIR)\installed\$(VCPKG_TRIPLET)\lib

LIBS=psapi.lib capstone.lib

OBJS_COMMON=$(BUILD_DIR)\ir.obj $(BUILD_DIR)\ir_strtab.obj $(BUILD_DIR)\ir_cfg.obj \
	$(BUILD_DIR)\ir_sccp.obj $(BUILD_DIR)\ir_gcm.obj $(BUILD_DIR)\ir_ra.obj $(BUILD_DIR)\ir_emit.obj \
	$(BUILD_DIR)\ir_load.obj $(BUILD_DIR)\ir_save.obj $(BUILD_DIR)\ir_emit_c.obj $(BUILD_DIR)\ir_dump.obj \
	$(BUILD_DIR)\ir_disasm.obj $(BUILD_DIR)\ir_check.obj
OBJS_IR = $(BUILD_DIR)\ir_main.obj
OBJS_IR_TEST = $(BUILD_DIR)\ir_test.obj

all: $(BUILD_DIR)\ir.exe

$(BUILD_DIR)\ir.exe: builddir capstone $(BUILD_DIR)\ir_emit_$(DASM_ARCH).h $(BUILD_DIR)\ir_fold_hash.h $(OBJS_IR) $(OBJS_COMMON)
	"$(LD)" $(LDFLAGS) $(OBJS_COMMON) $(OBJS_IR) $(LIBS) /out:$@

$(BUILD_DIR)\ir_fold_hash.h: $(BUILD_DIR)\gen_ir_fold_hash.exe $(SRC_DIR)\ir_fold.h $(SRC_DIR)\ir.h
	$(BUILD_DIR)\gen_ir_fold_hash.exe < $(SRC_DIR)\ir_fold.h > $(BUILD_DIR)\ir_fold_hash.h
$(BUILD_DIR)\gen_ir_fold_hash.exe: $(SRC_DIR)\gen_ir_fold_hash.c $(SRC_DIR)\ir_strtab.c
	"$(CC)" $(CFLAGS) /Fo$(BUILD_DIR)\ /Fe$@ $**

$(BUILD_DIR)\minilua.exe: $(SRC_DIR)\dynasm\minilua.c
	"$(CC)" /Fo$(BUILD_DIR)\ /Fe$@ $**
$(BUILD_DIR)\ir_emit_$(DASM_ARCH).h: $(BUILD_DIR)\minilua.exe
	$(BUILD_DIR)\minilua.exe $(SRC_DIR)\dynasm\dynasm.lua $(DASM_FLAGS) -o $@ $(SRC_DIR)/ir_$(DASM_ARCH).dasc

{$(SRC_DIR)}.c{$(BUILD_DIR)}.obj:
	"$(CC)" $(CFLAGS) /Fo$@ /c $<

# If the vcpkg dir exists, lets assume we're good with the deps.
!if !exist($(VCPKG_DIR))
vcpkg:
	git clone https://github.com/Microsoft/vcpkg.git "$(VCPKG_DIR)"
	"$(VCPKG_DIR)\bootstrap-vcpkg.bat"
!else
vcpkg:
!endif

capstone: vcpkg
	"$(VCPKG_DIR)\vcpkg.exe" install --triplet=$(VCPKG_TRIPLET) capstone

!if !exist($(BUILD_DIR))
builddir:
	md "$(BUILD_DIR)"
!else
builddir:
!endif

test: $(BUILD_DIR)\ir.exe
	$(BUILD_DIR)\ir.exe $(SRC_DIR)\test.ir --dump --save $(BUILD_DIR)\test.log

clean:
	del /f /q $(BUILD_DIR)\*.obj $(BUILD_DIR)\*.exe $(BUILD_DIR)\*.pdb $(BUILD_DIR)\*.ilk $(BUILD_DIR)\*.h

