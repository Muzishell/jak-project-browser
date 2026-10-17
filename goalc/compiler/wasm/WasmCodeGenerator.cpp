#include "WasmCodeGenerator.h"

#include <cstdlib>
#include <stdexcept>

#include "goalc/compiler/Compiler.h"
#include "goalc/compiler/Env.h"
#include "goalc/compiler/IR.h"
#include "goalc/compiler/Val.h"

#include "fmt/format.h"

namespace wasm_backend {
namespace {

constexpr const char* kMemory = "memory";
constexpr const char* kTable = "table";
constexpr const char* kEeBase = "ee_base";
constexpr const char* kSt = "st";
constexpr const char* kTableBase = "table_base";

// INT_128 and VECTOR_FLOAT share one v128 local per register
int class_key(RegClass reg_class) {
  return int(reg_class == RegClass::INT_128 ? RegClass::VECTOR_FLOAT : reg_class);
}

BinaryenType wasm_type(RegClass reg_class) {
  switch (reg_class) {
    case RegClass::GPR_64:
      return BinaryenTypeInt64();
    case RegClass::FLOAT:
      return BinaryenTypeFloat32();
    case RegClass::VECTOR_FLOAT:
    case RegClass::INT_128:
      return BinaryenTypeVec128();
    default:
      throw std::runtime_error("wasm backend: register class has no wasm type");
  }
}

}  // namespace

std::string function_wasm_name(const FunctionEnv* env) {
  return env->name();
}

/////////////////////
// WasmFunctionGen
/////////////////////

WasmFunctionGen::WasmFunctionGen(WasmModuleGen* module_gen, FunctionEnv* env)
    : m_module_gen(module_gen), m_env(env), m_allocs(env->alloc_result()) {}

BinaryenModuleRef WasmFunctionGen::module() const {
  return m_module_gen->module();
}

GameVersion WasmFunctionGen::version() const {
  return m_module_gen->version();
}

BinaryenType WasmFunctionGen::goal_params() const {
  return m_module_gen->goal_params();
}

emitter::Register WasmFunctionGen::get_reg(const RegVal* rv) const {
  if (rv->rlet_constraint().has_value()) {
    return rv->rlet_constraint().value();
  }
  const auto& ass = m_allocs.ass_as_ranges.at(rv->ireg().id).get(m_ir_idx);
  if (ass.kind != Assignment::Kind::REGISTER) {
    throw std::runtime_error(
        fmt::format("wasm backend: {} is not in a register at IR {}", rv->print(), m_ir_idx));
  }
  return ass.reg;
}

BinaryenIndex WasmFunctionGen::add_local(BinaryenType type) {
  m_vars.push_back(type);
  return kGoalArgCount + BinaryenIndex(m_vars.size()) - 1;
}

BinaryenIndex WasmFunctionGen::reg_local(emitter::Register reg, RegClass reg_class) {
  if (reg_class == RegClass::GPR_64) {
    // the GPR argument registers are the function parameters
    const auto& info = emitter::reg_info(emitter::InstructionSet::WASM);
    for (int i = 0; i < kGoalArgCount; i++) {
      if (info.get_gpr_arg_reg(i) == reg) {
        return i;
      }
    }
  }
  auto key = std::make_pair(reg.id(), class_key(reg_class));
  auto it = m_reg_locals.find(key);
  if (it != m_reg_locals.end()) {
    return it->second;
  }
  auto idx = add_local(wasm_type(reg_class));
  m_reg_locals[key] = idx;
  return idx;
}

BinaryenIndex WasmFunctionGen::spill_local(int slot, RegClass reg_class) {
  auto key = std::make_pair(slot, class_key(reg_class));
  auto it = m_spill_locals.find(key);
  if (it != m_spill_locals.end()) {
    return it->second;
  }
  auto idx = add_local(wasm_type(reg_class));
  m_spill_locals[key] = idx;
  return idx;
}

BinaryenExpressionRef WasmFunctionGen::get_reg_value(emitter::Register reg, RegClass reg_class) {
  return BinaryenLocalGet(module(), reg_local(reg, reg_class), wasm_type(reg_class));
}

BinaryenExpressionRef WasmFunctionGen::set_reg_value(emitter::Register reg,
                                                     RegClass reg_class,
                                                     BinaryenExpressionRef value) {
  return BinaryenLocalSet(module(), reg_local(reg, reg_class), value);
}

BinaryenExpressionRef WasmFunctionGen::get(const RegVal* rv) {
  return get_reg_value(get_reg(rv), rv->ireg().reg_class);
}

BinaryenExpressionRef WasmFunctionGen::set(const RegVal* rv, BinaryenExpressionRef value) {
  return set_reg_value(get_reg(rv), rv->ireg().reg_class, value);
}

BinaryenExpressionRef WasmFunctionGen::i32(s32 value) {
  return BinaryenConst(module(), BinaryenLiteralInt32(value));
}

BinaryenExpressionRef WasmFunctionGen::i64(s64 value) {
  return BinaryenConst(module(), BinaryenLiteralInt64(value));
}

BinaryenExpressionRef WasmFunctionGen::wrap(BinaryenExpressionRef value_i64) {
  return BinaryenUnary(module(), BinaryenWrapInt64(), value_i64);
}

BinaryenExpressionRef WasmFunctionGen::ee_addr(BinaryenExpressionRef goal_addr_i32, s64 offset) {
  // GOAL offsets can be negative (basic type tags), so they can't go in the memarg offset
  auto addr = goal_addr_i32;
  if (offset != 0) {
    addr = BinaryenBinary(module(), BinaryenAddInt32(), addr, i32(s32(offset)));
  }
  return BinaryenBinary(module(), BinaryenAddInt32(), addr,
                        BinaryenGlobalGet(module(), kEeBase, BinaryenTypeInt32()));
}

BinaryenExpressionRef WasmFunctionGen::st() {
  return BinaryenGlobalGet(module(), kSt, BinaryenTypeInt32());
}

BinaryenExpressionRef WasmFunctionGen::symbol_addr(const std::string& name) {
  return BinaryenBinary(
      module(), BinaryenAddInt32(), st(),
      BinaryenGlobalGet(module(), m_module_gen->symbol_global(name), BinaryenTypeInt32()));
}

void WasmFunctionGen::emit(BinaryenExpressionRef expr) {
  m_blocks.at(m_current_block).code.push_back(expr);
}

void WasmFunctionGen::set_branch_condition(BinaryenExpressionRef condition) {
  m_blocks.at(m_current_block).branch_condition = condition;
}

void WasmFunctionGen::emit_spill_ops(int ir_idx, bool loads) {
  for (const auto& op : m_allocs.stack_ops.at(ir_idx).ops) {
    auto type = wasm_type(op.reg_class);
    if (loads && op.load) {
      emit(set_reg_value(op.reg, op.reg_class,
                         BinaryenLocalGet(module(), spill_local(op.slot, op.reg_class), type)));
    }
    if (!loads && op.store) {
      emit(BinaryenLocalSet(module(), spill_local(op.slot, op.reg_class),
                            get_reg_value(op.reg, op.reg_class)));
    }
  }
}

int WasmFunctionGen::block_starting_at(int ir_idx) const {
  for (int i = 0; i < int(m_blocks.size()); i++) {
    if (m_blocks[i].start == ir_idx) {
      return i;
    }
  }
  throw std::runtime_error(fmt::format("wasm backend: no basic block starts at IR {}", ir_idx));
}

void WasmFunctionGen::build() {
  if (m_env->is_asm_func) {
    throw std::runtime_error(
        fmt::format("wasm backend does not support asm function {}", m_env->name()));
  }
  if (m_allocs.stack_slots_for_vars) {
    throw std::runtime_error(
        fmt::format("wasm backend does not support stack variables yet ({})", m_env->name()));
  }

  const auto& code = m_env->code();
  const int n = int(code.size());

  // basic blocks start at 0, at every jump target and after every jump
  std::vector<RegAllocInstr> rais;
  std::vector<bool> leader(n + 1, false);
  leader[0] = true;
  for (int i = 0; i < n; i++) {
    rais.push_back(code[i]->to_rai());
    const auto& rai = rais.back();
    ASSERT(rai.jumps.size() <= 1);
    for (int target : rai.jumps) {
      ASSERT(target >= 0 && target <= n);
      leader[target] = true;
    }
    if (!rai.jumps.empty() || !rai.fallthrough) {
      leader[i + 1] = true;
    }
  }
  for (int i = 0; i < n; i++) {
    if (leader[i]) {
      if (!m_blocks.empty()) {
        m_blocks.back().end = i;
      }
      m_blocks.push_back({});
      m_blocks.back().start = i;
    }
  }
  if (!m_blocks.empty()) {
    m_blocks.back().end = n;
  }

  // translate each IR, with the spill reloads before and spill stores after, like x86
  for (int b = 0; b < int(m_blocks.size()); b++) {
    m_current_block = b;
    for (int i = m_blocks[b].start; i < m_blocks[b].end; i++) {
      m_ir_idx = i;
      emit_spill_ops(i, true);
      code[i]->do_codegen_wasm(this);
      emit_spill_ops(i, false);
    }
  }

  // structure the control flow
  auto* mod = module();
  auto relooper = RelooperCreate(mod);
  for (auto& block : m_blocks) {
    block.ref = RelooperAddBlock(
        relooper, BinaryenBlock(mod, nullptr, block.code.data(), BinaryenIndex(block.code.size()),
                                BinaryenTypeNone()));
  }
  const auto& info = emitter::reg_info(emitter::InstructionSet::WASM);
  auto exit = RelooperAddBlock(
      relooper, BinaryenReturn(mod, get_reg_value(info.get_gpr_ret_reg(), RegClass::GPR_64)));
  auto block_for = [&](int ir_idx) {
    return ir_idx == n ? exit : m_blocks.at(block_starting_at(ir_idx)).ref;
  };

  for (auto& block : m_blocks) {
    const auto& last = rais.at(block.end - 1);
    auto next = block_for(block.end);
    if (!last.jumps.empty()) {
      auto target = block_for(last.jumps.at(0));
      if (!last.fallthrough) {
        RelooperAddBranch(block.ref, target, nullptr, nullptr);
      } else if (target == next) {
        // both ways go to the same block, the condition has no side effects
        RelooperAddBranch(block.ref, next, nullptr, nullptr);
      } else {
        if (!block.branch_condition) {
          throw std::runtime_error("wasm backend: conditional jump without a condition");
        }
        RelooperAddBranch(block.ref, target, block.branch_condition, nullptr);
        RelooperAddBranch(block.ref, next, nullptr, nullptr);
      }
    } else if (last.fallthrough) {
      RelooperAddBranch(block.ref, next, nullptr, nullptr);
    }
  }

  auto entry = m_blocks.empty() ? exit : m_blocks.front().ref;
  auto label_helper = add_local(BinaryenTypeInt32());
  BinaryenExpressionRef body_parts[2] = {RelooperRenderAndDispose(relooper, entry, label_helper),
                                         BinaryenUnreachable(mod)};
  auto body = BinaryenBlock(mod, nullptr, body_parts, 2, BinaryenTypeInt64());

  auto name = function_wasm_name(m_env);
  BinaryenAddFunction(mod, name.c_str(), m_module_gen->goal_params(), BinaryenTypeInt64(),
                      m_vars.data(), BinaryenIndex(m_vars.size()), body);
}

/////////////////////
// WasmModuleGen
/////////////////////

WasmModuleGen::WasmModuleGen(GameVersion version)
    : m_version(version), m_module(BinaryenModuleCreate()) {
  BinaryenModuleSetFeatures(m_module, BinaryenFeatureMVP() | BinaryenFeatureMutableGlobals() |
                                          BinaryenFeatureSignExt() | BinaryenFeatureSIMD128() |
                                          BinaryenFeatureBulkMemory());

  BinaryenType params[kGoalArgCount];
  for (auto& p : params) {
    p = BinaryenTypeInt64();
  }
  m_goal_params = BinaryenTypeCreate(params, kGoalArgCount);

  // no maximum: the importer decides the size
  BinaryenSetMemory(m_module, 0, BinaryenIndex(-1), nullptr, nullptr, nullptr, nullptr, nullptr,
                    nullptr, 0, false, false, kMemory);
  BinaryenAddMemoryImport(m_module, kMemory, "env", kMemory, false);
  BinaryenAddTable(m_module, kTable, 0, BinaryenIndex(-1), BinaryenTypeFuncref(), nullptr);
  BinaryenAddTableImport(m_module, kTable, "env", kTable);
  for (const char* global : {kEeBase, kSt, kTableBase}) {
    BinaryenAddGlobalImport(m_module, global, "env", global, BinaryenTypeInt32(), false);
  }
}

WasmModuleGen::~WasmModuleGen() {
  BinaryenModuleDispose(m_module);
}

const char* WasmModuleGen::symbol_global(const std::string& name) {
  auto it = m_symbol_globals.find(name);
  if (it == m_symbol_globals.end()) {
    it = m_symbol_globals.emplace(name, "sym:" + name).first;
    BinaryenAddGlobalImport(m_module, it->second.c_str(), "sym", name.c_str(), BinaryenTypeInt32(),
                            false);
  }
  return it->second.c_str();
}

std::vector<u8> WasmModuleGen::run(FileEnv* env) {
  std::vector<std::string> names;
  for (auto& f : env->functions()) {
    WasmFunctionGen gen(this, f.get());
    gen.build();
    names.push_back(function_wasm_name(f.get()));
  }

  std::vector<const char*> name_ptrs;
  for (auto& name : names) {
    name_ptrs.push_back(name.c_str());
    BinaryenAddFunctionExport(m_module, name.c_str(), name.c_str());
  }
  BinaryenAddActiveElementSegment(m_module, kTable, "functions", name_ptrs.data(),
                                  BinaryenIndex(name_ptrs.size()),
                                  BinaryenGlobalGet(m_module, kTableBase, BinaryenTypeInt32()));

  if (!BinaryenModuleValidate(m_module)) {
    throw std::runtime_error(
        fmt::format("wasm backend generated an invalid module for {}", env->name()));
  }

  auto result = BinaryenModuleAllocateAndWrite(m_module, nullptr);
  std::vector<u8> bytes((u8*)result.binary, (u8*)result.binary + result.binaryBytes);
  free(result.binary);
  if (result.sourceMap) {
    free(result.sourceMap);
  }
  return bytes;
}

std::string WasmModuleGen::text() const {
  char* text = BinaryenModuleAllocateAndWriteText(m_module);
  std::string result(text);
  free(text);
  return result;
}

}  // namespace wasm_backend

std::vector<u8> Compiler::compile_wasm_module_from_string(const std::string& src,
                                                          const std::string& obj_name,
                                                          std::string* text_out) {
  if (m_instr_set != emitter::InstructionSet::WASM) {
    throw std::runtime_error("compile_wasm_module_from_string needs the WASM instruction set");
  }
  auto code = m_goos.reader.read_from_string({src});
  auto compiled = compile_object_file(obj_name, code, true);
  color_object_file(compiled);
  wasm_backend::WasmModuleGen gen(m_version);
  auto result = gen.run(compiled);
  if (text_out) {
    *text_out = gen.text();
  }
  compiled->cleanup_after_codegen();
  return result;
}
