// Xenos microcode -> HLSL SM5. See shader_translator.h for the binding contract.
//
// Instruction encodings follow rex/graphics/format/ucode.h (Xenia, BSD). Semantics:
// - ALU vector and scalar operations co-issue: both read sources before writing.
// - Multiplications follow the Direct3D 9 rule 0 * anything = 0 (games rely on it).
// - Control flow: straight-line code when the shader has no jumps, calls or loops;
//   otherwise a `pc` state machine over the control flow instructions.

#include "shader_translator.h"

#include <algorithm>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <map>
#include <set>

#include <rex/graphics/format/ucode.h>

namespace copa {

namespace ucode = rex::graphics::ucode;
namespace xenos = rex::graphics::xenos;
using ucode::AluScalarOpcode;
using ucode::AluVectorOpcode;
using ucode::ControlFlowOpcode;

const char* ShaderCommonHlsl() {
  return R"HLSL(
cbuffer VsConstants : register(b0) { float4 vs_c[256]; };
cbuffer PsConstants : register(b1) { float4 ps_c[256]; };
cbuffer SystemConstants : register(b2) {
  uint4 sys_bools[2];
  uint4 sys_loops[8];
  uint4 sys_vfetch_endian[24];
  uint4 sys_tex_swizzle[8];
  uint4 sys_tex_flags[8];
  float4 sys_tex_size[32];
  float4 sys_viewport;
  float4 sys_viewport_z;
  uint sys_vte_flags;
  float sys_pixel_offset;
  float sys_window_offset_x;
  float sys_window_offset_y;
  uint sys_alpha_test;
  float sys_alpha_ref;
  float2 sys_res_scale;
  float4 sys_rt_exp_scale;
};

uint SysU32(uint4 v[2], uint i) { return v[i >> 2][i & 3]; }
bool BoolConst(uint i) { return (sys_bools[i >> 7][(i >> 5) & 3] >> (i & 31)) & 1; }
uint LoopConst(uint i) { return sys_loops[i >> 2][i & 3]; }
uint VfetchEndian(uint i) { return sys_vfetch_endian[i >> 2][i & 3]; }
uint TexSwizzle(uint i) { return sys_tex_swizzle[i >> 2][i & 3]; }
uint TexFlags(uint i) { return sys_tex_flags[i >> 2][i & 3]; }

uint GpuSwap(uint v, uint mode) {
  if (mode == 1) return ((v & 0x00FF00FFu) << 8) | ((v & 0xFF00FF00u) >> 8);
  if (mode == 2) return (v << 24) | ((v & 0xFF00u) << 8) | ((v >> 8) & 0xFF00u) | (v >> 24);
  if (mode == 3) return (v << 16) | (v >> 16);
  return v;
}

// Direct3D 9 multiplication: +-0 * anything (including Inf/NaN) = +0.
float4 MulZ(float4 a, float4 b) { return (a == 0.0 || b == 0.0) ? 0.0 : a * b; }
float MulZ1(float a, float b) { return (a == 0.0 || b == 0.0) ? 0.0 : a * b; }
float Dp4Z(float4 a, float4 b) { float4 m = MulZ(a, b); return (m.x + m.y) + (m.z + m.w); }
float Dp3Z(float4 a, float4 b) { float4 m = MulZ(a, b); return (m.x + m.y) + m.z; }
float ClampMax(float v) { return isnan(v) ? v : clamp(v, -3.402823466e+38, 3.402823466e+38); }

float4 CubeVector(float4 s0, float4 s1) {
  // s0 = src.zzxy, s1 = src.yxzz -> x = s1.y, y = s1.x, z = s0.x
  float3 v = float3(s1.y, s1.x, s0.x);
  float3 a = abs(v);
  float tc, sc, ma, id;
  if (a.z >= a.x && a.z >= a.y) {
    tc = -v.y; sc = v.z < 0.0 ? -v.x : v.x; ma = 2.0 * v.z; id = v.z < 0.0 ? 5.0 : 4.0;
  } else if (a.y >= a.x) {
    tc = v.y < 0.0 ? -v.z : v.z; sc = v.x; ma = 2.0 * v.y; id = v.y < 0.0 ? 3.0 : 2.0;
  } else {
    tc = -v.y; sc = v.x < 0.0 ? v.z : -v.z; ma = 2.0 * v.x; id = v.x < 0.0 ? 1.0 : 0.0;
  }
  return float4(tc, sc, ma, id);
}

// Rebuild a cube direction from (sc, tc, face) as produced by `cube` + mad 1.5.
float3 CubeDirection(float3 st_face) {
  float sc = st_face.x - 1.5, tc = st_face.y - 1.5;
  uint face = (uint)clamp(st_face.z + 0.5, 0.0, 5.0);
  float s = sc * 2.0, t = tc * 2.0;
  if (face == 0) return float3(1.0, -t, -s);
  if (face == 1) return float3(-1.0, -t, s);
  if (face == 2) return float3(s, 1.0, t);
  if (face == 3) return float3(s, -1.0, -t);
  if (face == 4) return float3(s, -t, 1.0);
  return float3(-s, -t, -1.0);
}

float4 ApplyTexSwizzle(float4 v, uint slot) {
  uint sw = TexSwizzle(slot);
  float c[6] = { v.x, v.y, v.z, v.w, 0.0, 1.0 };
  float4 r;
  r.x = c[min(sw & 7, 5)];
  r.y = c[min((sw >> 3) & 7, 5)];
  r.z = c[min((sw >> 6) & 7, 5)];
  r.w = c[min((sw >> 9) & 7, 5)];
  uint flags = TexFlags(slot);
  if (flags & 1) r = r * 2.0 - 1.0;
  int exp_adjust = (int)(flags << 16) >> 24;
  if (exp_adjust != 0) r *= exp2((float)exp_adjust);
  return r;
}

float UNorm(uint v, uint bits) { return (float)v / (float)((1u << bits) - 1u); }
float SNorm(uint v, uint bits) {
  int s = (int)(v << (32 - bits)) >> (32 - bits);
  return max((float)s / (float)((1u << (bits - 1)) - 1u), -1.0);
}
float SInt(uint v, uint bits) { return (float)((int)(v << (32 - bits)) >> (32 - bits)); }
float UnpackField(uint word, uint offset, uint bits, bool is_signed, bool normalized) {
  uint v = (word >> offset) & ((bits == 32) ? 0xFFFFFFFFu : ((1u << bits) - 1u));
  if (normalized) return is_signed ? SNorm(v, bits) : UNorm(v, bits);
  return is_signed ? SInt(v, bits) : (float)v;
}
)HLSL";
}

namespace {

constexpr const char* kComp = "xyzw";

struct Writer {
  std::string out;
  int indent = 1;
  void line(const std::string& s) {
    out.append(size_t(indent) * 2, ' ');
    out += s;
    out += '\n';
  }
};

std::string Fmt(const char* f, ...) {
  char buf[1024];
  va_list args;
  va_start(args, f);
  vsnprintf(buf, sizeof(buf), f, args);
  va_end(args);
  return buf;
}

// Explicitly zero-initialized float4 array: FXC cannot always prove loop initialization
// through the control-flow state machine ("used without having been completely
// initialized").
std::string ZeroArray(const char* name, uint32_t count) {
  std::string s = Fmt("  float4 %s[%u] = { ", name, count);
  for (uint32_t i = 0; i < count; ++i) s += i ? ", (float4)0" : "(float4)0";
  return s + " };\n";
}

std::string FloatLiteral(float f) {
  uint32_t bits;
  std::memcpy(&bits, &f, 4);
  return Fmt("asfloat(0x%08Xu)", bits);
}

class Translator {
 public:
  Translator(ShaderStage stage, const uint32_t* d, uint32_t n, const ShaderKey& key)
      : stage_(stage), d_(d), n_(n), key_(key) {
    result_.stage = stage;
  }

