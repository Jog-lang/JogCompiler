# An application that requires a local library. Every v2 directive the
# compiler reads appears here or in ../lib/jog.mod.
module app
version 0.1.0
kind executable
jog 0.0.1

require lib ../lib
