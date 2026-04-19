include common.mk

export SYSROOT_INC := $(SYSROOT)/include/$(TARGET_TRIPLE)
export TARGET_TRIPLE

RT_SOURCE := $(LLVM_ROOT)/compiler-rt
RT_INSTALL_DIR := $(SDK_ROOT)/lib/clang/$(CLANG_VERSION)

# Stamps to mark completion of some tasks 
HEADER_STAMP := $(SYSROOT_INC)/.headers_complete

CMAKE_FLAGS := \
	-G Ninja \
	-S $(RT_SOURCE) \
	-B $(RT_BUILD_DIR) \
	-DCMAKE_C_COMPILER=$(CLANG) \
	-DCMAKE_CXX_COMPILER=$(CLANGXX) \
	-DCMAKE_C_COMPILER_TARGET=wasm32-wasi \
	-DCMAKE_CXX_COMPILER_TARGET=wasm32-wasi \
	-DCMAKE_SYSTEM_NAME=WASI \
	-DCMAKE_SYSTEM_PROCESSOR=wasm32 \
	-DCMAKE_SYSTEM_VERSION=1 \
	-DCMAKE_INSTALL_PREFIX="" \
	-DCMAKE_SYSROOT=$(SYSROOT) \
	-DLLVM_CONFIG_PATH=$(LLVM_CONFIG) \
	-DCOMPILER_RT_BAREMETAL_BUILD=ON \
	-DCMAKE_BUILD_TYPE=Release \
	-DCOMPILER_RT_DEFAULT_TARGET_ONLY=ON \
	-DCOMPILER_RT_BUILD_CRT=OFF \
	-DCOMPILER_RT_BUILD_BUILTINS=ON \
	-DCOMPILER_RT_INCLUDE_TESTS=OFF \
	-DCOMPILER_RT_BUILD_SANITIZERS=OFF \
	-DCOMPILER_RT_BUILD_XRAY=OFF \
	-DCOMPILER_RT_BUILD_LIBFUZZER=OFF \
	-DCOMPILER_RT_BUILD_PROFILE=OFF \
	-DCOMPILER_RT_HAS_FPIC_FLAG=OFF \
	-DCMAKE_TRY_COMPILE_TARGET_TYPE=STATIC_LIBRARY

.PHONY: all clean reconfigure

all: install

# 1. Install Headers needed for compiler-rt 
$(HEADER_STAMP):
	@echo "Installing WASI headers to $(SYSROOT_INC)..."
	@mkdir -p $(SYSROOT_INC)
	@if bash "../support/wasi-libc/scripts/install-include-headers.sh"; then \
		touch $(HEADER_STAMP); \
		echo "Successfully installed WASI headers."; \
	else \
		echo "Error: Header installation failed!"; exit 1; \
	fi

# 2. Configure CMake
build: $(HEADER_STAMP)
	@echo "Building compiler-rt..."
	@mkdir -p $(RT_BUILD_DIR)
	cmake $(CMAKE_FLAGS)
	ninja -C $(RT_BUILD_DIR) -j4
	
# 3. Build & Copy
install: build	
	@echo "Installing builtins to $(RT_INSTALL_DIR)/lib/wasi"
	DESTDIR=$(RT_INSTALL_DIR) ninja -C $(RT_BUILD_DIR) install
	@echo "Built & Copied WASLR builtins."

clean:
	rm -rf $(RT_BUILD_DIR)