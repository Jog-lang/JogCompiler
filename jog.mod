# jogc frontend library: lex, parse, load, check (and lower/ir/emit for the CLI).
# Stage 0 is an external C jog. The CLI driver lives in cmd/.
module jogc
version 0.1.0
kind library
jog 0.0.1
targets windows linux macos

export src.lex
export src.parse
export src.load
export src.check
export src.frontend
export src.lower
export src.ir
export src.emit
