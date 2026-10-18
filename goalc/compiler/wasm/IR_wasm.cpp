/*!
 * @file IR_wasm.cpp
 * IR::do_codegen_wasm for the IR the wasm backend supports (OPENGOAL_WASM_BACKEND only).
 * Each one mirrors the semantics of its do_codegen_x86 in IR.cpp.
 */

#include <array>
#include <vector>

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

bool is_float(const RegVal* rv) {
  return rv->ireg().reg_class == RegClass::FLOAT;
}

bool is_128(const RegVal* rv) {
  return rv->ireg().reg_class == RegClass::VECTOR_FLOAT ||
         rv->ireg().reg_class == RegClass::INT_128;
}

BinaryenExpressionRef v128_const(WasmFunctionGen* gen, const std::array<u8, 16>& bytes) {
  return BinaryenConst(gen->module(), BinaryenLiteralVec128(bytes.data()));
}

BinaryenExpressionRef v128_zero(WasmFunctionGen* gen) {
  return v128_const(gen, {});
}

BinaryenExpressionRef shuffle(WasmFunctionGen* gen,
                              BinaryenExpressionRef a,
                              BinaryenExpressionRef b,
                              const std::array<u8, 16>& mask) {
  return BinaryenSIMDShuffle(gen->module(), a, b, mask.data());
}

//! Shuffle elements of elem_bytes: pick k is element k of a for k < n, else element k-n of b.
BinaryenExpressionRef pick_elements(WasmFunctionGen* gen,
                                    BinaryenExpressionRef a,
                                    BinaryenExpressionRef b,
                                    int elem_bytes,
                                    const std::vector<int>& picks) {
  const int n = 16 / elem_bytes;
  ASSERT(int(picks.size()) == n);
  std::array<u8, 16> mask{};
  for (int k = 0; k < n; k++) {
    int src_byte = picks[k] < n ? picks[k] * elem_bytes : 16 + (picks[k] - n) * elem_bytes;
    for (int j = 0; j < elem_bytes; j++) {
      mask[k * elem_bytes + j] = u8(src_byte + j);
    }
  }
  return shuffle(gen, a, b, mask);
}

//! x86 punpck{l,h}{bw,wd,dq,qdq} a, b: interleave the low or high halves of a and b.
BinaryenExpressionRef unpack(WasmFunctionGen* gen,
                             BinaryenExpressionRef a,
                             BinaryenExpressionRef b,
                             int elem_bytes,
                             bool high) {
  const int n = 16 / elem_bytes;
  std::vector<int> picks(n);
  for (int i = 0; i < n / 2; i++) {
    int e = (high ? n / 2 : 0) + i;
    picks[2 * i] = e;
    picks[2 * i + 1] = n + e;
  }
  return pick_elements(gen, a, b, elem_bytes, picks);
}

[[noreturn]] void unsupported(IR* ir) {
  throw std::runtime_error("wasm backend does not support IR: " + ir->print());
}

BinaryenExpressionRef unary(WasmFunctionGen* gen, BinaryenOp op, BinaryenExpressionRef value) {
  return BinaryenUnary(gen->module(), op, value);
}

