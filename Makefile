# Rebuild the jog compiler. scripts/check.ps1 remains the merge gate.
ifeq ($(OS),Windows_NT)
EXE := .exe
else
EXE :=
endif

.PHONY: all
all:
ifeq ($(wildcard build/CMakeCache.txt),)
	cmake -S . -B build -G Ninja
endif
	cmake --build build --target jog
	cmake -E echo $(abspath build/jog$(EXE))
