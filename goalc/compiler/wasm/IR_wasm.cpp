/*!
 * @file IR_wasm.cpp
 * IR::do_codegen_wasm for the IR the wasm backend supports (OPENGOAL_WASM_BACKEND only).
 * Each one mirrors the semantics of its do_codegen_x86 in IR.cpp.
 */

#include "common/symbols.h"

#include "goalc/compiler/Env.h"
#include "goalc/compiler/IR.h"
#include "goalc/compiler/StaticObject.h"
#include "goalc/compiler/Val.h"
#include "goalc/compiler/wasm/WasmCodeGenerator.h"

#include "fmt/format.h"

using namespace wasm_backend;

namespace {

bool is_gpr(const RegVal* rv) {
  return rv->ireg().reg_class == RegClass::GPR_64;
}

[[noreturn]] void unsupported(IR* ir) {
  throw std::runtime_error("wasm backend does not support IR: " + ir->print());
}

void regset_common(WasmFunctionGen* gen, IR* ir, const RegVal* dst, const RegVal* src) {
  if (is_gpr(dst) && is_gpr(src)) {
    if (gen->get_reg(dst) != gen->get_reg(src)) {
      gen->emit(gen->set(dst, gen->get(src)));
    }
  } else {
    unsupported(ir);
  }
}

BinaryenExpressionRef unary(WasmFunctionGen* gen, BinaryenOp op, BinaryenExpressionRef value) {
  return BinaryenUnary(gen->module(), op, value);
}

BinaryenExpressionRef binary(WasmFunctionGen* gen,
                             BinaryenOp op,
                             BinaryenExpressionRef a,
                             BinaryenExpressionRef b) {
  return BinaryenBinary(gen->module(), op, a, b);
}

// 32-bit op on the low halves, result sign extended to 64 bits like the x86 movsx
BinaryenExpressionRef op32_sext(WasmFunctionGen* gen,
                                BinaryenOp op,
                                BinaryenExpressionRef a,
                                BinaryenExpressionRef b) {
  return unary(gen, BinaryenExtendSInt32(), binary(gen, op, gen->wrap(a), gen->wrap(b)));
}

}  // namespace

void IR_Return::do_codegen_wasm(WasmFunctionGen* gen) {
  regset_common(gen, this, m_return_reg, m_value);
}

void IR_LoadConstant64::do_codegen_wasm(WasmFunctionGen* gen) {
  if (!is_gpr(m_dest)) {
    unsupported(this);
  }
  gen->emit(gen->set(m_dest, gen->i64(s64(m_value))));
}

void IR_LoadSymbolPointer::do_codegen_wasm(WasmFunctionGen* gen) {
  if (!is_gpr(m_dest)) {
    unsupported(this);
  }
  BinaryenExpressionRef addr;
  if (m_name == "#f") {
    static_assert(false_symbol_offset() == 0, "false symbol location");
    addr = gen->st();
  } else if (m_name == "#t") {
    addr = binary(gen, BinaryenAddInt32(), gen->st(), gen->i32(true_symbol_offset(gen->version())));
  } else if (m_name == "_empty_") {
    addr = binary(gen, BinaryenAddInt32(), gen->st(),
                  gen->i32(empty_pair_offset_from_s7(gen->version())));
  } else {
    addr = gen->symbol_addr(m_name);
  }
  gen->emit(gen->set(m_dest, unary(gen, BinaryenExtendUInt32(), addr)));
}

void IR_SetSymbolValue::do_codegen_wasm(WasmFunctionGen* gen) {
  if (!is_gpr(m_src)) {
    unsupported(this);
  }
  gen->emit(BinaryenStore(gen->module(), 4, 0, 0, gen->ee_addr(gen->symbol_addr(m_dest->name())),
                          gen->wrap(gen->get(m_src)), BinaryenTypeInt32(), "memory"));
}

void IR_GetSymbolValue::do_codegen_wasm(WasmFunctionGen* gen) {
  if (!is_gpr(m_dest)) {
    unsupported(this);
  }
  gen->emit(
      gen->set(m_dest, BinaryenLoad(gen->module(), 4, m_sext, 0, 0, BinaryenTypeInt64(),
                                    gen->ee_addr(gen->symbol_addr(m_src->name())), "memory")));
}

