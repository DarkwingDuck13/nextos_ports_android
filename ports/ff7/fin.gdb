set confirm off
set pagination off
set breakpoint pending on
break so_relocate
run
set $tb = *(unsigned long *)&text_base
printf "TB=0x%lx\n", $tb
delete 1
tbreak *($tb + 0x195010)
continue
printf "=== at func_0x0040b6e0 entry; finishing whole boot ===\n"
finish
printf "=== boot returned x0=0x%lx pc_off=0x%lx ===\n", (unsigned long)$x0, ((unsigned long)$pc - $tb)
printf "FRAME1_off=0x%lx\n", ((unsigned long)$pc - $tb)
bt 12
quit
