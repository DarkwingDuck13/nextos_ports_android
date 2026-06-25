set pagination off
set breakpoint pending on
break so_relocate
run
set $tb = *(unsigned long *)&text_base
printf "=== TEXTBASE=0x%lx ===\n", $tb
set $qb=0
set $rm=0
set $vs=0
# CoreSource::QueueBuffer(void* buf, size)
break *($tb + 0x1244428)
commands
  set $qb=$qb+1
  if $qb <= 6
    printf ">>> QueueBuffer #%d buf=%p size=%d\n", $qb, $x1, $w2
  end
  if $qb == 6
    disable
  end
  continue
end
# CoreSource::RenderMix(void* out, size)
break *($tb + 0x1244784)
commands
  set $rm=$rm+1
  if $rm <= 6
    printf ">>> RenderMix #%d out=%p size=%d\n", $rm, $x1, $w2
  end
  if $rm == 6
    disable
  end
  continue
end
# vorbis_synthesis_blockin
break *($tb + 0x1247304)
commands
  set $vs=$vs+1
  if $vs <= 4
    printf ">>> vorbis_synthesis_blockin #%d\n", $vs
  end
  if $vs == 4
    disable
  end
  continue
end
continue
printf "=== COUNTS QueueBuffer=%d RenderMix=%d vorbis_blockin=%d ===\n", $qb, $rm, $vs
