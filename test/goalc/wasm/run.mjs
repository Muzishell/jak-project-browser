// Minimal no-kernel GOAL runtime for running wasm-backend output under Node (M2 test harness).
//
// Usage: node run.mjs --true-offset N --empty-offset N <a.o> <a.wasm> [<b.o> <b.wasm> ...]
//
// Links each v3 object like game/kernel/jak1/klink.cpp (symbol, type and pointer links), puts the
// module's functions in the shared table, then calls the top-level function like the -nokernel
// kernel loop (kboot.cpp: cprintf("%ld\n")). Files run in order in one runtime, like the files of
// a native test suite share one runtime; only the last file's output is printed (format output
// first, then the result). Only the kernel functions the compiler tests need exist here;
// M3 replaces all of this with the real C++ runtime built with Emscripten.

import fs from "node:fs";

// ---- arguments ----
const args = process.argv.slice(2);
const opts = { trueOffset: null, emptyOffset: null, files: [] };
for (let i = 0; i < args.length; i++) {
  if (args[i] === "--true-offset") opts.trueOffset = Number(args[++i]);
  else if (args[i] === "--empty-offset") opts.emptyOffset = Number(args[++i]);
  else opts.files.push(args[i]);
}
if (opts.trueOffset === null || opts.emptyOffset === null || opts.files.length < 2 ||
    opts.files.length % 2) {
  console.error("usage: run.mjs --true-offset N --empty-offset N <a.o> <a.wasm> [...]");
  process.exit(2);
}
const debug = process.env.WASM_RUN_DEBUG ? (msg) => console.error(`[run.mjs] ${msg}`) : () => {};

// ---- EE memory ----
// the layout follows the native jak1 runtime where tests can observe it: 128 MB of EE memory,
// s7 at the native address, the stack just below the top of EE memory
// shared like the pthread runtime's, which the modules declare
const memory = new WebAssembly.Memory({ initial: 2048, maximum: 2048, shared: true }); // 128 MB, EE_MAIN_MEM_SIZE
const table = new WebAssembly.Table({ initial: 0, element: "anyfunc" });
const EE_BASE = 0; // GOAL address == linear memory address
const u8 = () => new Uint8Array(memory.buffer);
const dv = () => new DataView(memory.buffer);
const r32 = (addr) => dv().getUint32(EE_BASE + addr, true);
const w32 = (addr, v) => dv().setUint32(EE_BASE + addr, v >>> 0, true);

let heapTop = 0x400000; // below this: the symbol table
// shadow stack for functions with stack variables: x86 starts the GOAL stack at
// EE_MAIN_MEM_SIZE - GOAL_STACK_TOP_OFFSET (8); keep it 16-byte aligned
const stackPointer = new WebAssembly.Global({ value: "i32", mutable: true }, 0x8000000 - 16);
// process pointer (r13): call_goal from C starts it at the symbol table, like the x86 stubs
let processPointer = null;
// 128-bit register area (xmm0-8, 16 bytes each), a linear address in the unused low memory
const XMM_AREA = 0x100;
// Asyncify state shared by the modules; nothing suspends here, so it stays 0 (normal)
const asyncifyState = new WebAssembly.Global({ value: "i32", mutable: true }, 0);
const asyncifyData = new WebAssembly.Global({ value: "i32", mutable: true }, 0);
function alloc(size, align = 16) {
  heapTop = Math.ceil(heapTop / align) * align;
  const addr = heapTop;
  heapTop += size;
  u8().fill(0, EE_BASE + addr, EE_BASE + heapTop);
  return addr;
}

