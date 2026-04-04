#!/bin/bash

WASLR_ENABLED=1

TARGET_PATH=../waslr-llvm/build/lib/clang/20/lib/wasi/libclang_rt.builtins-wasm32.a

if [ "$WASLR_ENABLED" -eq 1 ]; then
  cp libclang_rt.builtins-wasm32-waslr.a "$TARGET_PATH"
  echo "Copied WASLR builtins!"
else
  cp libclang_rt.builtins-wasm32.a "$TARGET_PATH"
  echo "Copied default builtins!"
fi