  TranslatedShader Run() {
    if (!ParseControlFlow()) return result_;
    bool structured = true;
    for (const auto& cf : cfs_) {
      auto op = cf.opcode();
      if (op == ControlFlowOpcode::kLoopStart || op == ControlFlowOpcode::kLoopEnd ||
          op == ControlFlowOpcode::kCondCall || op == ControlFlowOpcode::kReturn ||
          op == ControlFlowOpcode::kCondJmp)
        structured = false;
    }
    Writer body;
    if (structured)
      EmitStructured(body);
    else
      EmitStateMachine(body);
    if (!result_.error.empty()) return result_;
    Assemble(body.out);
    result_.ok = true;
    return result_;
  }

 private:
  // ---------------------------------------------------------------- control flow
  bool ParseControlFlow() {
    uint32_t limit = n_ / 3;  // in 3-dword units
    for (uint32_t i = 0; i < limit && i * 3 + 2 < n_; ++i) {
      ucode::ControlFlowInstruction pair[2];
      ucode::UnpackControlFlowInstructions(d_ + i * 3, pair);
      for (auto& cf : pair) {
        cfs_.push_back(cf);
        if (ucode::IsControlFlowOpcodeExec(cf.opcode())) {
          limit = std::min<uint32_t>(limit, cf.exec.address());
        }
      }
    }
    // Trim trailing instructions after the last reachable end (keeps label numbering).
    if (cfs_.empty()) {
      result_.error = "empty control flow";
      return false;
    }
    return true;
  }

  std::string CfCondition(const ucode::ControlFlowInstruction& cf) {
    switch (cf.opcode()) {
      case ControlFlowOpcode::kCondExec:
      case ControlFlowOpcode::kCondExecEnd:
      case ControlFlowOpcode::kCondExecPredClean:
      case ControlFlowOpcode::kCondExecPredCleanEnd:
        return Fmt("BoolConst(%uu) == %s", cf.cond_exec.bool_address(),
                   cf.cond_exec.condition() ? "true" : "false");
      case ControlFlowOpcode::kCondExecPred:
      case ControlFlowOpcode::kCondExecPredEnd:
        return Fmt("p0 == %s", cf.cond_exec_pred.condition() ? "true" : "false");
      default:
        return "";
    }
  }

  void EmitStructured(Writer& w) {
    bool can_end = false;
    for (size_t i = 0; i < cfs_.size(); ++i) {
      const auto& cf = cfs_[i];
      auto op = cf.opcode();
      if (op == ControlFlowOpcode::kAlloc || op == ControlFlowOpcode::kNop ||
          op == ControlFlowOpcode::kMarkVsFetchDone)
        continue;
      if (!ucode::IsControlFlowOpcodeExec(op)) continue;
      std::string cond = CfCondition(cf);
      bool ends = ucode::DoesControlFlowOpcodeEndShader(op);
      if (can_end) {
        w.line("if (!done) {");
        ++w.indent;
      }
      if (!cond.empty()) {
        w.line("if (" + cond + ") {");
        ++w.indent;
      }
      EmitExec(w, cf);
      if (ends) w.line("done = true;");
      if (!cond.empty()) {
        --w.indent;
        w.line("}");
      }
      if (can_end) {
        --w.indent;
        w.line("}");
      }
      if (ends) {
        if (cond.empty()) break;  // unconditional end
        can_end = true;
      }
    }
  }

  void EmitStateMachine(Writer& w) {
    w.line("uint pc = 0u;");
    w.line("int loop_depth = -1;");
    w.line("uint loop_iter[4] = { 0u, 0u, 0u, 0u };");
    w.line("int loop_aL[4] = { 0, 0, 0, 0 };");
    w.line("uint call_stack[4] = { 0u, 0u, 0u, 0u };");
    w.line("int call_depth = -1;");
    w.line("uint guard = 0u;");
    w.line("[loop] while (!done && guard++ < 65536u) {");
    ++w.indent;
    w.line("[branch] switch (pc) {");  // plain switch hits an FXC internal error (l-value expected)
    for (size_t i = 0; i < cfs_.size(); ++i) {
      const auto& cf = cfs_[i];
      auto op = cf.opcode();
      w.line(Fmt("case %uu: {", uint32_t(i)));
      ++w.indent;
      std::string next = Fmt("pc = %uu;", uint32_t(i + 1));
      switch (op) {
        case ControlFlowOpcode::kExec:
        case ControlFlowOpcode::kExecEnd:
        case ControlFlowOpcode::kCondExec:
        case ControlFlowOpcode::kCondExecEnd:
        case ControlFlowOpcode::kCondExecPred:
        case ControlFlowOpcode::kCondExecPredEnd:
        case ControlFlowOpcode::kCondExecPredClean:
        case ControlFlowOpcode::kCondExecPredCleanEnd: {
          std::string cond = CfCondition(cf);
          bool ends = ucode::DoesControlFlowOpcodeEndShader(op);
          if (!cond.empty()) {
            w.line("if (" + cond + ") {");
            ++w.indent;
          }
          EmitExec(w, cf);
          if (ends) w.line("done = true;");
          if (!cond.empty()) {
            --w.indent;
            w.line("}");
          }
          w.line(next);
          break;
        }
        case ControlFlowOpcode::kLoopStart: {
          uint32_t id = cf.loop_start.loop_id();
          w.line("loop_depth = min(loop_depth + 1, 3);");
          w.line("loop_iter[loop_depth] = 0u;");
          if (cf.loop_start.is_repeat())
            w.line("loop_aL[loop_depth] = loop_depth > 0 ? loop_aL[loop_depth - 1] : 0;");
          else
            w.line(Fmt("loop_aL[loop_depth] = (int)((LoopConst(%uu) >> 8) & 0xFFu);", id));
          w.line("aL = loop_aL[loop_depth];");
          w.line(Fmt("if ((LoopConst(%uu) & 0xFFu) == 0u) { loop_depth--; aL = loop_depth >= 0 ? "
                     "loop_aL[max(loop_depth, 0)] : 0; pc = %uu; } else { %s }",
                     id, cf.loop_start.address(), next.c_str()));
          break;
        }
        case ControlFlowOpcode::kLoopEnd: {
          uint32_t id = cf.loop_end.loop_id();
          w.line("{");
          ++w.indent;
          w.line("uint li = loop_iter[max(loop_depth, 0)] + 1u;");
          w.line("loop_iter[max(loop_depth, 0)] = li;");
          w.line(Fmt("int lstep = (int)(LoopConst(%uu) << 8) >> 24;", id));
          w.line("loop_aL[max(loop_depth, 0)] += lstep;");
          w.line("aL = loop_aL[max(loop_depth, 0)];");
          std::string brk = cf.loop_end.is_predicated_break()
                                ? Fmt(" && p0 != %s", cf.loop_end.condition() ? "true" : "false")
                                : "";
          w.line(Fmt("if (li < (LoopConst(%uu) & 0xFFu)%s) { pc = %uu; } else { loop_depth--; "
                     "aL = loop_depth >= 0 ? loop_aL[max(loop_depth, 0)] : 0; %s }",
                     id, brk.c_str(), cf.loop_end.address(), next.c_str()));
          --w.indent;
          w.line("}");
          break;
        }
        case ControlFlowOpcode::kCondCall: {
          std::string cond = "true";
          if (!cf.cond_call.is_unconditional()) {
            cond = cf.cond_call.is_predicated()
                       ? Fmt("p0 == %s", cf.cond_call.condition() ? "true" : "false")
                       : Fmt("BoolConst(%uu) == %s", cf.cond_call.bool_address(),
                             cf.cond_call.condition() ? "true" : "false");
          }
          w.line(Fmt("if (%s && call_depth < 3) { call_depth++; call_stack[call_depth] = %uu; "
                     "pc = %uu; } else { %s }",
                     cond.c_str(), uint32_t(i + 1), cf.cond_call.address(), next.c_str()));
          break;
        }
        case ControlFlowOpcode::kReturn:
          w.line(Fmt("if (call_depth >= 0) { pc = call_stack[call_depth]; call_depth--; } else { "
                     "%s }",
                     next.c_str()));
          break;
        case ControlFlowOpcode::kCondJmp: {
          std::string cond = "true";
          if (!cf.cond_jmp.is_unconditional()) {
            cond = cf.cond_jmp.is_predicated()
                       ? Fmt("p0 == %s", cf.cond_jmp.condition() ? "true" : "false")
                       : Fmt("BoolConst(%uu) == %s", cf.cond_jmp.bool_address(),
                             cf.cond_jmp.condition() ? "true" : "false");
          }
          w.line(Fmt("if (%s) { pc = %uu; } else { %s }", cond.c_str(), cf.cond_jmp.address(),
                     next.c_str()));
          break;
        }
        default:
          w.line(next);
          break;
      }
      w.line("break;");
      --w.indent;
      w.line("}");
    }
    w.line("default: done = true; break;");
    w.line("}");
    --w.indent;
    w.line("}");
  }