// ---- symbol table ----
// GOAL address of s7; symbols are 4-byte value slots at st + offset. New symbols get offsets
// that are 4 mod 8 like basics, so they never look like pairs (2) or bintegers (0).
const ST = 0x14fd24; // jak1 s7 (variables/get-symbol tests print it)
const symbolOffsets = new Map([["#f", 0], ["#t", opts.trueOffset]]);
let nextSymbolOffset = 0x1004;
let symbolType = 0; // symbols are basics: the symbol type tag is in the word before the value
function intern(name) {
  if (!symbolOffsets.has(name)) {
    symbolOffsets.set(name, nextSymbolOffset);
    w32(ST + nextSymbolOffset, ST); // unbound symbols hold #f
    if (symbolType) w32(ST + nextSymbolOffset - 4, symbolType);
    nextSymbolOffset += 8;
  }
  return symbolOffsets.get(name);
}
w32(ST, ST); // #f
w32(ST + opts.trueOffset, ST + opts.trueOffset); // #t
const FALSE = ST;
const TRUE = ST + opts.trueOffset;
const EMPTY_PAIR = ST + opts.emptyOffset;

// ---- kernel functions (C functions in the real runtime) ----
// A JS function can't go in a funcref table directly; re-export it from a tiny wasm module.
const wrapperModule = (() => {
  const i64 = 0x7e;
  const str = (s) => [s.length, ...Buffer.from(s)];
  const section = (id, body) => [id, body.length, ...body];
  return new WebAssembly.Module(new Uint8Array([
    0x00, 0x61, 0x73, 0x6d, 0x01, 0x00, 0x00, 0x00,
    ...section(1, [1, 0x60, 8, ...Array(8).fill(i64), 1, i64]), // type: (i64 x8) -> i64
    ...section(2, [1, ...str("env"), ...str("f"), 0x00, 0]), // import env.f
    ...section(7, [1, ...str("f"), 0x00, 0]), // export f
  ]));
})();

// ---- types: basics laid out like jak1 Type (kscheme.h) ----
// [tag] symbol(0) parent(4) allocated_size(8) padded_size(10) heap_base(12) num_methods(14)
// method table(16...). Methods nobody defined trap.
const MAX_METHODS = 64;
const types = new Map();
let missingMethodSlot = 0;
let functionType = 0;
// every type prints with the kernel's print_object (method 2, GOAL_PRINT_METHOD); set once the
// print function exists
const GOAL_PRINT_METHOD = 2;
let printMethodSlot = 0;
const setPrintMethod = (type) => w32(type + 16 + 4 * GOAL_PRINT_METHOD, printMethodSlot);
function allocType(name, methodCount) {
  const addr = alloc(4 + 16 + 4 * MAX_METHODS) + 4;
  types.set(name, addr);
  w32(addr, ST + intern(name));
  dv().setUint16(EE_BASE + addr + 14, methodCount, true);
  for (let i = 0; i < MAX_METHODS; i++) w32(addr + 16 + 4 * i, missingMethodSlot);
  if (printMethodSlot) setPrintMethod(addr);
  w32(ST + intern(name), addr);
  return addr;
}
function internType(name, methodCount = 0) {
  const addr = types.has(name) ? types.get(name) : allocType(name, methodCount);
  const count = dv().getUint16(EE_BASE + addr + 14, true);
  if (methodCount > count) dv().setUint16(EE_BASE + addr + 14, methodCount, true);
  return addr;
}

function makeFunction(fn) {
  const wrapped = new WebAssembly.Instance(wrapperModule, { env: { f: fn } }).exports.f;
  const index = table.grow(1);
  table.set(index, wrapped);
  const slot = alloc(8) + 4; // type tag, then the table index
  w32(slot - 4, functionType);
  w32(slot, index);
  return slot;
}
function defineKernelFunction(name, fn) {
  w32(ST + intern(name), makeFunction(fn));
}

// bootstrap: the function type, then the trap for missing methods, then fill it in
functionType = allocType("function", 9);
missingMethodSlot = makeFunction(() => {
  throw new Error("called a method the wasm test runtime does not have");
});
for (let i = 0; i < MAX_METHODS; i++) w32(functionType + 16 + 4 * i, missingMethodSlot);
w32(missingMethodSlot - 4, functionType);

// symbol type tags, now that types exist
symbolType = internType("symbol", 9);
for (const offset of symbolOffsets.values()) w32(ST + offset - 4, symbolType);

// the empty pair's car and cdr are itself, as in the kernel
w32(EMPTY_PAIR - 2, EMPTY_PAIR);
w32(EMPTY_PAIR + 2, EMPTY_PAIR);

let output = "";

