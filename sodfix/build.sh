#!/bin/sh
# Cross-build (Linux/macOS/Windows) with zig: pip install ziglang
python3 -m ziglang c++ -target x86-windows-gnu -O2 -shared -fno-exceptions -fno-rtti \
  -o build/dinput8.dll src/sodfix.cpp src/dinput8.def -luser32 -lgdi32 -lkernel32