  void EmitExec(Writer& w, const ucode::ControlFlowInstruction& cf) {
    uint32_t address = cf.exec.address();
    uint32_t count = cf.exec.count();
    uint32_t sequence = cf.exec.sequence();
    bool relative_aL = cf.exec.addressing_mode() == ucode::AddressingMode::kRelative;
    (void)relative_aL;
    for (uint32_t k = 0; k < count; ++k) {
      uint32_t slot = address + k;
      if (slot * 3 + 2 >= n_) {
        result_.error = "instruction outside microcode";
        return;
      }
      const uint32_t* ins = d_ + slot * 3;
      bool is_fetch = (sequence >> (k * 2)) & 1;
      if (is_fetch)
        EmitFetch(w, ins);
      else
        EmitAlu(w, ins);
    }
  }

  // ---------------------------------------------------------------- operands
  std::string Temp(uint32_t index, bool relative) {
    max_temp_ = std::max(max_temp_, index + 1);
    if (relative) {
      uses_relative_temps_ = true;
      return Fmt("r[clamp(aL + %d, 0, 63)]", int(index));
    }
    return Fmt("r[%u]", index);
  }

  std::string ConstArray() const { return stage_ == ShaderStage::kVertex ? "vs_c" : "ps_c"; }

  std::string Const(uint32_t index, bool addressed, bool a0_relative) {
    if (addressed) {
      if (a0_relative) return Fmt("%s[clamp(a0 + %d, 0, 255)]", ConstArray().c_str(), int(index));
      return Fmt("%s[clamp(aL + %d, 0, 255)]", ConstArray().c_str(), int(index));
    }
    return Fmt("%s[%u]", ConstArray().c_str(), index & 0xFF);
  }

  // Full float4 source with relative swizzle, negate and abs applied.
  std::string AluSource(const ucode::AluInstruction& a, int i) {
    uint32_t reg = a.src_reg(i);
    std::string base;
    bool abs = false;
    if (a.src_is_temp(i)) {
      base = Temp(ucode::AluInstruction::src_temp_reg(reg),
                  ucode::AluInstruction::is_src_temp_relative(reg));
      abs = ucode::AluInstruction::is_src_temp_value_absolute(reg);
    } else {
      base = Const(reg, a.src_const_is_addressed(i), a.is_const_address_register_relative());
      abs = a.abs_constants();
    }
    uint32_t swz = a.src_swizzle(i);
    std::string s = base;
    if (swz != 0) {
      s += ".";
      for (uint32_t c = 0; c < 4; ++c)
        s += kComp[ucode::AluInstruction::GetSwizzledComponentIndex(swz, c)];
    }
    if (abs) s = "abs(" + s + ")";
    if (a.src_negate(i)) s = "(-" + s + ")";
    return s;
  }

  // ---------------------------------------------------------------- ALU
  void EmitAlu(Writer& w, const uint32_t* ins) {
    ucode::AluInstruction a;
    std::memcpy(&a, ins, 12);
    bool pred = a.is_predicated();
    if (pred) {
      w.line(Fmt("if (p0 == %s) {", a.predicate_condition() ? "true" : "false"));
      ++w.indent;
    }
    w.line("{");
    ++w.indent;

    AluVectorOpcode vop = a.vector_opcode();
    AluScalarOpcode sop = a.scalar_opcode();
    bool is_export = a.is_export();
    uint32_t vmask = a.vector_write_mask();
    uint32_t smask = a.scalar_write_mask();
    bool vector_needed = vmask != 0 || vop == AluVectorOpcode::kKillEq ||
                         vop == AluVectorOpcode::kKillGt || vop == AluVectorOpcode::kKillGe ||
                         vop == AluVectorOpcode::kKillNe || vop == AluVectorOpcode::kMaxA ||
                         (vop >= AluVectorOpcode::kSetpEqPush && vop <= AluVectorOpcode::kSetpGePush);

    // Read all sources first (co-issue semantics).
    std::string vres;
    if (vector_needed) {
      vres = VectorOp(w, a);
      w.line("float4 vres = " + vres + ";");
      if (a.vector_clamp()) w.line("vres = saturate(vres);");
    }
    bool scalar_needed = true;  // every scalar op except retain_prev updates ps
    if (sop == AluScalarOpcode::kRetainPrev && smask == 0) scalar_needed = false;
    if (scalar_needed) {
      std::string sres = ScalarOp(w, a);
      w.line("float sres = " + sres + ";");
      if (a.scalar_clamp()) w.line("sres = saturate(sres);");
      if (sop != AluScalarOpcode::kRetainPrev) w.line("ps = sres;");
    }

    if (!is_export) {
      if (vmask && vector_needed) {
        std::string dst = Temp(a.vector_dest(), a.is_vector_dest_relative());
        std::string m;
        for (int c = 0; c < 4; ++c)
          if (vmask & (1 << c)) m += kComp[c];
        w.line(Fmt("%s.%s = vres.%s;", dst.c_str(), m.c_str(), m.c_str()));
      }
      if (smask && scalar_needed) {
        std::string dst = Temp(a.scalar_dest(), a.is_scalar_dest_relative());
        std::string m;
        for (int c = 0; c < 4; ++c)
          if (smask & (1 << c)) m += kComp[c];
        w.line(Fmt("%s.%s = sres;", dst.c_str(), m.c_str()));
      }
    } else {
      EmitExport(w, a, vector_needed, scalar_needed);
    }
    --w.indent;
    w.line("}");
    if (pred) {
      --w.indent;
      w.line("}");
    }
  }

  void EmitExport(Writer& w, const ucode::AluInstruction& a, bool has_v, bool has_s) {
    uint32_t reg = a.vector_dest();
    uint32_t vmask = a.vector_write_mask();
    uint32_t smask = a.scalar_write_mask();
    bool zero_rest = a.is_scalar_dest_relative();
    std::string dst;
    if (stage_ == ShaderStage::kVertex) {
      if (reg == 62) {
        dst = "o_pos";
        writes_position_ = true;
      } else if (reg == 63) {
        dst = "o_misc";
      } else if (reg < 16) {
        dst = Fmt("o_interp[%u]", reg);
        result_.interpolators_written |= 1u << reg;
      } else {
        if (reg >= 32 && reg <= 37) result_.uses_memexport = true;
        return;  // memory export: unsupported, ignored
      }
    } else {
      if (reg < 4) {
        dst = Fmt("o_color[%u]", reg);
        result_.color_outputs |= 1u << reg;
      } else if (reg == 61) {
        dst = "o_depth4";
        result_.writes_depth = true;
      } else {
        if (reg >= 32 && reg <= 37) result_.uses_memexport = true;
        return;
      }
    }
    for (int c = 0; c < 4; ++c) {
      bool v = vmask & (1 << c), s = smask & (1 << c);
      std::string comp = Fmt("%s.%c", dst.c_str(), kComp[c]);
      if (v && s)
        w.line(comp + " = 1.0;");
      else if (v && has_v)
        w.line(Fmt("%s = vres.%c;", comp.c_str(), kComp[c]));
      else if (s && has_s)
        w.line(comp + " = sres;");
      else if (!v && !s && zero_rest)
        w.line(comp + " = 0.0;");
    }
  }

