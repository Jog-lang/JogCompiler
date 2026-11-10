# CLI driver for the jogc library. Build from this folder:
#   JOG=… jog build -o ../out/jogc --module jogc=../.
# or: jog-build build -o ../out/jogc
module jogc_cmd
version 0.1.0
kind executable
jog 0.0.1

require jogc ..
