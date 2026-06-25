set pagination off
set breakpoint pending on
break so_relocate
run
set $tb = *(unsigned long *)&text_base
printf "TEXTBASE=0x%lx\n", $tb
delete 1
tbreak *($tb + 0x195010)
continue
printf "=== boot entry, recording ===\n"
record full
tbreak *($tb + 0x13fee4)
continue
printf "=== BOOT RETURN reached; reversing 70 insns ===\n"
set $i = 0
while $i < 70
  printf "off=0x%lx\n", ((unsigned long)$pc - $tb)
  reverse-stepi
  set $i = $i + 1
end
quit
