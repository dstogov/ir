
!if "$(BUILD)" != "debug"
BUILD=release
!endif

!if "$(BUILD_DIR)" == ""
BUILD_DIR=win32\build_$(VSCMD_ARG_TGT_ARCH)_$(BUILD)
!endif
SRC_DIR=.

EXAMPLES_SRC_DIR=$(SRC_DIR)\examples
EXAMPLES_BUILD_DIR=$(BUILD_DIR)\examples

CAPSTONE_TAG=5.0-rc2
!if "$(CAPSTONE_DIR)" == ""
CAPSTONE_DIR=win32\capstone
!endif
CAPSTONE_VCXPRJ=capstone_static\capstone_static.vcxproj
CAPSTONE_VCXPRJ_PATH=$(CAPSTONE_DIR)\msvc\$(CAPSTONE_VCXPRJ)
!if "$(VSCMD_ARG_TGT_ARCH)" == "x64"
CAPSTONE_ARTIFACTS_DIR=$(CAPSTONE_DIR)\msvc\$(VSCMD_ARG_TGT_ARCH)\$(BUILD)
!else
CAPSTONE_ARTIFACTS_DIR=$(CAPSTONE_DIR)\msvc\$(BUILD)
!endif
PATH=$(PATH);$(CAPSTONE_ARTIFACTS_DIR)

CFLAGS=/nologo /utf-8 /W3 /EHsc /Zi /I$(BUILD_DIR) /I$(CAPSTONE_DIR)\include
LDFLAGS=/nologo
!if "$(BUILD)" == "release"
CFLAGS=$(CFLAGS) /MT /Ox
LDFLAGS=$(LDFLAGS) /LTCG
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
TEST_TARGET=Windows-x86_64
!endif
!if "$(TARGET)" == "x86"
CFLAGS=$(CFLAGS) /DIR_TARGET_X86
DASM_ARCH=x86
DASM_FLAGS=
TEST_TARGET=x86
!endif

!if "$(VSCMD_ARG_TGT_ARCH)" == "x64"
LDFLAGS=$(LDFLAGS) /libpath:$(CAPSTONE_DIR)\msvc\$(VSCMD_ARG_TGT_ARCH)\$(BUILD)
!else
LDFLAGS=$(LDFLAGS) /libpath:$(CAPSTONE_DIR)\msvc\$(BUILD)
!endif

LIBS=psapi.lib capstone.lib

OBJS_COMMON=$(BUILD_DIR)\ir.obj $(BUILD_DIR)\ir_strtab.obj $(BUILD_DIR)\ir_cfg.obj \
	$(BUILD_DIR)\ir_sccp.obj $(BUILD_DIR)\ir_gcm.obj $(BUILD_DIR)\ir_ra.obj $(BUILD_DIR)\ir_emit.obj \
	$(BUILD_DIR)\ir_load.obj $(BUILD_DIR)\ir_save.obj $(BUILD_DIR)\ir_emit_c.obj $(BUILD_DIR)\ir_dump.obj \
	$(BUILD_DIR)\ir_disasm.obj $(BUILD_DIR)\ir_check.obj $(BUILD_DIR)\ir_cpuinfo.obj \
	$(BUILD_DIR)\ir_emit_llvm.obj
OBJS_IR = $(BUILD_DIR)\ir_main.obj
OBJS_IR_TEST = $(BUILD_DIR)\ir_test.obj
EXAMPLE_EXES = $(EXAMPLES_BUILD_DIR)\0001-basic.exe $(EXAMPLES_BUILD_DIR)\0001-while.exe $(EXAMPLES_BUILD_DIR)\0005-basic-runner-func.exe \
	       $(EXAMPLES_BUILD_DIR)\0001-pointer.exe $(EXAMPLES_BUILD_DIR)\0001-func.exe

all: $(BUILD_DIR) $(BUILD_DIR)\ir.exe $(BUILD_DIR)\ir_test.exe

$(BUILD_DIR)\ir.exe: $(OBJS_IR) $(OBJS_COMMON)
	"$(LD)" $(LDFLAGS) $(OBJS_COMMON) $(OBJS_IR) $(LIBS) /out:$@

#$(BUILD_DIR)\ir_test.exe: builddir capstone $(BUILD_DIR)\ir_emit_$(DASM_ARCH).h $(BUILD_DIR)\ir_fold_hash.h $(OBJS_IR_TEST) $(OBJS_COMMON)
$(BUILD_DIR)\ir_test.exe: $(OBJS_IR_TEST) $(OBJS_COMMON) $(LIBCAPSTONE)
	"$(LD)" $(LDFLAGS) $(OBJS_COMMON) $(OBJS_IR_TEST) $(LIBS) /out:$@

