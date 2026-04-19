include make/common.mk

MK_BUILTINS:=$(MAKE) -C make -f builtins.mk
MK_LLVM:=$(MAKE) -C make -f llvm.mk
MK_STDLIB:=$(MAKE) -C make -f stdlib.mk
MK_WASLR_RT:=$(MAKE) -C make -f waslr_runtime.mk

.PHONY: toolchain sdk clean-sysroot-build clean-toolchain-build clean-build clean-sdk clean-all

all: sdk

toolchain:
	$(MK_LLVM) install

sdk: toolchain
	$(MK_BUILTINS) install
	$(MK_STDLIB) install
	$(MK_WASLR_RT) install

clean-sysroot-build:
	$(MK_BUILTINS) clean
	$(MK_STDLIB) clean
	$(MK_WASLR_RT) clean

clean-toolchain-build:
	$(MK_LLVM) clean

clean-build: clean-sysroot-build clean-toolchain-build 

clean-sdk:
	rm -rf $(SDK_ROOT)

clean-all: clean-build clean-sdk