  std::string VectorOp(Writer& w, const ucode::AluInstruction& a) {
    auto s1 = [&] { return AluSource(a, 1); };
    auto s2 = [&] { return AluSource(a, 2); };
    auto s3 = [&] { return AluSource(a, 3); };
    switch (a.vector_opcode()) {
      case AluVectorOpcode::kAdd: return "(" + s1() + " + " + s2() + ")";
      case AluVectorOpcode::kMul: return "MulZ(" + s1() + ", " + s2() + ")";
      case AluVectorOpcode::kMax: {
        w.line("float4 va = " + s1() + "; float4 vb = " + s2() + ";");
        return "(va >= vb ? va : vb)";
      }
      case AluVectorOpcode::kMin: {
        w.line("float4 va = " + s1() + "; float4 vb = " + s2() + ";");
        return "(va < vb ? va : vb)";
      }
      case AluVectorOpcode::kSeq: return "(float4)(" + s1() + " == " + s2() + ")";
      case AluVectorOpcode::kSgt: return "(float4)(" + s1() + " > " + s2() + ")";
      case AluVectorOpcode::kSge: return "(float4)(" + s1() + " >= " + s2() + ")";
      case AluVectorOpcode::kSne: return "(float4)(" + s1() + " != " + s2() + ")";
      case AluVectorOpcode::kFrc: return "frac(" + s1() + ")";
      case AluVectorOpcode::kTrunc: return "trunc(" + s1() + ")";
      case AluVectorOpcode::kFloor: return "floor(" + s1() + ")";
      case AluVectorOpcode::kMad: return "(MulZ(" + s1() + ", " + s2() + ") + " + s3() + ")";
      case AluVectorOpcode::kCndEq: return "(" + s1() + " == 0.0 ? " + s2() + " : " + s3() + ")";
      case AluVectorOpcode::kCndGe: return "(" + s1() + " >= 0.0 ? " + s2() + " : " + s3() + ")";
      case AluVectorOpcode::kCndGt: return "(" + s1() + " > 0.0 ? " + s2() + " : " + s3() + ")";
      case AluVectorOpcode::kDp4: return "Dp4Z(" + s1() + ", " + s2() + ").xxxx";
      case AluVectorOpcode::kDp3: return "Dp3Z(" + s1() + ", " + s2() + ").xxxx";
      case AluVectorOpcode::kDp2Add: {
        w.line("float4 va = " + s1() + "; float4 vb = " + s2() + "; float4 vc = " + s3() + ";");
        return "(MulZ1(va.x, vb.x) + MulZ1(va.y, vb.y) + vc.x).xxxx";
      }
      case AluVectorOpcode::kCube: return "CubeVector(" + s1() + ", " + s2() + ")";
      case AluVectorOpcode::kMax4: {
        w.line("float4 va = " + s1() + ";");
        return "max(max(va.x, va.y), max(va.z, va.w)).xxxx";
      }
      case AluVectorOpcode::kSetpEqPush:
      case AluVectorOpcode::kSetpNePush:
      case AluVectorOpcode::kSetpGtPush:
      case AluVectorOpcode::kSetpGePush: {
        const char* op = a.vector_opcode() == AluVectorOpcode::kSetpEqPush   ? "=="
                         : a.vector_opcode() == AluVectorOpcode::kSetpNePush ? "!="
                         : a.vector_opcode() == AluVectorOpcode::kSetpGtPush ? ">"
                                                                              : ">=";
        w.line("float4 va = " + s1() + "; float4 vb = " + s2() + ";");
        w.line(Fmt("p0 = (va.w == 0.0 && vb.w %s 0.0);", op));
        return Fmt("((va.x == 0.0 && vb.x %s 0.0) ? 0.0 : va.x + 1.0).xxxx", op);
      }
      case AluVectorOpcode::kKillEq:
      case AluVectorOpcode::kKillGt:
      case AluVectorOpcode::kKillGe:
      case AluVectorOpcode::kKillNe: {
        const char* op = a.vector_opcode() == AluVectorOpcode::kKillEq   ? "=="
                         : a.vector_opcode() == AluVectorOpcode::kKillGt ? ">"
                         : a.vector_opcode() == AluVectorOpcode::kKillGe ? ">="
                                                                          : "!=";
        w.line("float4 va = " + s1() + "; float4 vb = " + s2() + ";");
        w.line(Fmt("bool vkill = any(va %s vb);", op));
        if (stage_ == ShaderStage::kPixel) {
          w.line("if (vkill) discard;");
          result_.uses_kill = true;
        }
        return "(vkill ? 1.0 : 0.0).xxxx";
      }
      case AluVectorOpcode::kDst: {
        w.line("float4 va = " + s1() + "; float4 vb = " + s2() + ";");
        return "float4(1.0, MulZ1(va.y, vb.y), va.z, vb.w)";
      }
      case AluVectorOpcode::kMaxA: {
        w.line("float4 va = " + s1() + "; float4 vb = " + s2() + ";");
        w.line("a0 = (int)clamp(floor(va.w + 0.5), -256.0, 255.0);");
        return "(va >= vb ? va : vb)";
      }
      default:
        return "float4(0.0, 0.0, 0.0, 0.0)";
    }
  }

