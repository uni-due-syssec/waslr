#!/bin/bash

WASLR_ENABLED=1

TARGET_DIR=../waslr-llvm/build/lib/clang/20/lib/wasi/

mkdir -p "$TARGET_DIR"

if [ "$WASLR_ENABLED" -eq 1 ]; then
  cp libclang_rt.builtins-wasm32-waslr.a "$TARGET_DIR"/libclang_rt.builtins-wasm32.a
  echo "Copied WASLR builtins!"
else
  cp libclang_rt.builtins-wasm32.a "$TARGET_DIR"/libclang_rt.builtins-wasm32.a
  echo "Copied default builtins!"
fi
