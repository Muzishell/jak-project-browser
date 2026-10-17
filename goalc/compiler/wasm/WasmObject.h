#pragma once

#include <vector>

#include "common/common_types.h"

namespace wasm_backend {

//! What the wasm backend produces for one GOAL file (see WasmCodeGenerator.h for the ABI).
struct WasmObject {
  std::vector<u8> object;  // v3 object file: static data and function slots
  std::vector<u8> wasm;    // wasm module: code
};

}  // namespace wasm_backend
