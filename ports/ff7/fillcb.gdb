set pagination off
set breakpoint pending on
break so_relocate
run
set $tb = *(unsigned long *)&text_base
printf "TEXTBASE=0x%lx\n", $tb
tbreak *($tb + 0x1245374)
continue
set $fc = *(unsigned long *)($tb + 0x1d1a000 + 2800)
printf "FILLCB=0x%lx (libjni+0x%lx)\n", $fc, $fc - $tb
kill
quit
