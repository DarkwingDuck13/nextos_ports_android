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
  printf ">>> RenderMix #%d this=%p\n", $rm, $x19
  printf "    f176=%f f180=%f f184=%f f88(w)=0x%x\n", *(float*)($x19+176), *(float*)($x19+180), *(float*)($x19+184), *(unsigned*)($x19+88)
  printf "    f168=%f f172=%f f160=%f f164=%f\n", *(float*)($x19+168), *(float*)($x19+172), *(float*)($x19+160), *(float*)($x19+164)
  if $rm >= 4
    disable
  end
  continue
end
continue
