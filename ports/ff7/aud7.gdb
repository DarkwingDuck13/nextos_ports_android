set pagination off
set breakpoint pending on
break so_relocate
run
set $tb = *(unsigned long *)&text_base
printf "=== TEXTBASE=0x%lx ===\n", $tb
set $qb=0
# CoreSource::QueueBuffer(this, buf, size)
break *($tb + 0x1244428)
commands
  set $qb=$qb+1
  printf "=== QueueBuffer #%d buf=%p size=%d ===\n", $qb, $x1, $w2
  printf "  head16: %02x %02x %02x %02x  peek_s16: %d %d %d %d\n", *(unsigned char*)($x1), *(unsigned char*)($x1+1), *(unsigned char*)($x1+2), *(unsigned char*)($x1+3), *(short*)($x1), *(short*)($x1+2), *(short*)($x1+4), *(short*)($x1+6)
  if $qb >= 6
    disable
  end
  continue
end
# vorbis_synthesis_pcmout(v, pcm) -> finish, w0=count
set $pc=0
break *($tb + 0x1248068)
commands
  set $pc=$pc+1
  finish
  printf "=== pcmout #%d returned_samples=%d ===\n", $pc, $x0
  if $pc >= 5
    disable
  end
  continue
end
continue
