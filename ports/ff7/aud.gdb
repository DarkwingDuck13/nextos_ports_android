set pagination off
set breakpoint pending on
break so_relocate
run
set $tb = *(unsigned long *)&text_base
printf "=== TEXTBASE=0x%lx ===\n", $tb
# CreateSource(ICoreSource**, int rate, int ch, ICoreSourceCallback*)
break *($tb + 0x12437f0)
commands
  printf ">>> CreateSource ret=%p arg_rate=%d arg_ch=%d cb=%p\n", $x1, $w2, $w3, $x4
  continue
end
# akbMaterialDecode
break *($tb + 0x1239e74)
commands
  printf ">>> akbMaterialDecode x0=%p x1=%p x2=%p x3=%p\n", $x0, $x1, $x2, $x3
  continue
end
continue