  std::string ScalarOp(Writer& w, const ucode::AluInstruction& a) {
    AluScalarOpcode op = a.scalar_opcode();
    uint32_t swz = a.src_swizzle(3);
    uint32_t ca = ucode::AluInstruction::GetSwizzledComponentIndex(swz, 3);  // W
    // Single-operand `b` is Z when the vector op also uses source 3, X otherwise.
    AluVectorOpcode vop = a.vector_opcode();
    bool vector_uses_src3 = vop == AluVectorOpcode::kMad || vop == AluVectorOpcode::kCndEq ||
                            vop == AluVectorOpcode::kCndGe || vop == AluVectorOpcode::kCndGt ||
                            vop == AluVectorOpcode::kDp2Add;
    uint32_t cb = ucode::AluInstruction::GetSwizzledComponentIndex(swz, vector_uses_src3 ? 2 : 0);
    auto operand = [&]() {
      // Single r#/c# operand in src3.
      uint32_t reg = a.src_reg(3);
      std::string base;
      bool abs;
      if (a.src_is_temp(3)) {
        base = Temp(ucode::AluInstruction::src_temp_reg(reg),
                    ucode::AluInstruction::is_src_temp_relative(reg));
        abs = ucode::AluInstruction::is_src_temp_value_absolute(reg);
      } else {
        base = Const(reg, a.src_const_is_addressed(3), a.is_const_address_register_relative());
        abs = a.abs_constants();
      }
      std::string s = base;
      if (abs) s = "abs(" + s + ")";
      if (a.src_negate(3)) s = "(-" + s + ")";
      return s;
    };
    bool is_const_op = op == AluScalarOpcode::kMulsc0 || op == AluScalarOpcode::kMulsc1 ||
                       op == AluScalarOpcode::kAddsc0 || op == AluScalarOpcode::kAddsc1 ||
                       op == AluScalarOpcode::kSubsc0 || op == AluScalarOpcode::kSubsc1;
    if (is_const_op) {
      // a = constant (src3 index, W swizzle), b = temp (packed index, X swizzle).
      uint32_t reg = a.src_reg(3);
      std::string c = Const(reg, a.src_const_is_addressed(3), a.is_const_address_register_relative());
      std::string t = Temp(a.scalar_const_reg_op_src_temp_reg(), false);
      std::string sa = Fmt("%s.%c", c.c_str(), kComp[ca]);
      std::string sb = Fmt("%s.%c", t.c_str(),
                           kComp[ucode::AluInstruction::GetSwizzledComponentIndex(swz, 0)]);
      if (a.abs_constants()) {
        sa = "abs(" + sa + ")";
        sb = "abs(" + sb + ")";
      }
      if (a.src_negate(3)) {
        sa = "(-" + sa + ")";
        sb = "(-" + sb + ")";
      }
      w.line("float sa = " + sa + "; float sb = " + sb + ";");
      if (op == AluScalarOpcode::kMulsc0 || op == AluScalarOpcode::kMulsc1) return "MulZ1(sa, sb)";
      if (op == AluScalarOpcode::kAddsc0 || op == AluScalarOpcode::kAddsc1) return "(sa + sb)";
      return "(sa - sb)";
    }
    // Operand-less ops.
    if (op == AluScalarOpcode::kRetainPrev) return "ps";
    if (op == AluScalarOpcode::kSetpClr) {
      w.line("p0 = false;");
      return "3.402823466e+38";
    }
    std::string src = operand();
    w.line(Fmt("float4 so = %s; float sa = so.%c; float sb = so.%c;", src.c_str(), kComp[ca],
               kComp[cb]));
    switch (op) {
      case AluScalarOpcode::kAdds: return "(sa + sb)";
      case AluScalarOpcode::kAddsPrev: return "(sa + ps)";
      case AluScalarOpcode::kMuls: return "MulZ1(sa, sb)";
      case AluScalarOpcode::kMulsPrev: return "MulZ1(sa, ps)";
      case AluScalarOpcode::kMulsPrev2:
        return "((ps == -3.402823466e+38 || isinf(ps) || isnan(ps) || isinf(sb) || isnan(sb) || "
               "sb <= 0.0) ? -3.402823466e+38 : MulZ1(sa, ps))";
      case AluScalarOpcode::kMaxs: return "(sa >= sb ? sa : sb)";
      case AluScalarOpcode::kMins: return "(sa < sb ? sa : sb)";
      case AluScalarOpcode::kSeqs: return "(sa == 0.0 ? 1.0 : 0.0)";
      case AluScalarOpcode::kSgts: return "(sa > 0.0 ? 1.0 : 0.0)";
      case AluScalarOpcode::kSges: return "(sa >= 0.0 ? 1.0 : 0.0)";
      case AluScalarOpcode::kSnes: return "(sa != 0.0 ? 1.0 : 0.0)";
      case AluScalarOpcode::kFrcs: return "frac(sa)";
      case AluScalarOpcode::kTruncs: return "trunc(sa)";
      case AluScalarOpcode::kFloors: return "floor(sa)";
      case AluScalarOpcode::kExp: return "exp2(sa)";
      case AluScalarOpcode::kLogc: return "(sa == 1.0 ? 0.0 : max(log2(sa), -3.402823466e+38))";
      case AluScalarOpcode::kLog: return "(sa == 1.0 ? 0.0 : log2(sa))";
      case AluScalarOpcode::kRcpc: return "(sa == 1.0 ? 1.0 : ClampMax(1.0 / sa))";
      case AluScalarOpcode::kRcpf: {
        w.line("float rf = sa == 1.0 ? 1.0 : 1.0 / sa;");
        return "(isinf(rf) ? (rf < 0.0 ? -0.0 : 0.0) : rf)";
      }
      case AluScalarOpcode::kRcp: return "(sa == 1.0 ? 1.0 : 1.0 / sa)";
      case AluScalarOpcode::kRsqc: return "(sa == 1.0 ? 1.0 : ClampMax(rsqrt(sa)))";
      case AluScalarOpcode::kRsqf: {
        w.line("float rf = sa == 1.0 ? 1.0 : rsqrt(sa);");
        return "(isinf(rf) ? (rf < 0.0 ? -0.0 : 0.0) : rf)";
      }
      case AluScalarOpcode::kRsq: return "(sa == 1.0 ? 1.0 : rsqrt(sa))";
      case AluScalarOpcode::kMaxAs:
        w.line("a0 = (int)clamp(floor(sa + 0.5), -256.0, 255.0);");
        return "(sa >= sb ? sa : sb)";
      case AluScalarOpcode::kMaxAsf:
        w.line("a0 = (int)clamp(floor(sa), -256.0, 255.0);");
        return "(sa >= sb ? sa : sb)";
      case AluScalarOpcode::kSubs: return "(sa - sb)";
      case AluScalarOpcode::kSubsPrev: return "(sa - ps)";
      case AluScalarOpcode::kSetpEq:
        w.line("p0 = sa == 0.0;");
        return "(p0 ? 0.0 : 1.0)";
      case AluScalarOpcode::kSetpNe:
        w.line("p0 = sa != 0.0;");
        return "(p0 ? 0.0 : 1.0)";
      case AluScalarOpcode::kSetpGt:
        w.line("p0 = sa > 0.0;");
        return "(p0 ? 0.0 : 1.0)";
      case AluScalarOpcode::kSetpGe:
        w.line("p0 = sa >= 0.0;");
        return "(p0 ? 0.0 : 1.0)";
      case AluScalarOpcode::kSetpInv:
        w.line("p0 = sa == 1.0;");
        return "(p0 ? 0.0 : (sa == 0.0 ? 1.0 : sa))";
      case AluScalarOpcode::kSetpPop:
        w.line("p0 = sa - 1.0 <= 0.0;");
        return "(p0 ? 0.0 : sa - 1.0)";
      case AluScalarOpcode::kSetpRstr:
        w.line("p0 = sa == 0.0;");
        return "(p0 ? 0.0 : sa)";
      case AluScalarOpcode::kKillsEq:
      case AluScalarOpcode::kKillsGt:
      case AluScalarOpcode::kKillsGe:
      case AluScalarOpcode::kKillsNe:
      case AluScalarOpcode::kKillsOne: {
        const char* cond = op == AluScalarOpcode::kKillsEq   ? "sa == 0.0"
                           : op == AluScalarOpcode::kKillsGt ? "sa > 0.0"
                           : op == AluScalarOpcode::kKillsGe ? "sa >= 0.0"
                           : op == AluScalarOpcode::kKillsNe ? "sa != 0.0"
                                                             : "sa == 1.0";
        w.line(Fmt("bool skill = %s;", cond));
        if (stage_ == ShaderStage::kPixel) {
          w.line("if (skill) discard;");
          result_.uses_kill = true;
        }
        return "(skill ? 1.0 : 0.0)";
      }
      case AluScalarOpcode::kSqrt: return "sqrt(sa)";
      case AluScalarOpcode::kSin: return "sin(sa)";
      case AluScalarOpcode::kCos: return "cos(sa)";
      default: return "0.0";
    }
  }

  // ---------------------------------------------------------------- fetch
  void EmitFetch(Writer& w, const uint32_t* ins) {
    ucode::FetchInstruction f;
    std::memcpy(&f, ins, 12);
    bool pred = f.is_predicated();
    if (pred) {
      w.line(Fmt("if (p0 == %s) {", f.predicate_condition() ? "true" : "false"));
      ++w.indent;
    }
    if (f.opcode() == ucode::FetchOpcode::kVertexFetch)
      EmitVertexFetch(w, f.vertex_fetch());
    else
      EmitTextureFetch(w, f.texture_fetch());
    if (pred) {
      --w.indent;
      w.line("}");
    }
  }