function readCString(addr) {
  const bytes = u8();
  let end = EE_BASE + addr;
  while (bytes[end]) end++;
  return Buffer.from(bytes.subarray(EE_BASE + addr, end)).toString("latin1");
}

// GOAL strings: [type tag] [u32 allocated length] [chars] ; the pointer is after the tag
function goalString(addr) {
  return readCString(addr + 4);
}

function makeGoalString(text) {
  const addr = alloc(4 + 4 + text.length + 1) + 4;
  w32(addr - 4, internType("string"));
  w32(addr, text.length);
  u8().set(Buffer.from(text, "latin1"), EE_BASE + addr + 4);
  return addr;
}

const asSigned = (v) => BigInt.asIntN(64, v);
const asUnsigned = (v) => BigInt.asUintN(64, v);

// a float argument is the low 32 bits of the register
const f32Bits = new Float32Array(1);
const f32BitsU = new Uint32Array(f32Bits.buffer);
function asFloat(v) {
  f32BitsU[0] = Number(asUnsigned(v) & 0xffffffffn);
  return f32Bits[0];
}

// cvt_float in game/kernel/common/kprint.cpp: single precision, digit by digit, no rounding
function cvtFloat(x, precision) {
  const f = Math.fround;
  let abs = f(x);
  let lead = "";
  if (abs < 0) {
    abs = f(-abs);
    lead = "-";
  }
  if (Number.isNaN(abs)) return "NaN";
  let integerPart = Math.trunc(abs);
  let fractionPart = f(abs - integerPart);
  let digits = "";
  while (integerPart !== 0) {
    const scaled = f(integerPart / 10);
    integerPart = Math.trunc(scaled);
    const rounder = f(f(f(scaled - integerPart) * 10) + 0.5);
    digits = String.fromCharCode(48 + Math.trunc(rounder)) + digits;
  }
  if (!digits) digits = "0";
  if (precision) digits += ".";
  let prec = precision;
  if (fractionPart !== 0 && precision) {
    do {
      const scaled = f(fractionPart * 10);
      const nextInt = Math.trunc(scaled);
      fractionPart = f(scaled - nextInt);
      digits += String.fromCharCode(48 + nextInt);
      prec--;
    } while (prec && fractionPart !== 0);
  }
  while (prec-- > 0) digits += "0";
  return lead + digits;
}

// the parts of the kernel's print_object (kscheme.cpp) the compiler tests need:
// symbols, strings and pairs (print_pair)
const OFFSET_MASK = 7, PAIR_OFFSET = 2;
function printObject(value) {
  const addr = Number(asUnsigned(value) & 0xffffffffn);
  for (const [name, offset] of symbolOffsets) {
    if (addr === ST + offset) return name;
  }
  if (addr === EMPTY_PAIR) return "()";
  if ((addr & OFFSET_MASK) === PAIR_OFFSET) {
    let out = "(";
    let toPrint = addr;
    for (;;) {
      if ((toPrint & OFFSET_MASK) === PAIR_OFFSET) {
        out += printObject(BigInt(r32(toPrint - 2)));
        const cdr = r32(toPrint + 2);
        toPrint = cdr;
        if (cdr === EMPTY_PAIR) return out + ")";
        out += " ";
      } else {
        return out + ". " + printObject(BigInt(toPrint)) + ")";
      }
    }
  }
  // print_string quotes the text
  if (addr >= 4 && r32(addr - 4) === types.get("string")) return `"${goalString(addr)}"`;
  throw new Error(`print: can't print 0x${addr.toString(16)} in the wasm test runtime`);
}

