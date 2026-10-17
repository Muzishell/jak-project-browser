#pragma once

/*!
 * @file WasmCodeGenerator.h
 * WebAssembly backend (OPENGOAL_WASM_BACKEND only). Turns a colored FileEnv into one wasm module.
 *
 * Module ABI (all imports come from module "env" unless noted):
 *  - memory "memory": linear memory holding EE memory at the imported i32 global "ee_base".
 *  - table "table": shared funcref table. This file's functions are placed by an active element
 *    segment at the imported i32 global "table_base", in FileEnv function order.
 *  - global "st": i32 GOAL address of the symbol table (the x86 st register, r14).
 *  - module "sym", global "<name>": i32 offset of symbol <name> from st, one per symbol used.
 *  - every GOAL function has type (i64 x 8) -> i64. The parameters are the GPR argument registers
 *    (rdi rsi rdx rcx r8 r9 r10 r11) and the result is rax.
 *  - a GOAL function value is an EE address holding the function's u32 table index;
 *    a call is call_indirect on the index loaded from that address.
 *  - each function is exported under its GOAL internal name.
 */

#include <map>
#include <string>
#include <utility>
#include <vector>

#include "common/common_types.h"
#include "common/versions/versions.h"

#include "binaryen-c.h"
#include "goalc/emitter/Register.h"
#include "goalc/regalloc/allocator_interface.h"

class FileEnv;
class FunctionEnv;
class RegVal;

namespace wasm_backend {

constexpr int kGoalArgCount = 8;

class WasmModuleGen;

/*!
 * Per-function state handed to IR::do_codegen_wasm.
 */
class WasmFunctionGen {
 public:
  WasmFunctionGen(WasmModuleGen* module_gen, FunctionEnv* env);

  BinaryenModuleRef module() const;
  GameVersion version() const;
  //! Parameter type shared by every GOAL function.
  BinaryenType goal_params() const;
  const AllocationResult& allocs() const { return m_allocs; }

  //! The register an IR operand is in at the current IR (same rules as the x86 get_reg).
  emitter::Register get_reg(const RegVal* rv) const;

  //! Local holding a register, typed by the register class of the value in it.
  BinaryenIndex reg_local(emitter::Register reg, RegClass reg_class);
  BinaryenExpressionRef get(const RegVal* rv);
  BinaryenExpressionRef set(const RegVal* rv, BinaryenExpressionRef value);
  BinaryenExpressionRef get_reg_value(emitter::Register reg, RegClass reg_class);
  BinaryenExpressionRef set_reg_value(emitter::Register reg,
                                      RegClass reg_class,
                                      BinaryenExpressionRef value);

  //! Linear-memory address of a GOAL address (i32) plus a constant offset.
  BinaryenExpressionRef ee_addr(BinaryenExpressionRef goal_addr_i32, s64 offset = 0);
  //! GOAL address of a symbol (i32): st + symbol offset.
  BinaryenExpressionRef symbol_addr(const std::string& name);
  BinaryenExpressionRef st();

  BinaryenExpressionRef i32(s32 value);
  BinaryenExpressionRef i64(s64 value);
  BinaryenExpressionRef wrap(BinaryenExpressionRef value_i64);

  //! Add an expression to the current basic block.
  void emit(BinaryenExpressionRef expr);
  //! Condition (i32) for the branch that ends the current basic block.
  void set_branch_condition(BinaryenExpressionRef condition);

  //! Called by CodeGenerator: build the function body and add it to the module.
  void build();

 private:
  struct Block {
    int start = 0;  // first IR index
    int end = 0;    // one past the last IR index
    std::vector<BinaryenExpressionRef> code;
    BinaryenExpressionRef branch_condition = nullptr;
    RelooperBlockRef ref = nullptr;
  };

  BinaryenIndex add_local(BinaryenType type);
  BinaryenIndex spill_local(int slot, RegClass reg_class);
  void emit_spill_ops(int ir_idx, bool loads);
  int block_starting_at(int ir_idx) const;

  WasmModuleGen* m_module_gen = nullptr;
  FunctionEnv* m_env = nullptr;
  const AllocationResult& m_allocs;
  int m_ir_idx = -1;
  std::vector<Block> m_blocks;
  int m_current_block = -1;
  std::vector<BinaryenType> m_vars;                             // locals after the parameters
  std::map<std::pair<int, int>, BinaryenIndex> m_reg_locals;    // (reg id, class) -> local
  std::map<std::pair<int, int>, BinaryenIndex> m_spill_locals;  // (slot, class) -> local
};

/*!
 * Whole-module state: imports, function types and the element segment.
 */
class WasmModuleGen {
 public:
  explicit WasmModuleGen(GameVersion version);
  ~WasmModuleGen();
  WasmModuleGen(const WasmModuleGen&) = delete;
  WasmModuleGen& operator=(const WasmModuleGen&) = delete;

  //! Generate the module for a colored file. Throws on unsupported IR.
  std::vector<u8> run(FileEnv* env);

  BinaryenModuleRef module() const { return m_module; }
  GameVersion version() const { return m_version; }
  BinaryenType goal_params() const { return m_goal_params; }
  //! Name of the imported global holding a symbol's offset from st (added on first use).
  const char* symbol_global(const std::string& name);
  //! Text form of the last generated module, for debugging.
  std::string text() const;

 private:
  GameVersion m_version;
  BinaryenModuleRef m_module = nullptr;
  BinaryenType m_goal_params;
  std::map<std::string, std::string> m_symbol_globals;
};

std::string function_wasm_name(const FunctionEnv* env);

}  // namespace wasm_backend
