set pagination off
set breakpoint pending on
break so_relocate
run
set $tb = *(unsigned long *)&text_base
printf "=== TEXTBASE=0x%lx ===\n", $tb
break *($tb + 0x12215f0)
delete 1
continue
printf "=== HIT fw_SwapBuffers (1st) ===\n"
bt 30
continue
printf "=== fw_SwapBuffers (2nd)? ===\n"
bt 12
kill
quit