$(BUILD_DIR)\ir_fold_hash.h: $(BUILD_DIR)\gen_ir_fold_hash.exe $(SRC_DIR)\ir_fold.h $(SRC_DIR)\ir.h
	$(BUILD_DIR)\gen_ir_fold_hash.exe < $(SRC_DIR)\ir_fold.h > $(BUILD_DIR)\ir_fold_hash.h
$(BUILD_DIR)\gen_ir_fold_hash.exe: $(SRC_DIR)\gen_ir_fold_hash.c $(SRC_DIR)\ir_strtab.c $(SRC_DIR)\ir.h
	"$(CC)" $(CFLAGS) /Fo$(BUILD_DIR)\ /Fe$@ $(SRC_DIR)\gen_ir_fold_hash.c

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
$(BUILD_DIR)\ir_disasm.obj: capstone

{$(SRC_DIR)}.c{$(BUILD_DIR)}.obj:
	"$(CC)" $(CFLAGS) /Fo$@ /c $<

{$(EXAMPLES_SRC_DIR)}.c{$(EXAMPLES_BUILD_DIR)}.exe:
	"$(CC)" $(CFLAGS) /Fo$*.obj /c $<
	"$(LD)" $(LDFLAGS) $(OBJS_COMMON) $*.obj $(LIBS) /out:$@

capstone:
!if !exist($(CAPSTONE_DIR))
	git clone --branch $(CAPSTONE_TAG) https://github.com/capstone-engine/capstone "$(CAPSTONE_DIR)"
	devenv "$(CAPSTONE_DIR)\msvc\capstone.sln" /upgrade
# Remove unused features to simplify the resulting lib
	powershell -Command "foreach ($$D in 'CAPSTONE_HAS_ARM64', 'CAPSTONE_HAS_ARM', 'CAPSTONE_HAS_BPF', \
		'CAPSTONE_HAS_EVM', 'CAPSTONE_HAS_M680X', 'CAPSTONE_HAS_M68K', 'CAPSTONE_HAS_MIPS', \
		'CAPSTONE_HAS_MOS65XX', 'CAPSTONE_HAS_POWERPC', 'CAPSTONE_HAS_RISCV', 'CAPSTONE_HAS_SPARC', \
		'CAPSTONE_HAS_SYSZ', 'CAPSTONE_HAS_TMS320C64X', 'CAPSTONE_HAS_WASM', 'CAPSTONE_HAS_XCORE') { \
			(Get-Content $(CAPSTONE_VCXPRJ_PATH)) -replace ($$D + ';'), '' | \
			Out-File -encoding ASCII $(CAPSTONE_VCXPRJ_PATH) \
		}"
!endif
!if !exist($(CAPSTONE_ARTIFACTS_DIR))
!if "$(VSCMD_ARG_TGT_ARCH)" == "x64"
	devenv "$(CAPSTONE_DIR)\msvc\capstone.sln" /build "$(BUILD)|x64" /project $(CAPSTONE_VCXPRJ)
!else
	devenv "$(CAPSTONE_DIR)\msvc\capstone.sln" /build "$(BUILD)|win32" /project $(CAPSTONE_VCXPRJ)
!endif
!endif

capstone_tag:
	@echo $(CAPSTONE_TAG)

capstone_dir:
	@echo $(CAPSTONE_DIR)

$(BUILD_DIR):
	md "$(BUILD_DIR)"

$(EXAMPLES_BUILD_DIR):
	md "$(EXAMPLES_BUILD_DIR)"

$(BUILD_DIR)\tester.exe: $(SRC_DIR)/tools/tester.c
	"$(CC)" $(CFLAGS) $(SRC_DIR)\tools\tester.c /Fe:$@

test: $(BUILD_DIR)\ir.exe $(BUILD_DIR)\tester.exe
	$(BUILD_DIR)\ir.exe $(SRC_DIR)\test.ir --dump --save $(BUILD_DIR)\test.log
	$(BUILD_DIR)\tester.exe --test-cmd $(BUILD_DIR)\ir.exe --target $(TEST_TARGET) --default-args "--save" \
	    --additional-args "--no-abort-fault" \
		--test-extension ".irt" --code-extension ".ir" $(SRC_DIR)\tests

test-ci: $(BUILD_DIR)\ir.exe $(BUILD_DIR)\tester.exe
	$(BUILD_DIR)\tester.exe --test-cmd $(BUILD_DIR)\ir.exe --target $(TEST_TARGET) --default-args "--save" \
	    --additional-args "--no-abort-fault" --diff-cmd "diff --strip-trailing-cr -u" \
		--test-extension ".irt" --code-extension ".ir" --show-diff $(SRC_DIR)\tests

examples: $(OBJS_COMMON) $(EXAMPLES_BUILD_DIR) $(EXAMPLE_EXES)

clean:
	del /f /q $(BUILD_DIR)\*.obj $(BUILD_DIR)\*.exe $(BUILD_DIR)\*.pdb $(BUILD_DIR)\*.ilk $(BUILD_DIR)\*.h