// subset of kernel format (game/kernel/common/kprint.cpp) used by the compiler tests
function format(dest, fmt, ...rest) {
  const text = goalString(Number(asUnsigned(fmt) & 0xffffffffn));
  const values = rest.slice(0, 6);
  let out = "";
  let arg = 0;
  for (let i = 0; i < text.length; i++) {
    if (text[i] !== "~") {
      out += text[i];
      continue;
    }
    // ~[params]directive ; params are comma separated, e.g. ~,,5f
    let j = i + 1;
    while (j < text.length && /[0-9,'+-]/.test(text[j])) j++;
    const directive = text[j];
    const params = text.slice(i + 1, j).split(",");
    i = j;
    switch (directive) {
      case "f":
      case "F": {
        const precision = params[2] ? parseInt(params[2], 10) : 4;
        const width = params[0] ? parseInt(params[0], 10) : 0;
        out += cvtFloat(asFloat(values[arg++]), precision).padStart(width, " ");
        break;
      }
      case "%":
        out += "\n";
        break;
      case "~":
        out += "~";
        break;
      case "D":
      case "d":
        out += asSigned(values[arg++]).toString(10);
        break;
      case "A":
      case "a":
        out += printObject(values[arg++]);
        break;
      case "X":
      case "x":
        out += asUnsigned(values[arg++]).toString(16);
        break;
      default:
        throw new Error(`format: directive ~${directive} not supported by the wasm test runtime`);
    }
  }
  const destAddr = Number(asUnsigned(dest) & 0xffffffffn);
  if (destAddr === TRUE) {
    output += out;
    return 0n;
  }
  if (destAddr === FALSE) {
    return BigInt(makeGoalString(out));
  }
  throw new Error("format: only #t and #f destinations are supported by the wasm test runtime");
}
// _format is the C kernel function, format the name GOAL code normally uses for it
defineKernelFunction("_format", format);
defineKernelFunction("format", format);

// pair is a C kernel type; its new method is new_pair: (heap type car cdr) -> pair
// (car at -2, cdr at +2 from the pair pointer)
{
  const pair = internType("pair", 9);
  w32(pair + 16, makeFunction((heap, type, car, cdr) => {
    const mem = alloc(8, 8);
    w32(mem, Number(asUnsigned(car) & 0xffffffffn));
    w32(mem + 4, Number(asUnsigned(cdr) & 0xffffffffn));
    return BigInt(mem + PAIR_OFFSET);
  }));
}

// print and string->symbol (kscheme.cpp)
const printFunction = (obj) => {
  output += printObject(obj);
  return obj;
};
defineKernelFunction("print", printFunction);
printMethodSlot = makeFunction(printFunction);
for (const type of types.values()) setPrintMethod(type);
defineKernelFunction("string->symbol", (str) =>
  BigInt(ST + intern(goalString(Number(asUnsigned(str) & 0xffffffffn)))));

// kscheme.cpp test_function: s32 arguments and result
defineKernelFunction("test-function", (a, b, c, d) => {
  const s32 = (v) => BigInt.asIntN(32, v);
  return BigInt.asIntN(32, s32(a) + 2n * s32(b) + 3n * s32(c) + 4n * s32(d));
});

// ---- linker for v3 object files (see klink.cpp jak1_work_v3) ----
const LINK_TABLE_END = 0, LINK_SYMBOL_OFFSET = 1, LINK_TYPE_PTR = 2, LINK_PTR = 5;
const N_SEG = 3, TOP_LEVEL_SEGMENT = 2;
const SEG_NAMES = ["seg_main", "seg_debug", "seg_top"];

function link(objectBytes) {
  const obj = new DataView(objectBytes.buffer, objectBytes.byteOffset, objectBytes.byteLength);
  if (Buffer.from(objectBytes.subarray(0, 4)).toString() !== "GOAL") throw new Error("not a GOAL object");
  if (obj.getUint32(8, true) !== 3) throw new Error("only v3 objects are supported");
  if (obj.getUint32(12, true) !== N_SEG) throw new Error("unexpected segment count");
  // after "GOAL": major, minor, version, count (12 bytes), then {offset,size} x3 link, x3 code
  const linkInfo = (seg) => ({ offset: 4 + obj.getUint32(16 + 8 * seg, true), size: obj.getUint32(20 + 8 * seg, true) });
  const codeInfo = (seg) => ({ offset: obj.getUint32(40 + 8 * seg, true), size: obj.getUint32(44 + 8 * seg, true) });
  const dataStart = obj.getUint32(64, true);

  const segBase = [0, 0, 0];
  for (let seg = N_SEG - 1; seg >= 0; seg--) {
    const code = codeInfo(seg);
    if (code.size === 0) continue;
    segBase[seg] = alloc(code.size, 64);
    u8().set(objectBytes.subarray(dataStart + code.offset, dataStart + code.offset + code.size), EE_BASE + segBase[seg]);
  }

  let functionCount = 0;
  const functionSlots = [];
  for (let seg = 0; seg < N_SEG; seg++) {
    if (!segBase[seg]) continue;
    const info = linkInfo(seg);
    let p = info.offset;
    const readName = () => {
      let end = p;
      while (objectBytes[end]) end++;
      const name = Buffer.from(objectBytes.subarray(p, end)).toString("latin1");
      p = end + 1;
      return name;
    };
    while (objectBytes[p] !== LINK_TABLE_END) {
      const kind = objectBytes[p++];
      if (kind === LINK_SYMBOL_OFFSET) {
        const name = readName();
        const offset = intern(name);
        const count = obj.getUint32(p, true); p += 4;
        for (let i = 0; i < count; i++, p += 4) {
          const at = segBase[seg] + obj.getInt32(p, true);
          // -1 means store the symbol's address, anything else means its offset from st
          w32(at, dv().getInt32(EE_BASE + at, true) === -1 ? ST + offset : offset);
        }
      } else if (kind === LINK_TYPE_PTR) {
        const name = readName();
        const type = internType(name, objectBytes[p++]); // jak1: the method count
        const count = obj.getUint32(p, true); p += 4;
        for (let i = 0; i < count; i++, p += 4) {
          const at = segBase[seg] + obj.getInt32(p, true);
          w32(at, type);
          if (name === "function") functionSlots.push(at + 4);
        }
      } else if (kind === LINK_PTR) {
        const source = obj.getUint32(p, true), dest = obj.getUint32(p + 4, true);
        p += 8;
        w32(segBase[seg] + source, segBase[seg] + dest);
      } else {
        throw new Error(`link kind ${kind} is not used by wasm objects`);
      }
    }
    functionCount = functionSlots.length;
  }

  // functions: the slot holds the index in this file, the table holds them from table_base
  const tableBase = table.grow(functionCount);
  for (const slot of functionSlots) w32(slot, r32(slot) + tableBase);
  debug(`segments ${segBase.map((b) => "0x" + b.toString(16))}, table_base ${tableBase}, ` +
        `slots ${functionSlots.map((s) => `0x${s.toString(16)}=${r32(s)}`)}`);
  return { segBase, tableBase };
}

// ---- run ----
const i32 = (v) => new WebAssembly.Global({ value: "i32", mutable: false }, v);

function runFile(objectPath, wasmPath) {
  const objectBytes = new Uint8Array(fs.readFileSync(objectPath));
  const module = new WebAssembly.Module(fs.readFileSync(wasmPath));
  const { segBase, tableBase } = link(objectBytes);

  processPointer ??= new WebAssembly.Global({ value: "i64", mutable: true }, BigInt(ST));
  const env = { memory, table, ee_base: i32(EE_BASE), st: i32(ST), table_base: i32(tableBase),
                sp: stackPointer, pp: processPointer, xmm_area: i32(XMM_AREA),
                __asyncify_state: asyncifyState, __asyncify_data: asyncifyData };
  SEG_NAMES.forEach((name, seg) => (env[name] = i32(segBase[seg])));
  const sym = {};
  for (const imp of WebAssembly.Module.imports(module)) {
    if (imp.module === "sym") sym[imp.name] = i32(intern(imp.name));
  }
  new WebAssembly.Instance(module, { env, sym });

  // like link_and_exec: the entry is the top-level function at the start of the top-level segment
  const entry = segBase[TOP_LEVEL_SEGMENT] + 4;
  const result = table.get(r32(entry))(0n, 0n, 0n, 0n, 0n, 0n, 0n, 0n);
  output += `${asSigned(result)}\n`;
}

for (let i = 0; i < opts.files.length; i += 2) {
  output = ""; // earlier files only set up state
  debug(`running ${opts.files[i]}`);
  runFile(opts.files[i], opts.files[i + 1]);
}
process.stdout.write(output);
