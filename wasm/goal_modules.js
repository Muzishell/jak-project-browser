// --pre-js of the headless runtime: the JS side of running GOAL code (CLAUDE.MD, M3).
// Runs in every thread; only the EE thread uses it, since all GOAL code runs there. Each thread has
// its own table, so GOAL functions only exist in the EE thread's.

// GOAL's shadow stack pointer, the mutable "sp" global every GOAL module imports
Module.goalStackPointer = () => {
  if (!Module.goalSp) {
    Module.goalSp = new WebAssembly.Global({ value: "i32", mutable: true }, 0);
  }
  return Module.goalSp;
};

// the process pointer (x86 r13), the mutable i64 "pp" global
Module.goalProcessPointer = () => {
  if (!Module.goalPp) {
    Module.goalPp = new WebAssembly.Global({ value: "i64", mutable: true }, 0n);
  }
  return Module.goalPp;
};

// Binaryen Asyncify's state (0 normal, 1 unwinding, 2 rewinding) and the address of its data
// header, the mutable i32 "__asyncify_state" and "__asyncify_data" globals every module imports,
// so a thread suspend unwinds through all modules (wasm/goal_thread.cpp)
Module.goalAsyncifyState = () => {
  if (!Module.goalAsyncState) {
    Module.goalAsyncState = new WebAssembly.Global({ value: "i32", mutable: true }, 0);
  }
  return Module.goalAsyncState;
};
Module.goalAsyncifyData = () => {
  if (!Module.goalAsyncData) {
    Module.goalAsyncData = new WebAssembly.Global({ value: "i32", mutable: true }, 0);
  }
  return Module.goalAsyncData;
};

// modules compiled by klink, waiting to be instantiated once their object is linked
Module.goalModules = new Map();
Module.goalNextModule = 1;

// compile the module of a GOAL object (bytes are copied out of the shared heap) and grow the table
// by its function count. Returns the handle; the table base is written to tableBasePtr.
Module.goalCompileModule = (bytes, tableBasePtr) => {
  const module = new WebAssembly.Module(bytes.slice());
  const functionCount = WebAssembly.Module.exports(module).filter((e) => e.kind === "function")
    .length;
  const tableBase = wasmTable.grow(functionCount);
  HEAPU32[tableBasePtr >> 2] = tableBase;
  const handle = Module.goalNextModule++;
  Module.goalModules.set(handle, { module, tableBase });
  return handle;
};

// instantiate a compiled module against the shared memory and table. symbolOffset(name) is the
// offset of a symbol from st (the kernel interns it). xmmArea is the linear address of the
// 128-bit register area.
Module.goalInstantiateModule = (handle, eeBase, st, xmmArea, segBases, symbolOffset) => {
  const { module, tableBase } = Module.goalModules.get(handle);
  Module.goalModules.delete(handle);
  const i32 = (v) => new WebAssembly.Global({ value: "i32", mutable: false }, v);
  const env = {
    memory: wasmMemory,
    table: wasmTable,
    ee_base: i32(eeBase),
    st: i32(st),
    table_base: i32(tableBase),
    seg_main: i32(segBases[0]),
    seg_debug: i32(segBases[1]),
    seg_top: i32(segBases[2]),
    sp: Module.goalStackPointer(),
    pp: Module.goalProcessPointer(),
    xmm_area: i32(xmmArea),
    __asyncify_state: Module.goalAsyncifyState(),
    __asyncify_data: Module.goalAsyncifyData(),
  };
  const sym = {};
  for (const imp of WebAssembly.Module.imports(module)) {
    if (imp.module === "sym") {
      sym[imp.name] = i32(symbolOffset(imp.name));
    }
  }
  // the active element segment puts the functions in the table at table_base
  new WebAssembly.Instance(module, { env, sym });
};
