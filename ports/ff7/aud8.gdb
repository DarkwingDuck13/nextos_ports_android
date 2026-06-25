set pagination off
set breakpoint pending on
break so_relocate
run
set $tb = *(unsigned long *)&text_base
printf "=== TEXTBASE=0x%lx ===\n", $tb
set $qb=0
break *($tb + 0x1244428)
commands
  set $qb=$qb+1
  printf "=== QB #%d size=%d head: %02x %02x %02x %02x s16: %d %d %d %d ===\n", $qb, $w2, *(unsigned char*)($x1), *(unsigned char*)($x1+1), *(unsigned char*)($x1+2), *(unsigned char*)($x1+3), *(short*)($x1), *(short*)($x1+2), *(short*)($x1+4), *(short*)($x1+6)
  if $qb >= 10
    disable
  end
  continue
end
continue