  int VfetchBinding(uint32_t fetch_index) {
    for (size_t i = 0; i < result_.vfetch_slots.size(); ++i)
      if (result_.vfetch_slots[i] == fetch_index) return int(i);
    result_.vfetch_slots.push_back(uint8_t(fetch_index));
    return int(result_.vfetch_slots.size() - 1);
  }

  void EmitVertexFetch(Writer& w, const ucode::VertexFetchInstruction& v) {
    if (!v.is_mini_fetch()) {
      last_vfetch_ = v;
      has_last_vfetch_ = true;
    } else if (!has_last_vfetch_) {
      return;
    }
    const auto& full = v.is_mini_fetch() ? last_vfetch_ : v;
    uint32_t fetch_index = full.fetch_constant_index();
    int binding = VfetchBinding(fetch_index);
    w.line("{");
    ++w.indent;
    std::string src = Temp(full.src(), full.is_src_relative());
    char sc = kComp[full.src_swizzle() & 3];
    if (full.is_index_rounded())
      w.line(Fmt("int vidx = (int)floor(%s.%c + 0.5);", src.c_str(), sc));
    else
      w.line(Fmt("int vidx = (int)floor(%s.%c);", src.c_str(), sc));
    w.line(Fmt("uint vaddr = (uint)(vidx * %u + %d) * 4u;", full.stride(), v.offset()));
    {
      uint32_t words = 1;
      switch (v.data_format()) {
        case xenos::VertexFormat::k_16_16_16_16:
        case xenos::VertexFormat::k_16_16_16_16_FLOAT:
        case xenos::VertexFormat::k_32_32:
        case xenos::VertexFormat::k_32_32_FLOAT: words = 2; break;
        case xenos::VertexFormat::k_32_32_32_FLOAT: words = 3; break;
        case xenos::VertexFormat::k_32_32_32_32:
        case xenos::VertexFormat::k_32_32_32_32_FLOAT: words = 4; break;
        default: break;
      }
      result_.vfetch_stride.resize(result_.vfetch_slots.size(), 0);
      result_.vfetch_end.resize(result_.vfetch_slots.size(), 0);
      result_.vfetch_stride[binding] = full.stride();
      result_.vfetch_end[binding] = std::max(result_.vfetch_end[binding], uint32_t(int32_t(v.offset())) + words);
    }
    w.line(Fmt("uint vend = VfetchEndian(%uu);", fetch_index));
    std::string buf = Fmt("vb%d", binding);
    auto word = [&](int k) { return Fmt("GpuSwap(%s.Load(vaddr + %du), vend)", buf.c_str(), k * 4); };
    bool sgn = v.is_signed();
    bool norm = v.is_normalized();
    const char* sgn_s = sgn ? "true" : "false";
    const char* norm_s = norm ? "true" : "false";
    std::string val = "float4(0.0, 0.0, 0.0, 1.0)";
    switch (v.data_format()) {
      case xenos::VertexFormat::k_8_8_8_8:
        w.line("uint vw0 = " + word(0) + ";");
        val = Fmt("float4(UnpackField(vw0, 0u, 8u, %s, %s), UnpackField(vw0, 8u, 8u, %s, %s), "
                  "UnpackField(vw0, 16u, 8u, %s, %s), UnpackField(vw0, 24u, 8u, %s, %s))",
                  sgn_s, norm_s, sgn_s, norm_s, sgn_s, norm_s, sgn_s, norm_s);
        break;
      case xenos::VertexFormat::k_2_10_10_10:
        w.line("uint vw0 = " + word(0) + ";");
        val = Fmt("float4(UnpackField(vw0, 0u, 10u, %s, %s), UnpackField(vw0, 10u, 10u, %s, %s), "
                  "UnpackField(vw0, 20u, 10u, %s, %s), UnpackField(vw0, 30u, 2u, %s, %s))",
                  sgn_s, norm_s, sgn_s, norm_s, sgn_s, norm_s, sgn_s, norm_s);
        break;
      case xenos::VertexFormat::k_10_11_11:
        w.line("uint vw0 = " + word(0) + ";");
        val = Fmt("float4(UnpackField(vw0, 0u, 11u, %s, %s), UnpackField(vw0, 11u, 11u, %s, %s), "
                  "UnpackField(vw0, 22u, 10u, %s, %s), 1.0)",
                  sgn_s, norm_s, sgn_s, norm_s, sgn_s, norm_s);
        break;
      case xenos::VertexFormat::k_11_11_10:
        w.line("uint vw0 = " + word(0) + ";");
        val = Fmt("float4(UnpackField(vw0, 0u, 10u, %s, %s), UnpackField(vw0, 10u, 11u, %s, %s), "
                  "UnpackField(vw0, 21u, 11u, %s, %s), 1.0)",
                  sgn_s, norm_s, sgn_s, norm_s, sgn_s, norm_s);
        break;
      case xenos::VertexFormat::k_16_16:
        w.line("uint vw0 = " + word(0) + ";");
        val = Fmt("float4(UnpackField(vw0, 0u, 16u, %s, %s), UnpackField(vw0, 16u, 16u, %s, %s), "
                  "0.0, 1.0)",
                  sgn_s, norm_s, sgn_s, norm_s);
        break;
      case xenos::VertexFormat::k_16_16_16_16:
        w.line("uint vw0 = " + word(0) + "; uint vw1 = " + word(1) + ";");
        val = Fmt("float4(UnpackField(vw0, 0u, 16u, %s, %s), UnpackField(vw0, 16u, 16u, %s, %s), "
                  "UnpackField(vw1, 0u, 16u, %s, %s), UnpackField(vw1, 16u, 16u, %s, %s))",
                  sgn_s, norm_s, sgn_s, norm_s, sgn_s, norm_s, sgn_s, norm_s);
        break;
      case xenos::VertexFormat::k_16_16_FLOAT:
        w.line("uint vw0 = " + word(0) + ";");
        val = "float4(f16tof32(vw0 & 0xFFFFu), f16tof32(vw0 >> 16), 0.0, 1.0)";
        break;
      case xenos::VertexFormat::k_16_16_16_16_FLOAT:
        w.line("uint vw0 = " + word(0) + "; uint vw1 = " + word(1) + ";");
        val = "float4(f16tof32(vw0 & 0xFFFFu), f16tof32(vw0 >> 16), f16tof32(vw1 & 0xFFFFu), "
              "f16tof32(vw1 >> 16))";
        break;
      case xenos::VertexFormat::k_32:
        w.line("uint vw0 = " + word(0) + ";");
        val = norm ? (sgn ? "float4(max((float)(int)vw0 / 2147483647.0, -1.0), 0.0, 0.0, 1.0)"
                          : "float4((float)vw0 / 4294967295.0, 0.0, 0.0, 1.0)")
                   : (sgn ? "float4((float)(int)vw0, 0.0, 0.0, 1.0)"
                          : "float4((float)vw0, 0.0, 0.0, 1.0)");
        break;
      case xenos::VertexFormat::k_32_32:
        w.line("uint vw0 = " + word(0) + "; uint vw1 = " + word(1) + ";");
        val = sgn ? "float4((float)(int)vw0, (float)(int)vw1, 0.0, 1.0)"
                  : "float4((float)vw0, (float)vw1, 0.0, 1.0)";
        break;
      case xenos::VertexFormat::k_32_32_32_32:
        w.line("uint vw0 = " + word(0) + "; uint vw1 = " + word(1) + "; uint vw2 = " + word(2) +
               "; uint vw3 = " + word(3) + ";");
        val = sgn ? "float4((float)(int)vw0, (float)(int)vw1, (float)(int)vw2, (float)(int)vw3)"
                  : "float4((float)vw0, (float)vw1, (float)vw2, (float)vw3)";
        break;
      case xenos::VertexFormat::k_32_FLOAT:
        val = "float4(asfloat(" + word(0) + "), 0.0, 0.0, 1.0)";
        break;
      case xenos::VertexFormat::k_32_32_FLOAT:
        val = "float4(asfloat(" + word(0) + "), asfloat(" + word(1) + "), 0.0, 1.0)";
        break;
      case xenos::VertexFormat::k_32_32_32_FLOAT:
        val = "float4(asfloat(" + word(0) + "), asfloat(" + word(1) + "), asfloat(" + word(2) +
              "), 1.0)";
        break;
      case xenos::VertexFormat::k_32_32_32_32_FLOAT:
        val = "float4(asfloat(" + word(0) + "), asfloat(" + word(1) + "), asfloat(" + word(2) +
              "), asfloat(" + word(3) + "))";
        break;
      default:
        break;
    }
    w.line("float4 vf = " + val + ";");
    if (v.exp_adjust() != 0) w.line(Fmt("vf *= %s;", FloatLiteral(std::ldexp(1.0f, v.exp_adjust())).c_str()));
    EmitFetchDest(w, v.dest(), v.is_dest_relative(), v.dest_swizzle(), "vf");
    --w.indent;
    w.line("}");
  }

