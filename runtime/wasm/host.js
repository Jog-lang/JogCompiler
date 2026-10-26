"use strict";

/* Node host for a freestanding Jog wasm32 module. Undefined externs are
   env imports unless @link names another module. puts, jog_host_marker,
   and jog_host_ok are provided in whichever module imports them. The
   runtime's own imports are jogrt_host_write, jogrt_host_abort, and
   jogrt_host_exit. Arguments after the module path are the program's
   argv, after argv[0] which is the module path. */

const fs = require("fs");

const wasmPath = process.argv[2];
if (!wasmPath) {
  process.stderr.write("jog wasm host: missing module path\n");
  process.exit(2);
}

let memory = null;

function bytesAt(ptr, len) {
  if (!memory) {
    return null;
  }
  const mem = new Uint8Array(memory.buffer);
  const start = ptr >>> 0;
  const n = len >>> 0;
  if (n > mem.length || start > mem.length - n) {
    return null;
  }
  return Buffer.from(mem.subarray(start, start + n));
}

function writeFd(fd, ptr, len) {
  const which = fd | 0;
  const n = len >>> 0;
  if (which !== 1 && which !== 2) {
    return 0;
  }
  if (n === 0) {
    return 1;
  }
  const chunk = bytesAt(ptr, n);
  if (!chunk) {
    return 0;
  }
  fs.writeSync(which, chunk);
  return 1;
}

function readCString(ptr) {
  if (!memory) {
    return null;
  }
  const mem = new Uint8Array(memory.buffer);
  let end = ptr >>> 0;
  const start = end;
  while (end < mem.length && mem[end] !== 0) {
    end++;
  }
  if (end >= mem.length) {
    return null;
  }
  return Buffer.from(mem.subarray(start, end));
}

function puts(ptr) {
  try {
    const text = readCString(ptr);
    if (!text) {
      return -1;
    }
    fs.writeSync(1, text);
    fs.writeSync(1, Buffer.from("\n"));
    return 0;
  } catch (err) {
    return -1;
  }
}

function jogHostMarker() {
  fs.writeSync(1, Buffer.from("marker\n"));
  return 0;
}

function jogHostOk() {
  return 0;
}

const known = {
  puts: puts,
  jog_host_marker: jogHostMarker,
  jog_host_ok: jogHostOk,
  jogrt_host_write(fd, ptr, len) {
    try {
      return writeFd(fd, ptr, len);
    } catch (err) {
      return 0;
    }
  },
  jogrt_host_abort() {
    process.exit(134);
  },
  jogrt_host_exit(code) {
    process.exit(code | 0);
  },
};

function buildImports(mod) {
  const table = {};
  const absent = [];
  for (const item of WebAssembly.Module.imports(mod)) {
    const fn = item.kind === "function" ? known[item.name] : null;
    if (!fn) {
      absent.push(item.module + "." + item.name);
      continue;
    }
    if (!table[item.module]) {
      table[item.module] = {};
    }
    table[item.module][item.name] = fn;
  }
  return { table: table, absent: absent };
}

/* Write argv into freshly grown memory. argv[argc] is a null pointer.
   Returns the address of the pointer table. */
function installArgv(mem, args) {
  const page = 65536;
  const encoded = [];
  let bytes = 4 * (args.length + 1);
  let i;
  for (i = 0; i < args.length; i++) {
    const buf = Buffer.from(String(args[i]), "utf8");
    encoded.push(buf);
    if (bytes > 0x7fffffff - (buf.length + 1)) {
      process.stderr.write("jog wasm host: arguments do not fit in memory\n");
      process.exit(1);
    }
    bytes += buf.length + 1;
  }
  const pages = Math.max(1, Math.ceil(bytes / page));
  let base;
  try {
    base = mem.grow(pages);
  } catch (err) {
    process.stderr.write("jog wasm host: cannot grow memory for arguments\n");
    process.exit(1);
  }
  const start = base * page;
  const view = new DataView(mem.buffer);
  const bytesMem = new Uint8Array(mem.buffer);
  let strAt = start + 4 * (args.length + 1);
  for (i = 0; i < args.length; i++) {
    view.setUint32(start + i * 4, strAt, true);
    bytesMem.set(encoded[i], strAt);
    bytesMem[strAt + encoded[i].length] = 0;
    strAt += encoded[i].length + 1;
  }
  view.setUint32(start + args.length * 4, 0, true);
  return start;
}

async function main() {
  const bytes = fs.readFileSync(wasmPath);
  const mod = await WebAssembly.compile(bytes);
  const built = buildImports(mod);
  if (built.absent.length !== 0) {
    process.stderr.write("jog wasm host: missing import " + built.absent.join(", ") + "\n");
    process.exit(1);
  }
  const inst = await WebAssembly.instantiate(mod, built.table);
  memory = inst.exports.memory;
  if (!memory) {
    process.stderr.write("jog wasm host: module does not export memory\n");
    process.exit(1);
  }
  if (typeof inst.exports.main !== "function") {
    process.stderr.write("jog wasm host: module does not export main\n");
    process.exit(1);
  }
  const programArgs = [wasmPath].concat(process.argv.slice(3));
  const argvPtr = installArgv(memory, programArgs);
  const code = inst.exports.main(programArgs.length, argvPtr) | 0;
  process.exit(code);
}

main().catch((err) => {
  const text = err && err.stack ? err.stack : String(err);
  process.stderr.write(text + "\n");
  process.exit(1);
});
