set pagination off
set breakpoint pending on
break so_relocate
run
set $tb = *(unsigned long *)&text_base
printf "TB=0x%lx\n", $tb
delete 1
break *($tb + 0x122120c)
commands
silent
printf "FW CreateWindowExA\n"
continue
end
break *($tb + 0x12212a8)
commands
silent
printf "FW RegisterClassA\n"
continue
end
break *($tb + 0x12215e8)
commands
silent
printf "FW ChoosePixelFormat\n"
continue
end
break *($tb + 0x1221204)
commands
silent
printf "FW ShowWindow\n"
continue
end
break *($tb + 0x1220bb0)
commands
silent
printf "FW CreateThread\n"
continue
end
break *($tb + 0x1229324)
commands
silent
printf "FW DirectSoundCreate\n"
continue
end
break *($tb + 0x1221d94)
commands
silent
printf "FW DirectInputCreateA\n"
continue
end
break *($tb + 0x121fdb4)
commands
silent
printf "FW CreateFileA\n"
continue
end
continue
quit
