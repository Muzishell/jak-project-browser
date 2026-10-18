#pragma once

#include <vector>

#include "common/common_types.h"

namespace wasm_backend {

//! What the wasm backend produces for one GOAL file (see WasmCodeGenerator.h for the ABI).
struct WasmObject {
  std::vector<u8> object;  // v3 object file: static data and function slots
  std::vector<u8> wasm;    // wasm module: code
};

//! The single file goalc writes (out/<game>-wasm/obj/*.o, then packed into DGOs as usual):
//! the v3 object, the module size as a little-endian u32, then the module. The wasm runtime's
//! klink finds the size right after the last segment's data.
inline std::vector<u8> pack_wasm_object(const WasmObject& obj) {
  std::vector<u8> out = obj.object;
  u32 size = obj.wasm.size();
  for (int i = 0; i < 4; i++) {
    out.push_back(u8(size >> (8 * i)));
  }
  out.insert(out.end(), obj.wasm.begin(), obj.wasm.end());
  return out;
}

}  // namespace wasm_backend
