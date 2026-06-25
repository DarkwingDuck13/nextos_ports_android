set pagination off
set breakpoint pending on
break so_relocate
run
set $tb = *(unsigned long *)&text_base
printf "=== TEXTBASE=0x%lx ===\n", $tb
set $rm=0
break *($tb + 0x1244784)
commands
  set $rm=$rm+1
  set $out=$x1
  set $sz=$w2
  finish
  set $pk=0
  set $k=0
  while $k < ($sz/2) && $k < 512
    set $v=*(short*)($out+$k*2)
    if $v < 0
      set $v=0-$v
    end
    if $v > $pk
      set $pk=$v
    end
    set $k=$k+1
  end
  printf ">>> RenderMix #%d out=%p size=%d OUTPUT_PEAK=%d first4=%04x %04x %04x %04x\n", $rm, $out, $sz, $pk, *(unsigned short*)($out), *(unsigned short*)($out+2), *(unsigned short*)($out+4), *(unsigned short*)($out+6)
  if $rm >= 5
    disable
  end
  continue
end
continue