  void EmitFetchDest(Writer& w, uint32_t dest, bool relative, uint32_t swizzle, const char* val) {
    std::string dst = Temp(dest, relative);
    for (uint32_t c = 0; c < 4; ++c) {
      uint32_t s = (swizzle >> (3 * c)) & 7;
      if (s == 7) continue;
      std::string v;
      if (s < 4)
        v = Fmt("%s.%c", val, kComp[s]);
      else if (s == 4)
        v = "0.0";
      else
        v = "1.0";
      w.line(Fmt("%s.%c = %s;", dst.c_str(), kComp[c], v.c_str()));
    }
  }

  void EmitTextureFetch(Writer& w, const ucode::TextureFetchInstruction& t) {
    using ucode::FetchOpcode;
    uint32_t slot = t.fetch_constant_index();
    std::string src = Temp(t.src(), t.is_src_relative());
    uint32_t sw = t.src_swizzle();
    std::string coord =
        Fmt("float3(%s.%c, %s.%c, %s.%c)", src.c_str(), kComp[sw & 3], src.c_str(),
            kComp[(sw >> 2) & 3], src.c_str(), kComp[(sw >> 4) & 3]);
    w.line("{");
    ++w.indent;
    w.line("float3 tc = " + coord + ";");
    auto dim = t.dimension();
    TextureDimension td = TextureDimension::k2D;
    switch (dim) {
      case xenos::FetchOpDimension::k1D: td = TextureDimension::k1D; break;
      case xenos::FetchOpDimension::k2D: td = TextureDimension::k2D; break;
      case xenos::FetchOpDimension::k3DOrStacked: td = TextureDimension::k3D; break;
      case xenos::FetchOpDimension::kCube: td = TextureDimension::kCube; break;
    }
    switch (t.opcode()) {
      case FetchOpcode::kTextureFetch: {
        UseTexture(slot, td);
        if (t.unnormalized_coordinates() && td != TextureDimension::kCube)
          w.line(Fmt("tc /= max(sys_tex_size[%u].xyz, 1.0);", slot));
        float ox = t.offset_x(), oy = t.offset_y(), oz = t.offset_z();
        if (ox != 0.0f || oy != 0.0f || oz != 0.0f)
          w.line(Fmt("tc += float3(%s, %s, %s) / max(sys_tex_size[%u].xyz, 1.0);",
                     FloatLiteral(ox).c_str(), FloatLiteral(oy).c_str(), FloatLiteral(oz).c_str(),
                     slot));
        std::string sample;
        bool lod_fetch = stage_ == ShaderStage::kVertex || t.use_register_lod() ||
                         !t.use_computed_lod();
        std::string lod = t.use_register_lod() ? "tex_lod" : "0.0";
        float bias = t.lod_bias();
        if (bias != 0.0f && !lod_fetch) lod_fetch = false;
        std::string tex = TextureName(slot, td);
        std::string samp = Fmt("samp%u", slot);
        std::string c = td == TextureDimension::k1D ? "float2(tc.x, 0.5)"
                        : td == TextureDimension::k2D ? "tc.xy"
                        : td == TextureDimension::kCube ? "CubeDirection(tc)"
                                                        : "tc";
        if (td == TextureDimension::k3D) {
          // Stacked 2D textures are bound as arrays: z is the layer index.
          c = Fmt("Tex3DCoord%u(tc)", slot);
        }
        auto make_sample = [&](const std::string& t, const std::string& coord) {
          if (lod_fetch)
            return Fmt("%s.SampleLevel(%s, %s, %s)", t.c_str(), samp.c_str(), coord.c_str(),
                       lod.c_str());
          if (bias != 0.0f)
            return Fmt("%s.SampleBias(%s, %s, %s)", t.c_str(), samp.c_str(), coord.c_str(),
                       FloatLiteral(bias).c_str());
          return Fmt("%s.Sample(%s, %s)", t.c_str(), samp.c_str(), coord.c_str());
        };
        sample = make_sample(tex, c);
        if (td == TextureDimension::k3D && stage_ == ShaderStage::kPixel) {
          // 3D textures are bound as volumes (filtered across slices), stacked ones as arrays;
          // the fetch constant decides which at draw time (tex_flags bit 1).
          sample = Fmt("((TexFlags(%uu) & 2u) ? %s : %s)", slot,
                       make_sample(Fmt("tex%uv", slot), "tc").c_str(), sample.c_str());
        }
        w.line(Fmt("float4 tf = ApplyTexSwizzle(%s, %uu);", sample.c_str(), slot));
        EmitFetchDest(w, t.dest(), t.is_dest_relative(), t.dest_swizzle(), "tf");
        break;
      }
      case FetchOpcode::kGetTextureWeights: {
        UseTexture(slot, td);
        if (t.unnormalized_coordinates())
          w.line(Fmt("tc /= max(sys_tex_size[%u].xyz, 1.0);", slot));
        w.line(Fmt("float4 tf = float4(frac(tc * sys_tex_size[%u].xyz - 0.5), 0.0);", slot));
        EmitFetchDest(w, t.dest(), t.is_dest_relative(), t.dest_swizzle(), "tf");
        break;
      }
      case FetchOpcode::kGetTextureComputedLod: {
        UseTexture(slot, td);
        std::string tex = TextureName(slot, td);
        if (stage_ == ShaderStage::kPixel && td == TextureDimension::k2D)
          w.line(Fmt("float4 tf = %s.CalculateLevelOfDetailUnclamped(samp%u, tc.xy).xxxx;",
                     tex.c_str(), slot));
        else
          w.line("float4 tf = 0.0;");
        EmitFetchDest(w, t.dest(), t.is_dest_relative(), t.dest_swizzle(), "tf");
        break;
      }
      case FetchOpcode::kGetTextureGradients: {
        if (stage_ == ShaderStage::kPixel)
          w.line("float4 tf = float4(ddx(tc.x), ddy(tc.x), ddx(tc.y), ddy(tc.y));");
        else
          w.line("float4 tf = 0.0;");
        EmitFetchDest(w, t.dest(), t.is_dest_relative(), t.dest_swizzle(), "tf");
        break;
      }
      case FetchOpcode::kSetTextureLod:
        w.line("tex_lod = tc.x;");
        break;
      default:
        break;  // register gradients: implicit gradients are used instead
    }
    --w.indent;
    w.line("}");
  }

