#!/usr/bin/env python3
"""Disassemble libducktales.so at a file offset (==VMA). ARM or Thumb.
Usage: dis.py <hexoff> <count> [thumb]
"""
import sys, capstone
SO = "payload/lib/armeabi-v7a/libducktales.so"
off = int(sys.argv[1], 16)
n = int(sys.argv[2]) if len(sys.argv) > 2 else 40
thumb = len(sys.argv) > 3 and sys.argv[3].startswith("t")
data = open(SO, "rb").read()
mode = capstone.CS_MODE_THUMB if thumb else capstone.CS_MODE_ARM
md = capstone.Cs(capstone.CS_ARCH_ARM, mode)
md.detail = False
step = 2 if thumb else 4
code = data[off:off + n*step]
for ins in md.disasm(code, off):
    print(f"  {ins.address:08x}: {ins.bytes.hex():<8} {ins.mnemonic}\t{ins.op_str}")
