set pagination off
set breakpoint pending on
break so_relocate
run
set $tb = *(unsigned long *)&text_base
printf "TB=0x%lx\n", $tb
delete 1
# parar na ULTIMA chamada indireta antes do bail
break *($tb + 0x15587c) if (unsigned)$x0 == 0x677b11
continue
printf "=== hit _call_c_func(0x677b11); finishing ===\n"
finish
printf "=== back in caller; recording final stretch ===\n"
record full
tbreak *($tb + 0x13fee4)
continue
printf "=== BOOT RETURN; reversing ===\n"
set $i = 0
while $i < 120
  printf "off=0x%lx\n", ((unsigned long)$pc - $tb)
  reverse-stepi
  set $i = $i + 1
end
quit