  void UseTexture(uint32_t slot, TextureDimension td) {
    result_.tfetch_mask |= 1u << slot;
    if (result_.tfetch_dimension[slot] == TextureDimension::kNone)
      result_.tfetch_dimension[slot] = td;
  }

  std::string TextureName(uint32_t slot, TextureDimension td) {
    // One resource per slot: the first dimension used wins.
    (void)td;
    return Fmt("tex%u", slot);
  }

  // ---------------------------------------------------------------- assembly
  void Assemble(const std::string& body) {
    std::string s;
    s += ShaderCommonHlsl();
    for (uint32_t slot = 0; slot < 32; ++slot) {
      if (!(result_.tfetch_mask & (1u << slot))) continue;
      const char* type = "Texture2D<float4>";
      switch (result_.tfetch_dimension[slot]) {
        case TextureDimension::k1D: type = "Texture2D<float4>"; break;  // 1D bound as Nx1 2D
        case TextureDimension::k3D: type = "Texture2DArray<float4>"; break;
        case TextureDimension::kCube: type = "TextureCube<float4>"; break;
        default: break;
      }
      s += Fmt("%s tex%u : register(t%u);\nSamplerState samp%u : register(s%u);\n", type, slot,
               slot, slot, slot);
      if (result_.tfetch_dimension[slot] == TextureDimension::k3D) {
        s += Fmt("float3 Tex3DCoord%u(float3 tc) { return float3(tc.xy, floor(tc.z * "
                 "max(sys_tex_size[%u].z, 1.0))); }\n",
                 slot, slot);
        if (stage_ == ShaderStage::kPixel)
          s += Fmt("Texture3D<float4> tex%uv : register(t%u);\n", slot, kVolumeTextureBase + slot);
      }
    }
    for (size_t i = 0; i < result_.vfetch_slots.size(); ++i)
      s += Fmt("ByteAddressBuffer vb%u : register(t%u);\n", uint32_t(i), uint32_t(32 + i));

    uint32_t temps = std::max<uint32_t>(max_temp_, 16);
    temps = std::min<uint32_t>(std::max(temps, uses_relative_temps_ ? 64u : temps), 64);
    if (stage_ == ShaderStage::kVertex) {
      s += "struct VsOutput {\n  float4 position : SV_Position;\n  float4 interp[16] : TEXCOORD0;\n"
           "  float4 misc : TEXCOORD16;\n};\n";
      s += "VsOutput main(uint vertex_id : SV_VertexID) {\n";
      s += ZeroArray("r", temps);
      s += "  r[0].x = (float)vertex_id;\n";
      s += "  float4 o_pos = float4(0.0, 0.0, 0.0, 1.0);\n  float4 o_misc = 0.0;\n";
      s += ZeroArray("o_interp", 16);
      s += "  bool p0 = false; float ps = 0.0; int a0 = 0; int aL = 0; float tex_lod = 0.0;\n";
      s += "  bool done = false;\n";
      s += body;
      // Guest viewport transform, then host normalized device coordinates.
      s += R"HLSL(
  VsOutput output;
  float4 p = o_pos;
  // PA_CL_VTE_CNTL.vtx_w0_fmt (0x400) is set by Direct3D for every draw; the exported W is
  // used as is (treating it as a reciprocal blows up any draw whose W is not 1).
  float w = p.w;
  float inv_w = (w != 0.0) ? 1.0 / w : 1.0;
  float3 ndc = p.xyz;
  if (!(sys_vte_flags & 0x100u)) ndc.xy *= inv_w;
  if (!(sys_vte_flags & 0x200u)) ndc.z *= inv_w;
  float sx = (sys_vte_flags & 0x1u) ? ndc.x * sys_viewport.x : ndc.x;
  float sy = (sys_vte_flags & 0x4u) ? ndc.y * sys_viewport.z : ndc.y;
  float sz = (sys_vte_flags & 0x10u) ? ndc.z * sys_viewport_z.x : ndc.z;
  if (sys_vte_flags & 0x2u) sx += sys_viewport.y;
  if (sys_vte_flags & 0x8u) sy += sys_viewport.w;
  if (sys_vte_flags & 0x20u) sz += sys_viewport_z.y;
  sx += sys_window_offset_x + sys_pixel_offset;
  sy += sys_window_offset_y + sys_pixel_offset;
  float2 rt = max(sys_viewport_z.zw, 1.0);
  output.position = float4((sx / rt.x * 2.0 - 1.0) * w, (1.0 - sy / rt.y * 2.0) * w, sz * w, w);
  [unroll] for (uint oj = 0; oj < 16u; ++oj) output.interp[oj] = o_interp[oj];
  output.misc = o_misc;
  return output;
}
)HLSL";
    } else {
      s += "struct PsInput {\n  float4 position : SV_Position;\n  float4 interp[16] : TEXCOORD0;\n"
           "  float4 misc : TEXCOORD16;\n  bool front : SV_IsFrontFace;\n};\n";
      s += "struct PsOutput {\n  float4 color[4] : SV_Target0;\n";
      if (result_.writes_depth) s += "  float depth : SV_Depth;\n";
      s += "};\n";
      s += "PsOutput main(PsInput input) {\n";
      s += ZeroArray("r", temps);
      s += "  [unroll] for (uint ii = 0; ii < 16u; ++ii) r[ii] = input.interp[ii];\n";
      if (key_.param_gen && key_.param_gen_register < temps) {
        // XY = pixel position, Z = front facing sign, W = point coordinate (0).
        // In guest pixels: the render target may be drawn at a higher internal resolution.
        s += Fmt("  r[%u] = float4(input.position.xy * sys_res_scale.y - 0.5, input.front ? 0.0 : -0.0, 0.0);\n",
                 key_.param_gen_register);
        s += Fmt("  if (!input.front) r[%u].x = -r[%u].x - 1.0;\n", key_.param_gen_register,
                 key_.param_gen_register);
      }
      s += ZeroArray("o_color", 4);
      s += "  float4 o_depth4 = float4(input.position.z, 0.0, 0.0, 0.0);\n";
      s += "  bool p0 = false; float ps = 0.0; int a0 = 0; int aL = 0; float tex_lod = 0.0;\n";
      s += "  bool done = false;\n";
      s += body;
      s += R"HLSL(
  if (sys_alpha_test & 8u) {
    uint f = sys_alpha_test & 7u;
    float a = o_color[0].a, ref = sys_alpha_ref;
    bool alpha_ok = f == 0u ? false : f == 1u ? a < ref : f == 2u ? a == ref : f == 3u ? a <= ref
              : f == 4u ? a > ref : f == 5u ? a != ref : f == 6u ? a >= ref : true;
    if (!alpha_ok) discard;
  }
  PsOutput output;
  [unroll] for (uint ok = 0; ok < 4u; ++ok) output.color[ok] = o_color[ok] * sys_rt_exp_scale[ok];
)HLSL";
      if (result_.writes_depth) s += "  output.depth = saturate(o_depth4.x);\n";
      s += "  return output;\n}\n";
    }
    result_.hlsl = std::move(s);
  }

  ShaderStage stage_;
  const uint32_t* d_;
  uint32_t n_;
  ShaderKey key_;
  TranslatedShader result_;
  std::vector<ucode::ControlFlowInstruction> cfs_;
  uint32_t max_temp_ = 0;
  bool uses_relative_temps_ = false;
  bool writes_position_ = false;
  ucode::VertexFetchInstruction last_vfetch_{};
  bool has_last_vfetch_ = false;
};

}  // namespace

TranslatedShader TranslateShader(ShaderStage stage, const uint32_t* dwords, uint32_t count,
                                 const ShaderKey& key) {
  Translator t(stage, dwords, count, key);
  return t.Run();
}

}  // namespace copa
