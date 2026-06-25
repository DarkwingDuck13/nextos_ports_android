set pagination off
set breakpoint pending on
break so_relocate
run
set $tb = *(unsigned long *)&text_base
printf "TEXTBASE=0x%lx\n", $tb
delete 1
break *($tb + 0x15587c)
commands
silent
printf "CCF=0x%x\n", (unsigned)$x0
continue
end
break *($tb + 0x131410)
commands
silent
printf "CF=0x%x\n", (unsigned)$x0
continue
end
continue
quit
