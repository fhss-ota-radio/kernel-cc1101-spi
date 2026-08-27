savedcmd_cc1101.mod := printf '%s\n'   cc1101_core.o cc1101_main.o | awk '!x[$$0]++ { print("./"$$0) }' > cc1101.mod
