set pagination off
set breakpoint pending on
break so_relocate
run
set $tb = *(unsigned long *)&text_base
printf "=== TEXTBASE=0x%lx ===\n", $tb
set $rm=0
# SetMasterVolume(float vol, unsigned flag)
break *($tb + 0x1243938)
commands
  printf ">>> SetMasterVolume vol=%f flag=%d\n", $s0, $w1
  continue
end
# CoreSource::SetVolume(float)
break *($tb + 0x1244370)
commands
  printf ">>> CoreSource::SetVolume vol=%f\n", $s0
  continue
end
# CoreSource::RenderMix(out,size) -> dump output peak after finish
break *($tb + 0x1244784)
commands
  set $rm=$rm+1
  if $rm <= 4
    set $out=$x1
    set $sz=$w2
    finish
    set $pk=0
    set $k=0
    while $k < $sz/2 && $k < 256
      set $v=*(short*)($out+$k*2)
      if $v < 0
        set $v=-$v
      end
      if $v > $pk
        set $pk=$v
      end
      set $k=$k+1
    end
    printf ">>> RenderMix #%d out=%p size=%d OUTPUT_PEAK=%d\n", $rm, $out, $sz, $pk
  else
    disable
    continue
  end
end
continue
