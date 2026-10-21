#include "WasmCodeGenerator.h"

#include <cstdlib>
#include <stdexcept>

#include "common/link_types.h"

#include "goalc/compiler/Compiler.h"
#include "goalc/compiler/Env.h"
#include "goalc/compiler/IR.h"
#include "goalc/compiler/StaticObject.h"
#include "goalc/compiler/Val.h"
#include "goalc/debugger/DebugInfo.h"
#include "goalc/emitter/Instruction.h"
#include "goalc/emitter/ObjectGenerator.h"

#include "fmt/format.h"

namespace wasm_backend {
namespace {

constexpr const char* kMemory = "memory";
constexpr const char* kTable = "table";
constexpr const char* kEeBase = "ee_base";
constexpr const char* kSt = "st";
constexpr const char* kTableBase = "table_base";
constexpr BinaryenIndex kMaxMemoryPages = 65536;  // 4 GB of 64 KB pages

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
  if (reg_class == RegClass::GPR_64) {
    // the special registers live across calls, so they are globals, not locals
    const auto& info = emitter::reg_info(emitter::InstructionSet::WASM);
    if (reg == info.get_process_reg()) {
      return BinaryenGlobalGet(module(), m_module_gen->process_pointer_global(),
                               BinaryenTypeInt64());
    }
    if (reg == info.get_st_reg()) {
      return BinaryenUnary(module(), BinaryenExtendUInt32(), st());
    }
    if (reg == info.get_offset_reg()) {
      return BinaryenUnary(module(), BinaryenExtendUInt32(),
                           BinaryenGlobalGet(module(), kEeBase, BinaryenTypeInt32()));
    }
    if (reg == emitter::RSP) {
      // rsp is the shadow stack pointer, a GOAL address (wasm kernel code only)
      return BinaryenUnary(
          module(), BinaryenExtendUInt32(),
          BinaryenGlobalGet(module(), m_module_gen->stack_pointer_global(), BinaryenTypeInt32()));
    }
  }
  return BinaryenLocalGet(module(), reg_local(reg, reg_class), wasm_type(reg_class));
}

BinaryenExpressionRef WasmFunctionGen::set_reg_value(emitter::Register reg,
                                                     RegClass reg_class,
                                                     BinaryenExpressionRef value) {
  if (reg_class == RegClass::GPR_64) {
    const auto& info = emitter::reg_info(emitter::InstructionSet::WASM);
    if (reg == info.get_process_reg()) {
      return BinaryenGlobalSet(module(), m_module_gen->process_pointer_global(), value);
    }
    if (reg == emitter::RSP) {
      return BinaryenGlobalSet(module(), m_module_gen->stack_pointer_global(), wrap(value));
    }
    if (reg == info.get_st_reg() || reg == info.get_offset_reg()) {
      throw std::runtime_error(fmt::format("wasm backend: register {} can't be written",
                                           reg.print(emitter::InstructionSet::WASM)));
    }
  }
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

namespace {
BinaryenExpressionRef segment_offset_addr(BinaryenModuleRef mod,
                                          WasmModuleGen* module_gen,
                                          std::pair<int, int> location) {
  return BinaryenBinary(
      mod, BinaryenAddInt32(),
      BinaryenGlobalGet(mod, module_gen->segment_global(location.first), BinaryenTypeInt32()),
      BinaryenConst(mod, BinaryenLiteralInt32(location.second)));
}
}  // namespace

BinaryenExpressionRef WasmFunctionGen::function_addr(int idx_in_file) {
  return segment_offset_addr(module(), m_module_gen, m_module_gen->function_location(idx_in_file));
}

BinaryenExpressionRef WasmFunctionGen::static_addr(const StaticObject* obj) {
  auto location = m_module_gen->static_location(obj);
  location.second += obj->get_addr_offset();
  return segment_offset_addr(module(), m_module_gen, location);
}

namespace {
//! The register area slot of an xmm register: xmm0 (result) is slot 0, xmm1-8 (arguments) 1-8.
int xmm_area_slot(emitter::Register reg) {
  int slot = reg.id() - emitter::XMM0;
  ASSERT(slot >= 0 && slot < kXmmAreaSlots);
  return slot;
}
}  // namespace

BinaryenExpressionRef WasmFunctionGen::xmm_area_addr(emitter::Register reg) {
  return BinaryenBinary(
      module(), BinaryenAddInt32(),
      BinaryenGlobalGet(module(), m_module_gen->xmm_area_global(), BinaryenTypeInt32()),
      i32(16 * xmm_area_slot(reg)));
}

BinaryenExpressionRef WasmFunctionGen::store_xmm_area(emitter::Register reg,
                                                      BinaryenExpressionRef value) {
  return BinaryenStore(module(), 16, 0, 0, xmm_area_addr(reg), value, BinaryenTypeVec128(),
                       "memory");
}

BinaryenExpressionRef WasmFunctionGen::load_xmm_area(emitter::Register reg) {
  return BinaryenLoad(module(), 16, false, 0, 0, BinaryenTypeVec128(), xmm_area_addr(reg),
                      "memory");
}

void WasmFunctionGen::emit(BinaryenExpressionRef expr) {
  m_blocks.at(m_current_block).code.push_back(expr);
}

void WasmFunctionGen::set_branch_condition(BinaryenExpressionRef condition) {
  m_blocks.at(m_current_block).branch_condition = condition;
}

BinaryenExpressionRef WasmFunctionGen::frame_addr(int byte_offset) {
  ASSERT(m_frame_size > 0 && byte_offset >= 0 && byte_offset < m_frame_size);
  return BinaryenBinary(module(), BinaryenAddInt32(),
                        BinaryenLocalGet(module(), m_frame_local, BinaryenTypeInt32()),
                        i32(byte_offset));
}

namespace {
//! Memory width of a spilled value, as on x86 (FLOAT moves 32 bits of its 8-byte slot).
int spill_bytes(RegClass reg_class) {
  switch (reg_class) {
    case RegClass::GPR_64:
      return 8;
    case RegClass::FLOAT:
      return 4;
    default:
      return 16;
  }
}
}  // namespace

void WasmFunctionGen::emit_spill_ops(int ir_idx, bool loads) {
  for (const auto& op : m_allocs.stack_ops.at(ir_idx).ops) {
    auto type = wasm_type(op.reg_class);
    if (m_memory_spill_slots.count(op.slot)) {
      // address-taken variable: its value lives in the frame
      auto offset = m_allocs.get_slot_for_spill(op.slot) * emitter::GPR_SIZE;
      auto bytes = spill_bytes(op.reg_class);
      if (loads && op.load) {
        emit(set_reg_value(op.reg, op.reg_class,
                           BinaryenLoad(module(), bytes, false, 0, 0, type,
                                        ee_addr(frame_addr(offset)), "memory")));
      }
      if (!loads && op.store) {
        emit(BinaryenStore(module(), bytes, 0, 0, ee_addr(frame_addr(offset)),
                           get_reg_value(op.reg, op.reg_class), type, "memory"));
      }
      continue;
    }
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
  // address-taken variables are spilled to memory so their address means something
  for (auto& rv : m_env->reg_vals()) {
    if (rv->forced_on_stack()) {
      m_memory_spill_slots.insert(m_allocs.ass_as_ranges.at(rv->ireg().id).stack_slot());
    }
  }
  if (m_allocs.stack_slots_for_vars || !m_memory_spill_slots.empty()) {
    // same frame layout as x86: spill slots and stack variable slots, 16-byte aligned
    m_frame_size =
        emitter::GPR_SIZE * (m_allocs.stack_slots_for_spills + m_allocs.stack_slots_for_vars);
    m_frame_size = (m_frame_size + 15) & ~15;
    m_frame_local = add_local(BinaryenTypeInt32());
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
    if (b == 0) {
      // 128-bit arguments: the caller left them in the register area
      for (const auto& constr : m_env->constraints()) {
        if (constr.instr_idx == 0 &&
            constr.desired_register.is_xmm(emitter::InstructionSet::WASM)) {
          emit(set_reg_value(constr.desired_register, RegClass::INT_128,
                             load_xmm_area(constr.desired_register)));
        }
      }
    }
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
  auto ret = BinaryenReturn(mod, get_reg_value(info.get_gpr_ret_reg(), RegClass::GPR_64));
  RelooperBlockRef prologue = nullptr;
  RelooperBlockRef exit = nullptr;
  if (m_frame_size) {
    const char* sp = m_module_gen->stack_pointer_global();
    auto sp_get = [&]() { return BinaryenGlobalGet(mod, sp, BinaryenTypeInt32()); };
    BinaryenExpressionRef enter[2] = {
        BinaryenLocalSet(mod, m_frame_local,
                         BinaryenBinary(mod, BinaryenSubInt32(), sp_get(), i32(m_frame_size))),
        BinaryenGlobalSet(mod, sp, BinaryenLocalGet(mod, m_frame_local, BinaryenTypeInt32()))};
    prologue =
        RelooperAddBlock(relooper, BinaryenBlock(mod, nullptr, enter, 2, BinaryenTypeNone()));
    BinaryenExpressionRef leave[2] = {
        BinaryenGlobalSet(mod, sp,
                          BinaryenBinary(mod, BinaryenAddInt32(),
                                         BinaryenLocalGet(mod, m_frame_local, BinaryenTypeInt32()),
                                         i32(m_frame_size))),
        ret};
    exit = RelooperAddBlock(relooper, BinaryenBlock(mod, nullptr, leave, 2, BinaryenTypeNone()));
  } else {
    exit = RelooperAddBlock(relooper, ret);
  }
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
  if (prologue) {
    RelooperAddBranch(prologue, entry, nullptr, nullptr);
    entry = prologue;
  }
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
  BinaryenModuleSetFeatures(
      m_module, BinaryenFeatureMVP() | BinaryenFeatureMutableGlobals() | BinaryenFeatureSignExt() |
                    BinaryenFeatureSIMD128() | BinaryenFeatureBulkMemory() |
                    BinaryenFeatureNontrappingFPToInt() | BinaryenFeatureAtomics());

  BinaryenType params[kGoalArgCount];
  for (auto& p : params) {
    p = BinaryenTypeInt64();
  }
  m_goal_params = BinaryenTypeCreate(params, kGoalArgCount);

  // shared, since the runtime uses pthreads; a shared memory needs a maximum, so the largest
  // (4 GB) and the importer decides the size
  BinaryenSetMemory(m_module, 0, kMaxMemoryPages, nullptr, nullptr, nullptr, nullptr, nullptr,
                    nullptr, 0, true, false, kMemory);
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

const char* WasmModuleGen::segment_global(int seg) {
  static const char* names[N_SEG] = {"seg_main", "seg_debug", "seg_top"};
  ASSERT(seg >= 0 && seg < N_SEG);
  if (!m_segment_globals[seg]) {
    m_segment_globals[seg] = true;
    BinaryenAddGlobalImport(m_module, names[seg], "env", names[seg], BinaryenTypeInt32(), false);
  }
  return names[seg];
}

const char* WasmModuleGen::stack_pointer_global() {
  if (!m_stack_pointer_global) {
    m_stack_pointer_global = true;
    BinaryenAddGlobalImport(m_module, "sp", "env", "sp", BinaryenTypeInt32(), true);
  }
  return "sp";
}

const char* WasmModuleGen::xmm_area_global() {
  if (!m_xmm_area_global) {
    m_xmm_area_global = true;
    BinaryenAddGlobalImport(m_module, "xmm_area", "env", "xmm_area", BinaryenTypeInt32(), false);
  }
  return "xmm_area";
}

const char* WasmModuleGen::process_pointer_global() {
  if (!m_process_pointer_global) {
    m_process_pointer_global = true;
    BinaryenAddGlobalImport(m_module, "pp", "env", "pp", BinaryenTypeInt64(), true);
  }
  return "pp";
}

std::pair<int, int> WasmModuleGen::function_location(int idx_in_file) const {
  return m_function_locations.at(idx_in_file);
}

std::pair<int, int> WasmModuleGen::static_location(const StaticObject* obj) const {
  auto it = m_static_locations.find(obj);
  if (it == m_static_locations.end()) {
    throw std::runtime_error("wasm backend: static object is not in this file: " + obj->print());
  }
  return it->second;
}

WasmObject WasmModuleGen::run(FileEnv* env, const TypeSystem* ts) {
  WasmObject out;
  const auto& functions = env->functions();

  // lay out the object file first, so the code can use the final segment offsets.
  // each function is a type tag and a 4-byte slot holding its index in this file.
  DebugInfo debug_info(env->name());
  emitter::ObjectGenerator obj_gen(m_version, emitter::InstructionSet::WASM);
  std::vector<emitter::FunctionRecord> records;
  for (int i = 0; i < int(functions.size()); i++) {
    // the top-level function is added last and has no idx_in_file
    ASSERT(functions[i]->idx_in_file == i ||
           (functions[i]->idx_in_file == -1 && functions[i].get() == &env->top_level_function()));
    records.push_back(obj_gen.add_function_to_seg(
        functions[i]->segment, &debug_info.add_function(functions[i]->name(), env->name())));
  }
  for (auto& static_obj : env->statics()) {
    static_obj->generate(&obj_gen);
  }
  for (int i = 0; i < int(functions.size()); i++) {
    // two words: a lone zero word is InstructionARM64's null instruction and would emit nothing
    obj_gen.add_instr_no_ir(
        records[i],
        emitter::InstructionARM64(emitter::InstructionARM64(u32(i)), emitter::InstructionARM64(0)),
        InstructionInfo::Kind::IR);
  }
  out.object = obj_gen.generate_data_v3(ts).to_vector();
  for (auto& rec : records) {
    m_function_locations.emplace_back(rec.seg, obj_gen.function_offset_in_seg(rec));
  }
  for (auto& static_obj : env->statics()) {
    m_static_locations[static_obj.get()] = {static_obj->rec.seg,
                                            obj_gen.static_offset_in_seg(static_obj->rec)};
  }

  std::vector<std::string> names;
  for (auto& f : functions) {
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

  // thread suspend/resume (CLAUDE.MD, decision 5): Asyncify lets the runtime unwind a thread's
  // call stack into a buffer and rewind it later. Its state globals are imported, so one unwind
  // goes through the functions of every module. Any call_indirect may suspend, so every function
  // that makes a call is instrumented.
  BinaryenSetPassArgument("asyncify-import-globals", "1");
  const char* passes[] = {"asyncify"};
  BinaryenModuleRunPasses(m_module, passes, 1);
  BinaryenClearPassArguments();
  // the runtime sets the state globals itself
  for (const char* name : {"asyncify_start_unwind", "asyncify_stop_unwind", "asyncify_start_rewind",
                           "asyncify_stop_rewind", "asyncify_get_state"}) {
    BinaryenRemoveExport(m_module, name);
  }
  if (!BinaryenModuleValidate(m_module)) {
    throw std::runtime_error(
        fmt::format("wasm backend: Asyncify made an invalid module for {}", env->name()));
  }

  // keep the name section: GOAL function names in browser profiles and stack traces
  BinaryenSetDebugInfo(true);
  auto result = BinaryenModuleAllocateAndWrite(m_module, nullptr);
  out.wasm.assign((u8*)result.binary, (u8*)result.binary + result.binaryBytes);
  free(result.binary);
  if (result.sourceMap) {
    free(result.sourceMap);
  }
  return out;
}

std::string WasmModuleGen::text() const {
  char* text = BinaryenModuleAllocateAndWriteText(m_module);
  std::string result(text);
  free(text);
  return result;
}

}  // namespace wasm_backend

wasm_backend::WasmObject Compiler::compile_wasm_object(const goos::Object& code,
                                                       const std::string& obj_name,
                                                       std::string* text_out) {
  if (m_instr_set != emitter::InstructionSet::WASM) {
    throw std::runtime_error("compile_wasm_object needs the WASM instruction set");
  }
  auto compiled = compile_object_file(obj_name, code, true);
  color_object_file(compiled);
  wasm_backend::WasmModuleGen gen(m_version);
  auto result = gen.run(compiled, &m_ts);
  if (text_out) {
    *text_out = gen.text();
  }
  compiled->cleanup_after_codegen();
  return result;
}

wasm_backend::WasmObject Compiler::compile_wasm_object_from_string(const std::string& src,
                                                                   const std::string& obj_name,
                                                                   std::string* text_out) {
  return compile_wasm_object(m_goos.reader.read_from_string({src}), obj_name, text_out);
}

wasm_backend::WasmObject Compiler::compile_wasm_object_from_file(const std::string& path,
                                                                 std::string* text_out) {
  return compile_wasm_object(m_goos.reader.read_from_file({path}), "test-code", text_out);
}
