# JogCompiler (v2)

Zig-hosted Jog compiler that emits **native linux x64 ELF** with no C intermediate.

v1 lives on branches `develop` (jogc) and `seed` (C `jog`). This branch (`v2`) is a greenfield rewrite. Design: JogDocs `design/v2/`.

## Build

```
zig build
./zig-out/bin/jog version
```

## Commands

```
jog version
jog parse <file.jog>
jog check <file.jog>
jog build <file.jog> -o <out>
```

## Examples

```
zig build
./zig-out/bin/jog build examples/hello.jog -o out/hello
./out/hello
./zig-out/bin/jog check examples/subset.jog
```
