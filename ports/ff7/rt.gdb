set confirm off
set pagination off
set breakpoint pending on
break so_relocate
run
set $tb = *(unsigned long *)&text_base
printf "TB=0x%lx\n", $tb
delete 1
break *($tb + 0x14f3e0)
ignore 1 3
continue
set $rtp = *(unsigned long *)($tb + 0x133a000 + 2520)
printf "RESULT RT=0x%lx FBO=%u RBO=%u w=%u h=%u\n", $rtp, *(unsigned int*)($rtp+8), *(unsigned int*)($rtp+16), *(unsigned int*)($rtp+20), *(unsigned int*)($rtp+24)
quit
