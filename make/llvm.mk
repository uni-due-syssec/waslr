include common.mk

LLVM_SOURCE := $(LLVM_ROOT)/llvm

TOOLS := clang \
	lld \
	llvm-ar \
	llvm-nm \
	clang-resource-headers \
	llvm-ranlib \
	clang-format \
	clang-scan-deps \
	llvm-addr2line \
	llvm-mc \
	llvm-dwarfdump \
	llvm-dwp \
	llvm-strip \
	llvm-size \
	llvm-strings \
	llvm-objdump \
	llvm-objcopy \
	llvm-cxxfilt \
	llvm-config \
	libclang
 
CMAKE_FLAGS := \
	-G Ninja \
	-S $(LLVM_SOURCE) \
	-B $(LLVM_BUILD_DIR) \
	-DCMAKE_INSTALL_PREFIX="" \
	-DLLVM_DEFAULT_TARGET_TRIPLE="wasm32-wasi" \
	-DLLVM_TARGETS_TO_BUILD="WebAssembly" \
	-DLLVM_ENABLE_PROJECTS="clang;lld" \
	-DCMAKE_BUILD_TYPE=Debug \
	-DLLVM_INCLUDE_BENCHMARKS=OFF \
	-DLLVM_INCLUDE_TESTS=OFF \
	-DLLVM_BUILD_DOCS=OFF \
	-DLLVM_ENABLE_RTTI=ON \
	-DLLVM_ENABLE_EH=ON \
	-DLLVM_PARALLEL_LINK_JOBS=2

INSTALL_TARGETS := $(addprefix install-,$(TOOLS))

.PHONY: all build install reconfigure

all: install 

# 1. Configure CMake
$(LLVM_BUILD_DIR)/build.ninja:
	@echo "Configuring llvm..."
	@mkdir -p $(LLVM_BUILD_DIR)
	cmake $(CMAKE_FLAGS) > /dev/null;

# 2. Build 
build: $(LLVM_BUILD_DIR)/build.ninja
	@echo "Building llvm..."
	ninja -C $(LLVM_BUILD_DIR)

# 3. Install 
install: build
	@mkdir -p $(SDK_ROOT)
	DESTDIR=$(SDK_ROOT) ninja -C $(LLVM_BUILD_DIR) $(INSTALL_TARGETS) > /dev/null;

reconfigure:
	@echo "Reconfiguring llvm..."
	@mkdir -p $(LLVM_BUILD_DIR)
	cmake $(CMAKE_FLAGS)

clean:
	rm -rf $(LLVM_BUILD_DIR)