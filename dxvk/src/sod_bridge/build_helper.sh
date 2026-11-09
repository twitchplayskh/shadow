#!/bin/sh
# 64-bit SoD-RT helper (not part of the meson build): mingw-w64 x86_64 + glslang
cd "$(dirname "$0")"
glslangValidator -V --target-env vulkan1.2 -o selftest.spv selftest.comp
python3 -c "
import struct;d=open('selftest.spv','rb').read();w=struct.unpack('<%dI'%(len(d)//4),d)
open('selftest_spv.h','w').write('#pragma once\n#include <stdint.h>\nstatic const uint32_t kSelfTestSpv[] = {\n'+',\n'.join(', '.join('0x%08x'%x for x in w[i:i+8]) for i in range(0,len(w),8))+'\n};\n')"
glslangValidator -V --target-env vulkan1.2 -o rtmask.spv rtmask.comp
python3 -c "
import struct;d=open('rtmask.spv','rb').read();w=struct.unpack('<%dI'%(len(d)//4),d)
open('rtmask_spv.h','w').write('#pragma once\n#include <stdint.h>\nstatic const uint32_t kRtMaskSpv[] = {\n'+',\n'.join(', '.join('0x%08x'%x for x in w[i:i+8]) for i in range(0,len(w),8))+'\n};\n')"
x86_64-w64-mingw32-g++-posix -O2 -std=c++17 -mwindows -static -static-libgcc -static-libstdc++ \
  -I../../include/vulkan/include -o sod_rt64.exe sod_rt64.cpp