void IR_RegSet::do_codegen_wasm(WasmFunctionGen* gen) {
  regset_common(gen, this, m_dest, m_src);
}

void IR_StaticVarAddr::do_codegen_wasm(WasmFunctionGen* gen) {
  if (!is_gpr(m_dest)) {
    unsupported(this);
  }
  gen->emit(gen->set(m_dest, unary(gen, BinaryenExtendUInt32(), gen->static_addr(m_src))));
}

void IR_FunctionAddr::do_codegen_wasm(WasmFunctionGen* gen) {
  if (!is_gpr(m_dest)) {
    unsupported(this);
  }
  gen->emit(
      gen->set(m_dest, unary(gen, BinaryenExtendUInt32(), gen->function_addr(m_src->idx_in_file))));
}

void IR_FunctionCall::do_codegen_wasm(WasmFunctionGen* gen) {
  const auto instr_set = emitter::InstructionSet::WASM;
  const auto& info = emitter::reg_info(instr_set);
  for (auto& reg : m_arg_regs) {
    if (!reg.is_gpr(instr_set)) {
      unsupported(this);  // 128-bit arguments
    }
  }
  if (m_ret_reg && !m_ret_reg->is_gpr(instr_set)) {
    unsupported(this);  // 128-bit return
  }

  // every GOAL function takes all eight GPR argument registers
  BinaryenExpressionRef args[kGoalArgCount];
  for (int i = 0; i < kGoalArgCount; i++) {
    args[i] = gen->get_reg_value(info.get_gpr_arg_reg(i), RegClass::GPR_64);
  }
  // a function value is the EE address of its table index
  auto table_index = BinaryenLoad(gen->module(), 4, false, 0, 0, BinaryenTypeInt32(),
                                  gen->ee_addr(gen->wrap(gen->get(m_func))), "memory");
  auto call = BinaryenCallIndirect(gen->module(), "table", table_index, args, kGoalArgCount,
                                   gen->goal_params(), BinaryenTypeInt64());
  // the callee returns in rax, which a call clobbers either way
  gen->emit(gen->set_reg_value(info.get_gpr_ret_reg(), RegClass::GPR_64, call));
}

void IR_IntegerMath::do_codegen_wasm(WasmFunctionGen* gen) {
  if (!is_gpr(m_dest) || (m_arg && !is_gpr(m_arg))) {
    unsupported(this);
  }
  auto d = gen->get(m_dest);
  auto a = [&]() { return gen->get(m_arg); };
  BinaryenExpressionRef result = nullptr;
  switch (m_kind) {
    case IntegerMathKind::ADD_64:
      result = binary(gen, BinaryenAddInt64(), d, a());
      break;
    case IntegerMathKind::SUB_64:
      result = binary(gen, BinaryenSubInt64(), d, a());
      break;
    case IntegerMathKind::AND_64:
      result = binary(gen, BinaryenAndInt64(), d, a());
      break;
    case IntegerMathKind::OR_64:
      result = binary(gen, BinaryenOrInt64(), d, a());
      break;
    case IntegerMathKind::XOR_64:
      result = binary(gen, BinaryenXorInt64(), d, a());
      break;
    case IntegerMathKind::NOT_64:
      result = binary(gen, BinaryenXorInt64(), d, gen->i64(-1));
      break;
    // wasm masks shift counts to 6 bits, like x86 does with cl
    case IntegerMathKind::SHLV_64:
      result = binary(gen, BinaryenShlInt64(), d, a());
      break;
    case IntegerMathKind::SHRV_64:
      result = binary(gen, BinaryenShrUInt64(), d, a());
      break;
    case IntegerMathKind::SARV_64:
      result = binary(gen, BinaryenShrSInt64(), d, a());
      break;
    case IntegerMathKind::SHL_64:
      result = binary(gen, BinaryenShlInt64(), d, gen->i64(m_shift_amount));
      break;
    case IntegerMathKind::SHR_64:
      result = binary(gen, BinaryenShrUInt64(), d, gen->i64(m_shift_amount));
      break;
    case IntegerMathKind::SAR_64:
      result = binary(gen, BinaryenShrSInt64(), d, gen->i64(m_shift_amount));
      break;
    case IntegerMathKind::IMUL_32:
      // the PS2 sign extends the result even for multu
      result = op32_sext(gen, BinaryenMulInt32(), d, a());
      break;
    case IntegerMathKind::IMUL_64:
      result = binary(gen, BinaryenMulInt64(), d, a());
      break;
    // x86 sign extends the 32-bit quotient and remainder for both signed and unsigned
    case IntegerMathKind::IDIV_32:
      result = op32_sext(gen, BinaryenDivSInt32(), d, a());
      break;
    case IntegerMathKind::UDIV_32:
      result = op32_sext(gen, BinaryenDivUInt32(), d, a());
      break;
    case IntegerMathKind::IMOD_32:
      result = op32_sext(gen, BinaryenRemSInt32(), d, a());
      break;
    case IntegerMathKind::UMOD_32:
      result = op32_sext(gen, BinaryenRemUInt32(), d, a());
      break;
    default:
      unsupported(this);
  }
  gen->emit(gen->set(m_dest, result));
}

