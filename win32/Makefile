
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

EXAMPLES_SRC_DIR=$(SRC_DIR)\examples
EXAMPLES_BUILD_DIR=$(BUILD_DIR)\examples

!if "$(VCPKG_DIR)" == ""
VCPKG_DIR=win32\vcpkg
!endif
PATH=$(PATH);$(VCPKG_DIR)\installed\$(VCPKG_TRIPLET)\bin

CFLAGS=/nologo /utf-8 /W3 /EHsc /Zi /I$(SRC_DIR) /I$(BUILD_DIR) /I$(VCPKG_DIR)\installed\$(VCPKG_TRIPLET)\include
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

!if "$(CXX)" == ""
CXX=cl.exe
!endif

!if "$(LD)" == ""
LD=link.exe
!endif

!if "$(PHP)" == ""
PHP=php.exe
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

VCPKG_CAPSTONE=$(VCPKG_DIR)\installed\$(VCPKG_TRIPLET)\include\capstone

LDFLAGS=$(LDFLAGS) /libpath:$(VCPKG_DIR)\installed\$(VCPKG_TRIPLET)\lib

LIBS=psapi.lib capstone.lib

OBJS_COMMON=$(BUILD_DIR)\ir.obj $(BUILD_DIR)\ir_strtab.obj $(BUILD_DIR)\ir_cfg.obj \
	$(BUILD_DIR)\ir_sccp.obj $(BUILD_DIR)\ir_gcm.obj $(BUILD_DIR)\ir_ra.obj $(BUILD_DIR)\ir_emit.obj \
	$(BUILD_DIR)\ir_load.obj $(BUILD_DIR)\ir_save.obj $(BUILD_DIR)\ir_emit_c.obj $(BUILD_DIR)\ir_dump.obj \
	$(BUILD_DIR)\ir_disasm.obj $(BUILD_DIR)\ir_check.obj $(BUILD_DIR)\ir_cpuinfo.obj
OBJS_IR = $(BUILD_DIR)\ir_main.obj
OBJS_IR_TEST = $(BUILD_DIR)\ir_test.obj
EXAMPLE_EXES = $(EXAMPLES_BUILD_DIR)\0001-basic.exe $(EXAMPLES_BUILD_DIR)\0005-basic-runner-func.exe

all: $(BUILD_DIR) $(BUILD_DIR)\ir.exe $(BUILD_DIR)\ir_test.exe

#$(BUILD_DIR)\ir.exe: builddir capstone $(BUILD_DIR)\ir_emit_$(DASM_ARCH).h $(BUILD_DIR)\ir_fold_hash.h $(OBJS_IR) $(OBJS_COMMON)
$(BUILD_DIR)\ir.exe: $(OBJS_IR) $(OBJS_COMMON)
	"$(LD)" $(LDFLAGS) $(OBJS_COMMON) $(OBJS_IR) $(LIBS) /out:$@

#$(BUILD_DIR)\ir_test.exe: builddir capstone $(BUILD_DIR)\ir_emit_$(DASM_ARCH).h $(BUILD_DIR)\ir_fold_hash.h $(OBJS_IR_TEST) $(OBJS_COMMON)
$(BUILD_DIR)\ir_test.exe: $(OBJS_IR_TEST) $(OBJS_COMMON) $(LIBCAPSTONE)
	"$(LD)" $(LDFLAGS) $(OBJS_COMMON) $(OBJS_IR_TEST) $(LIBS) /out:$@

$(BUILD_DIR)\ir_fold_hash.h: $(BUILD_DIR)\gen_ir_fold_hash.exe $(SRC_DIR)\ir_fold.h $(SRC_DIR)\ir.h
	$(BUILD_DIR)\gen_ir_fold_hash.exe < $(SRC_DIR)\ir_fold.h > $(BUILD_DIR)\ir_fold_hash.h
$(BUILD_DIR)\gen_ir_fold_hash.exe: $(SRC_DIR)\gen_ir_fold_hash.c $(SRC_DIR)\ir_strtab.c
	"$(CC)" $(CFLAGS) /Fo$(BUILD_DIR)\ /Fe$@ $**

