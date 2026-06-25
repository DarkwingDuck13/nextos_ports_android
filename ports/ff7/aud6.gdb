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
  printf "=== RenderMix #%d this=%p ===\n", $rm, $x19
  printf "floats at +160..+188:\n"
  x/8fw $x19+160
  printf "parent ptr field +88 (w):\n"
  x/2xw $x19+88
  if $rm >= 3
    disable
  end
  continue
end
continue