void IR_GotoLabel::do_codegen_wasm(WasmFunctionGen* gen) {
  // the edge comes from to_rai() when the function's control flow is built
  (void)gen;
}

void IR_ConditionalBranch::do_codegen_wasm(WasmFunctionGen* gen) {
  if (condition.is_float || !is_gpr(condition.a) || !is_gpr(condition.b)) {
    unsupported(this);
  }
  const bool s = condition.is_signed;
  BinaryenOp op;
  switch (condition.kind) {
    case ConditionKind::EQUAL:
      op = BinaryenEqInt64();
      break;
    case ConditionKind::NOT_EQUAL:
      op = BinaryenNeInt64();
      break;
    case ConditionKind::LEQ:
      op = s ? BinaryenLeSInt64() : BinaryenLeUInt64();
      break;
    case ConditionKind::GEQ:
      op = s ? BinaryenGeSInt64() : BinaryenGeUInt64();
      break;
    case ConditionKind::LT:
      op = s ? BinaryenLtSInt64() : BinaryenLtUInt64();
      break;
    case ConditionKind::GT:
      op = s ? BinaryenGtSInt64() : BinaryenGtUInt64();
      break;
    default:
      unsupported(this);
  }
  gen->set_branch_condition(binary(gen, op, gen->get(condition.a), gen->get(condition.b)));
}

void IR_Null::do_codegen_wasm(WasmFunctionGen* gen) {
  (void)gen;
}

void IR_ValueReset::do_codegen_wasm(WasmFunctionGen* gen) {
  (void)gen;
}

void IR_Nop::do_codegen_wasm(WasmFunctionGen* gen) {
  (void)gen;
}

void IR_LoadConstOffset::do_codegen_wasm(WasmFunctionGen* gen) {
  if (!is_gpr(m_dest) || !is_gpr(m_base)) {
    unsupported(this);
  }
  int size = m_info.size;
  if (size != 1 && size != 2 && size != 4 && size != 8) {
    unsupported(this);
  }
  gen->emit(gen->set(
      m_dest,
      BinaryenLoad(gen->module(), size, size < 8 && m_info.sign_extend, 0, 0, BinaryenTypeInt64(),
                   gen->ee_addr(gen->wrap(gen->get(m_base)), m_offset), "memory")));
}

void IR_StoreConstOffset::do_codegen_wasm(WasmFunctionGen* gen) {
  if (!is_gpr(m_value) || !is_gpr(m_base)) {
    unsupported(this);
  }
  if (m_size != 1 && m_size != 2 && m_size != 4 && m_size != 8) {
    unsupported(this);
  }
  gen->emit(BinaryenStore(gen->module(), m_size, 0, 0,
                          gen->ee_addr(gen->wrap(gen->get(m_base)), m_offset), gen->get(m_value),
                          BinaryenTypeInt64(), "memory"));
}