$(BUILD_DIR)\minilua.exe: $(SRC_DIR)\dynasm\minilua.c
	"$(CC)" /Fo$(BUILD_DIR)\ /Fe$@ $**
$(BUILD_DIR)\ir_emit_$(DASM_ARCH).h: $(SRC_DIR)/ir_$(DASM_ARCH).dasc $(BUILD_DIR)\minilua.exe
	$(BUILD_DIR)\minilua.exe $(SRC_DIR)\dynasm\dynasm.lua $(DASM_FLAGS) -o $@ $(SRC_DIR)/ir_$(DASM_ARCH).dasc

$(OBJS_COMMON): $(SRC_DIR)\ir.h $(SRC_DIR)\ir_private.h

$(BUILD_DIR)\ir_main.obj: $(SRC_DIR)\ir.h
$(BUILD_DIR)\ir_test.obj: $(SRC_DIR)\ir.h $(SRC_DIR)\ir_builder.h
$(BUILD_DIR)\ir.obj: $(SRC_DIR)\ir_fold.h $(BUILD_DIR)\ir_fold_hash.h
$(BUILD_DIR)\ir_ra.obj: $(SRC_DIR)\ir_$(DASM_ARCH).h
$(BUILD_DIR)\ir_emit.obj: $(SRC_DIR)\ir_$(DASM_ARCH).h $(BUILD_DIR)\ir_emit_$(DASM_ARCH).h
$(BUILD_DIR)\ir_disasm.obj: $(VCPKG_CAPSTONE)

{$(SRC_DIR)}.c{$(BUILD_DIR)}.obj:
	"$(CC)" $(CFLAGS) /Fo$@ /c $<

{$(EXAMPLES_SRC_DIR)}.c{$(EXAMPLES_BUILD_DIR)}.exe:
	"$(CC)" $(CFLAGS) /Fo$*.obj /c $<
	"$(LD)" $(LDFLAGS) $(OBJS_COMMON) $*.obj $(LIBS) /out:$@

# If the vcpkg dir exists, lets assume we're good with the deps.
$(VCPKG_DIR)\vcpkg.exe:
	git clone https://github.com/Microsoft/vcpkg.git "$(VCPKG_DIR)"
	"$(VCPKG_DIR)\bootstrap-vcpkg.bat"

$(VCPKG_CAPSTONE): $(VCPKG_DIR)\vcpkg.exe
	"$(VCPKG_DIR)\vcpkg.exe" install --triplet=$(VCPKG_TRIPLET) capstone[x86]

$(BUILD_DIR):
	md "$(BUILD_DIR)"

$(EXAMPLES_BUILD_DIR):
	md "$(EXAMPLES_BUILD_DIR)"

$(BUILD_DIR)\ir-test.exe: $(SRC_DIR)/ir-test.cxx
	"$(CXX)" /std:c++17 $(SRC_DIR)\ir-test.cxx /Fe:$@

test: $(BUILD_DIR)\ir.exe $(BUILD_DIR)\ir-test.exe
	set PATH=$(VCPKG_DIR)\installed\$(VCPKG_TRIPLET)\bin:%%PATH%%
	$(BUILD_DIR)\ir.exe $(SRC_DIR)\test.ir --dump --save $(BUILD_DIR)\test.log
	$(BUILD_DIR)\ir-test.exe $(SRC_DIR)\tests

test-ci: $(BUILD_DIR)\ir.exe $(BUILD_DIR)\ir-test.exe
	set PATH=$(VCPKG_DIR)\installed\$(VCPKG_TRIPLET)\bin:%%PATH%%
	$(BUILD_DIR)\ir-test.exe --show-diff $(SRC_DIR)\tests

examples: $(OBJS_COMMON) $(EXAMPLES_BUILD_DIR) $(EXAMPLE_EXES)

clean:
	del /f /q $(BUILD_DIR)\*.obj $(BUILD_DIR)\*.exe $(BUILD_DIR)\*.pdb $(BUILD_DIR)\*.ilk $(BUILD_DIR)\*.h