void regset_common(WasmFunctionGen* gen, IR* ir, const RegVal* dst, const RegVal* src) {
  auto same_class = dst->ireg().reg_class == src->ireg().reg_class;
  if ((same_class && (is_gpr(dst) || is_float(dst))) || (is_128(dst) && is_128(src))) {
    if (gen->get_reg(dst) != gen->get_reg(src)) {
      gen->emit(gen->set(dst, gen->get(src)));
    }
  } else if (is_128(src) && is_float(dst)) {
    // movss: the low lane
    gen->emit(gen->set(
        dst, BinaryenSIMDExtract(gen->module(), BinaryenExtractLaneVecF32x4(), gen->get(src), 0)));
  } else if (is_float(src) && is_128(dst)) {
    // movss xmm, xmm: replaces the low lane, keeps the others
    gen->emit(gen->set(dst, BinaryenSIMDReplace(gen->module(), BinaryenReplaceLaneVecF32x4(),
                                                gen->get(dst), 0, gen->get(src))));
  } else if (is_gpr(src) && is_128(dst)) {
    // movq xmm, r64: zeroes the upper 64 bits
    gen->emit(gen->set(dst, BinaryenSIMDReplace(gen->module(), BinaryenReplaceLaneVecI64x2(),
                                                v128_zero(gen), 0, gen->get(src))));
  } else if (is_128(src) && is_gpr(dst)) {
    // movq r64, xmm: the low 64 bits
    gen->emit(gen->set(
        dst, BinaryenSIMDExtract(gen->module(), BinaryenExtractLaneVecI64x2(), gen->get(src), 0)));
  } else if (is_float(src) && is_gpr(dst)) {
    // movd, then sign extend like x86
    gen->emit(gen->set(dst, unary(gen, BinaryenExtendSInt32(),
                                  unary(gen, BinaryenReinterpretFloat32(), gen->get(src)))));
  } else if (is_gpr(src) && is_float(dst)) {
    // movd: the low 32 bits
    gen->emit(gen->set(dst, unary(gen, BinaryenReinterpretInt32(), gen->wrap(gen->get(src)))));
  } else {
    unsupported(ir);
  }
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
  if (is_128(m_return_reg)) {
    // 128-bit results go through the register area's xmm0 slot
    gen->emit(gen->store_xmm_area(gen->get_reg(m_return_reg), gen->get(m_return_reg)));
  }
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
  // 128-bit arguments go through the register area (xmm1-8), the callee loads them
  for (auto& reg : m_arg_regs) {
    if (!reg.is_gpr(instr_set)) {
      gen->emit(gen->store_xmm_area(reg, gen->get_reg_value(reg, RegClass::INT_128)));
    }
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
  if (m_ret_reg && !m_ret_reg->is_gpr(instr_set)) {
    // a 128-bit result is in the area's xmm0 slot
    gen->emit(gen->set_reg_value(*m_ret_reg, RegClass::INT_128, gen->load_xmm_area(*m_ret_reg)));
  }
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
  if (condition.is_float) {
    // comiss + jb/jbe/ja/jae on x86. Ordered compares here: they differ from x86 only for NaN.
    if (!is_float(condition.a) || !is_float(condition.b)) {
      unsupported(this);
    }
    BinaryenOp op;
    switch (condition.kind) {
      case ConditionKind::EQUAL:
        op = BinaryenEqFloat32();
        break;
      case ConditionKind::NOT_EQUAL:
        op = BinaryenNeFloat32();
        break;
      case ConditionKind::LEQ:
        op = BinaryenLeFloat32();
        break;
      case ConditionKind::GEQ:
        op = BinaryenGeFloat32();
        break;
      case ConditionKind::LT:
        op = BinaryenLtFloat32();
        break;
      case ConditionKind::GT:
        op = BinaryenGtFloat32();
        break;
      default:
        unsupported(this);
    }
    gen->set_branch_condition(binary(gen, op, gen->get(condition.a), gen->get(condition.b)));
    return;
  }
  if (!is_gpr(condition.a) || !is_gpr(condition.b)) {
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

void IR_FloatMath::do_codegen_wasm(WasmFunctionGen* gen) {
  if (!is_float(m_dest) || !is_float(m_arg)) {
    unsupported(this);
  }
  BinaryenExpressionRef result = nullptr;
  switch (m_kind) {
    case FloatMathKind::ADD_SS:
      result = binary(gen, BinaryenAddFloat32(), gen->get(m_dest), gen->get(m_arg));
      break;
    case FloatMathKind::SUB_SS:
      result = binary(gen, BinaryenSubFloat32(), gen->get(m_dest), gen->get(m_arg));
      break;
    case FloatMathKind::MUL_SS:
      result = binary(gen, BinaryenMulFloat32(), gen->get(m_dest), gen->get(m_arg));
      break;
    case FloatMathKind::DIV_SS:
      result = binary(gen, BinaryenDivFloat32(), gen->get(m_dest), gen->get(m_arg));
      break;
    // maxss/minss return the second operand unless the first is strictly greater/less,
    // which is not what f32.max/f32.min do for NaN and signed zeros
    case FloatMathKind::MAX_SS:
      result = BinaryenSelect(gen->module(),
                              binary(gen, BinaryenGtFloat32(), gen->get(m_dest), gen->get(m_arg)),
                              gen->get(m_dest), gen->get(m_arg));
      break;
    case FloatMathKind::MIN_SS:
      result = BinaryenSelect(gen->module(),
                              binary(gen, BinaryenLtFloat32(), gen->get(m_dest), gen->get(m_arg)),
                              gen->get(m_dest), gen->get(m_arg));
      break;
    case FloatMathKind::SQRT_SS:
      result = unary(gen, BinaryenSqrtFloat32(), gen->get(m_arg));
      break;
    default:
      unsupported(this);
  }
  gen->emit(gen->set(m_dest, result));
}

void IR_StaticVarLoad::do_codegen_wasm(WasmFunctionGen* gen) {
  if (m_dest->ireg().reg_class == RegClass::VECTOR_FLOAT) {
    // the whole 16 bytes, without checking the load info, like x86
    gen->emit(gen->set(m_dest, BinaryenLoad(gen->module(), 16, false, 0, 0, BinaryenTypeVec128(),
                                            gen->ee_addr(gen->static_addr(m_src)), "memory")));
    return;
  }
  if (!is_float(m_dest)) {
    unsupported(this);
  }
  auto load_info = m_src->get_load_info();
  ASSERT(m_src->get_addr_offset() == 0);
  ASSERT(!load_info.load_signed && load_info.load_size == 4 && load_info.requires_load);
  gen->emit(gen->set(m_dest, BinaryenLoad(gen->module(), 4, false, 0, 0, BinaryenTypeFloat32(),
                                          gen->ee_addr(gen->static_addr(m_src)), "memory")));
}

void IR_FloatToInt::do_codegen_wasm(WasmFunctionGen* gen) {
  if (!is_gpr(m_dest) || !is_float(m_src)) {
    unsupported(this);
  }
  // cvttss2si gives 0x80000000 for NaN and out of range values, then x86 sign extends
  auto* mod = gen->module();
  auto in_range = binary(gen, BinaryenAndInt32(),
                         binary(gen, BinaryenGeFloat32(), gen->get(m_src),
                                BinaryenConst(mod, BinaryenLiteralFloat32(-2147483648.0f))),
                         binary(gen, BinaryenLtFloat32(), gen->get(m_src),
                                BinaryenConst(mod, BinaryenLiteralFloat32(2147483648.0f))));
  auto truncated =
      BinaryenSelect(mod, in_range, unary(gen, BinaryenTruncSatSFloat32ToInt32(), gen->get(m_src)),
                     gen->i32(INT32_MIN));
  gen->emit(gen->set(m_dest, unary(gen, BinaryenExtendSInt32(), truncated)));
}

void IR_IntToFloat::do_codegen_wasm(WasmFunctionGen* gen) {
  if (!is_float(m_dest) || !is_gpr(m_src)) {
    unsupported(this);
  }
  // cvtsi2ss from the low 32 bits
  gen->emit(
      gen->set(m_dest, unary(gen, BinaryenConvertSInt32ToFloat32(), gen->wrap(gen->get(m_src)))));
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

void IR_AsmBreak::do_codegen_wasm(WasmFunctionGen* gen) {
  // x86 int3 stops in the debugger; there is no debugger to continue from here
  gen->emit(BinaryenUnreachable(gen->module()));
}

void IR_LoadConstOffset::do_codegen_wasm(WasmFunctionGen* gen) {
  if (!is_gpr(m_base)) {
    unsupported(this);
  }
  if (is_float(m_dest) && m_info.size == 4 && !m_info.sign_extend &&
      m_info.reg == RegClass::FLOAT) {
    gen->emit(gen->set(
        m_dest, BinaryenLoad(gen->module(), 4, false, 0, 0, BinaryenTypeFloat32(),
                             gen->ee_addr(gen->wrap(gen->get(m_base)), m_offset), "memory")));
    return;
  }
  if (is_128(m_dest) && m_info.size == 16 && !m_info.sign_extend &&
      m_info.reg == m_dest->ireg().reg_class) {
    gen->emit(gen->set(
        m_dest, BinaryenLoad(gen->module(), 16, false, 0, 0, BinaryenTypeVec128(),
                             gen->ee_addr(gen->wrap(gen->get(m_base)), m_offset), "memory")));
    return;
  }
  if (!is_gpr(m_dest)) {
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
  if (!is_gpr(m_base)) {
    unsupported(this);
  }
  if (is_float(m_value) && m_size == 4) {
    gen->emit(BinaryenStore(gen->module(), 4, 0, 0,
                            gen->ee_addr(gen->wrap(gen->get(m_base)), m_offset), gen->get(m_value),
                            BinaryenTypeFloat32(), "memory"));
    return;
  }
  if (is_128(m_value) && m_size == 16) {
    gen->emit(BinaryenStore(gen->module(), 16, 0, 0,
                            gen->ee_addr(gen->wrap(gen->get(m_base)), m_offset), gen->get(m_value),
                            BinaryenTypeVec128(), "memory"));
    return;
  }
  if (!is_gpr(m_value)) {
    unsupported(this);
  }
  if (m_size != 1 && m_size != 2 && m_size != 4 && m_size != 8) {
    unsupported(this);
  }
  gen->emit(BinaryenStore(gen->module(), m_size, 0, 0,
                          gen->ee_addr(gen->wrap(gen->get(m_base)), m_offset), gen->get(m_value),
                          BinaryenTypeInt64(), "memory"));
}

///////////////////////
// stack addresses
///////////////////////

void IR_GetStackAddr::do_codegen_wasm(WasmFunctionGen* gen) {
  if (!is_gpr(m_dest)) {
    unsupported(this);
  }
  auto offset = emitter::GPR_SIZE * gen->allocs().get_slot_for_var(m_slot);
  gen->emit(gen->set(m_dest, unary(gen, BinaryenExtendUInt32(), gen->frame_addr(offset))));
}

void IR_RegValAddr::do_codegen_wasm(WasmFunctionGen* gen) {
  if (!is_gpr(m_dest)) {
    unsupported(this);
  }
  // same slot as the x86 get_stack_offset: the variable's spill slot
  ASSERT(m_src->forced_on_stack() && !m_src->rlet_constraint().has_value());
  const auto& range = gen->allocs().ass_as_ranges.at(m_src->ireg().id);
  auto offset = emitter::GPR_SIZE * gen->allocs().get_slot_for_spill(range.stack_slot());
  gen->emit(gen->set(m_dest, unary(gen, BinaryenExtendUInt32(), gen->frame_addr(offset))));
}

///////////////////////
// 128-bit / vector float
///////////////////////

void IR_RegSetAsm::do_codegen_wasm(WasmFunctionGen* gen) {
  regset_common(gen, this, m_dst, m_src);
}

void IR_VFMath3Asm::do_codegen_wasm(WasmFunctionGen* gen) {
  if (!is_128(m_dst) || !is_128(m_src1) || !is_128(m_src2)) {
    unsupported(this);
  }
  auto a = [&]() { return gen->get(m_src1); };
  auto b = [&]() { return gen->get(m_src2); };
  auto* mod = gen->module();
  BinaryenExpressionRef result = nullptr;
  switch (m_kind) {
    case Kind::XOR:
      result = binary(gen, BinaryenXorVec128(), a(), b());
      break;
    case Kind::SUB:
      result = binary(gen, BinaryenSubVecF32x4(), a(), b());
      break;
    case Kind::ADD:
      result = binary(gen, BinaryenAddVecF32x4(), a(), b());
      break;
    case Kind::MUL:
      result = binary(gen, BinaryenMulVecF32x4(), a(), b());
      break;
    case Kind::DIV:
      result = binary(gen, BinaryenDivVecF32x4(), a(), b());
      break;
    // maxps/minps: per lane, src1 if strictly greater/less, else src2
    case Kind::MAX:
      result = BinaryenSIMDTernary(mod, BinaryenBitselectVec128(), a(), b(),
                                   binary(gen, BinaryenGtVecF32x4(), a(), b()));
      break;
    case Kind::MIN:
      result = BinaryenSIMDTernary(mod, BinaryenBitselectVec128(), a(), b(),
                                   binary(gen, BinaryenLtVecF32x4(), a(), b()));
      break;
    default:
      unsupported(this);
  }
  gen->emit(gen->set(m_dst, result));
}

void IR_Int128Math3Asm::do_codegen_wasm(WasmFunctionGen* gen) {
  if (!is_128(m_dst) || !is_128(m_src1) || !is_128(m_src2)) {
    unsupported(this);
  }
  // operands in the order the x86 code passes them to the 3-operand AVX instruction
  auto s1 = [&]() { return gen->get(m_src1); };
  auto s2 = [&]() { return gen->get(m_src2); };
  BinaryenExpressionRef result = nullptr;
  switch (m_kind) {
    case Kind::PEXTUB:
      result = unpack(gen, s2(), s1(), 1, true);
      break;
    case Kind::PEXTUH:
      result = unpack(gen, s2(), s1(), 2, true);
      break;
    case Kind::PEXTUW:
      result = unpack(gen, s2(), s1(), 4, true);
      break;
    case Kind::PEXTLB:
      result = unpack(gen, s2(), s1(), 1, false);
      break;
    case Kind::PEXTLH:
      result = unpack(gen, s2(), s1(), 2, false);
      break;
    case Kind::PEXTLW:
      result = unpack(gen, s2(), s1(), 4, false);
      break;
    case Kind::PCPYLD:  // vpunpcklqdq src2, src1
      result = unpack(gen, s2(), s1(), 8, false);
      break;
    case Kind::PCPYUD:  // vpunpckhqdq src1, src2
      result = unpack(gen, s1(), s2(), 8, true);
      break;
    case Kind::PCEQB:
      result = binary(gen, BinaryenEqVecI8x16(), s2(), s1());
      break;
    case Kind::PCEQH:
      result = binary(gen, BinaryenEqVecI16x8(), s2(), s1());
      break;
    case Kind::PCEQW:
      result = binary(gen, BinaryenEqVecI32x4(), s2(), s1());
      break;
    case Kind::PCGTB:
      result = binary(gen, BinaryenGtSVecI8x16(), s1(), s2());
      break;
    case Kind::PCGTH:
      result = binary(gen, BinaryenGtSVecI16x8(), s1(), s2());
      break;
    case Kind::PCGTW:
      result = binary(gen, BinaryenGtSVecI32x4(), s1(), s2());
      break;
    case Kind::PSUBW:  // psubw on MIPS is psubd on x86
      result = binary(gen, BinaryenSubVecI32x4(), s1(), s2());
      break;
    case Kind::POR:
      result = binary(gen, BinaryenOrVec128(), s2(), s1());
      break;
    case Kind::PXOR:
      result = binary(gen, BinaryenXorVec128(), s2(), s1());
      break;
    case Kind::PAND:
      result = binary(gen, BinaryenAndVec128(), s2(), s1());
      break;
    case Kind::PACKUSWB:  // signed words to unsigned saturated bytes, src1 then src2
      result = binary(gen, BinaryenNarrowUVecI16x8ToVecI8x16(), s1(), s2());
      break;
    case Kind::PADDB:
      result = binary(gen, BinaryenAddVecI8x16(), s1(), s2());
      break;
    default:
      unsupported(this);
  }
  gen->emit(gen->set(m_dst, result));
}

void IR_Int128Math2Asm::do_codegen_wasm(WasmFunctionGen* gen) {
  if (!is_128(m_dst) || !is_128(m_src) || !m_imm.has_value()) {
    unsupported(this);
  }
  const int imm = int(*m_imm);
  ASSERT(imm >= 0 && imm <= 255);
  auto* mod = gen->module();
  auto src = [&]() { return gen->get(m_src); };
  // x86 shifts by at least the lane width give zero (or all sign bits); wasm would wrap the count
  auto shift = [&](BinaryenOp op, int lane_bits, bool arithmetic) {
    if (imm >= lane_bits && !arithmetic) {
      return v128_zero(gen);
    }
    int count = imm >= lane_bits ? lane_bits - 1 : imm;
    return BinaryenSIMDShift(mod, op, src(), gen->i32(count));
  };
  BinaryenExpressionRef result = nullptr;
  switch (m_kind) {
    case Kind::PW_SLL:
      result = shift(BinaryenShlVecI32x4(), 32, false);
      break;
    case Kind::PW_SRL:
      result = shift(BinaryenShrUVecI32x4(), 32, false);
      break;
    case Kind::PW_SRA:
      result = shift(BinaryenShrSVecI32x4(), 32, true);
      break;
    case Kind::PH_SLL:
      result = shift(BinaryenShlVecI16x8(), 16, false);
      break;
    case Kind::PH_SRL:
      result = shift(BinaryenShrUVecI16x8(), 16, false);
      break;
    case Kind::VPSRLDQ:
    case Kind::VPSLLDQ: {
      // whole-register byte shift, zeros shifted in (byte 16 of the shuffle is the zero vector)
      std::array<u8, 16> mask{};
      for (int i = 0; i < 16; i++) {
        int from = m_kind == Kind::VPSRLDQ ? i + imm : i - imm;
        mask[i] = u8(from >= 0 && from < 16 ? from : 16);
      }
      result = shuffle(gen, src(), v128_zero(gen), mask);
    } break;
    case Kind::VPSHUFLW:
    case Kind::VPSHUFHW: {
      // shuffle the low (or high) four 16-bit words by the immediate, copy the other four
      int base = m_kind == Kind::VPSHUFLW ? 0 : 4;
      std::vector<int> picks(8);
      for (int k = 0; k < 8; k++) {
        picks[k] = k;
      }
      for (int k = 0; k < 4; k++) {
        picks[base + k] = base + ((imm >> (2 * k)) & 3);
      }
      result = pick_elements(gen, src(), src(), 2, picks);
    } break;
    default:
      unsupported(this);
  }
  gen->emit(gen->set(m_dst, result));
}

void IR_VFMath2Asm::do_codegen_wasm(WasmFunctionGen* gen) {
  if (!is_128(m_dst) || !is_128(m_src)) {
    unsupported(this);
  }
  auto* mod = gen->module();
  BinaryenExpressionRef result = nullptr;
  switch (m_kind) {
    case Kind::ITOF:
      result = unary(gen, BinaryenConvertSVecI32x4ToVecF32x4(), gen->get(m_src));
      break;
    case Kind::FTOI: {
      // cvttps2dq: 0x80000000 in lanes that are NaN or out of range
      auto splat_f = [&](float v) {
        return BinaryenUnary(mod, BinaryenSplatVecF32x4(),
                             BinaryenConst(mod, BinaryenLiteralFloat32(v)));
      };
      auto in_range =
          binary(gen, BinaryenAndVec128(),
                 binary(gen, BinaryenGeVecF32x4(), gen->get(m_src), splat_f(-2147483648.0f)),
                 binary(gen, BinaryenLtVecF32x4(), gen->get(m_src), splat_f(2147483648.0f)));
      result = BinaryenSIMDTernary(
          mod, BinaryenBitselectVec128(),
          unary(gen, BinaryenTruncSatSVecF32x4ToVecI32x4(), gen->get(m_src)),
          BinaryenUnary(mod, BinaryenSplatVecI32x4(), gen->i32(INT32_MIN)), in_range);
    } break;
    default:
      unsupported(this);
  }
  gen->emit(gen->set(m_dst, result));
}

void IR_BlendVF::do_codegen_wasm(WasmFunctionGen* gen) {
  if (!is_128(m_dst) || !is_128(m_src1) || !is_128(m_src2)) {
    unsupported(this);
  }
  // vblendps: lane i comes from src2 if mask bit i is set, else from src1
  std::vector<int> picks(4);
  for (int i = 0; i < 4; i++) {
    picks[i] = (m_mask >> i) & 1 ? 4 + i : i;
  }
  gen->emit(gen->set(m_dst, pick_elements(gen, gen->get(m_src1), gen->get(m_src2), 4, picks)));
}

namespace {
//! vshufps dst, src, src, control: lane k is src lane (control >> 2k) & 3
BinaryenExpressionRef swizzle(WasmFunctionGen* gen, const RegVal* src, int control) {
  std::vector<int> picks(4);
  for (int k = 0; k < 4; k++) {
    picks[k] = (control >> (2 * k)) & 3;
  }
  return pick_elements(gen, gen->get(src), gen->get(src), 4, picks);
}
}  // namespace

void IR_SplatVF::do_codegen_wasm(WasmFunctionGen* gen) {
  if (!is_128(m_dst) || !is_128(m_src)) {
    unsupported(this);
  }
  int lane;
  switch (m_element) {
    case emitter::Register::VF_ELEMENT::X:
      lane = 0;
      break;
    case emitter::Register::VF_ELEMENT::Y:
      lane = 1;
      break;
    case emitter::Register::VF_ELEMENT::Z:
      lane = 2;
      break;
    case emitter::Register::VF_ELEMENT::W:
      lane = 3;
      break;
    default:
      unsupported(this);
  }
  gen->emit(gen->set(m_dst, swizzle(gen, m_src, lane * 0b01010101)));
}

void IR_SwizzleVF::do_codegen_wasm(WasmFunctionGen* gen) {
  if (!is_128(m_dst) || !is_128(m_src)) {
    unsupported(this);
  }
  gen->emit(gen->set(m_dst, swizzle(gen, m_src, m_controlBytes)));
}

void IR_SqrtVF::do_codegen_wasm(WasmFunctionGen* gen) {
  if (!is_128(m_dst) || !is_128(m_src)) {
    unsupported(this);
  }
  gen->emit(gen->set(m_dst, unary(gen, BinaryenSqrtVecF32x4(), gen->get(m_src))));
}
