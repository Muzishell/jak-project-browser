#pragma once

namespace emitter {
// WASM is a compiler-only target: it reuses the x86 register layout (each register becomes a
// wasm local) and never goes through IGen. See CLAUDE.MD, architecture decision 1.
enum class InstructionSet { X86, ARM64, WASM };

//! Native target for tests that execute generated code.
//! Compiler targets must be selected explicitly.
constexpr InstructionSet kNativeInstructionSet =
#ifdef __aarch64__
    InstructionSet::ARM64;
#else
    InstructionSet::X86;
#endif

//! Name used for INSTRUCTION_SET in GOOS and in error messages.
constexpr const char* instruction_set_name(InstructionSet instr_set) {
  switch (instr_set) {
    case InstructionSet::ARM64:
      return "arm64";
    case InstructionSet::WASM:
      return "wasm";
    default:
      return "x86";
  }
}
};  // namespace emitter
