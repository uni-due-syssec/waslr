FILE_DIR := $(patsubst %/,%,$(dir $(abspath $(lastword $(MAKEFILE_LIST)))))

PROJECT_ROOT := $(abspath $(FILE_DIR)/..)

LLVM_ROOT := $(PROJECT_ROOT)/waslr-llvm
SUPPORT_ROOT := $(PROJECT_ROOT)/support
SDK_ROOT := $(PROJECT_ROOT)/waslr-sdk

SYSROOT := $(SDK_ROOT)/share/wasi-sysroot

# Build dirs
LLVM_BUILD_DIR := $(LLVM_ROOT)/build
RT_BUILD_DIR := $(LLVM_ROOT)/build-compiler-rt
LIBC_BUILD_DIR := $(SUPPORT_ROOT)/wasi-libc/build
LIBCXX_BUILD_DIR := $(LLVM_ROOT)/build-libcxx

# Toolchain
CLANG := $(SDK_ROOT)/bin/clang
CLANGXX := $(SDK_ROOT)/bin/clang++
LLVM_CONFIG := $(SDK_ROOT)/bin/llvm-config

CLANG_VERSION := 20
TARGET_TRIPLE := wasm32-wasi