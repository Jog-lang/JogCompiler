# Build and run programs in this folder with the compiler already built.
ifeq ($(OS),Windows_NT)
JOG := ../build/jog.exe
else
JOG := $(firstword $(wildcard ../build/jog ../build-linux-gcc/jog ../build-linux-clang/jog ../build-linux/jog))
ifeq ($(JOG),)
JOG := ../build/jog
endif
endif

.PHONY: all run

all:
	$(JOG) build

run:
	$(JOG) run
