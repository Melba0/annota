// Annota - jit.hpp : the machine code backend behind the single `[[jit]]` marker.
//
// `[[jit]]` asks for load-time optimisation.  The superinstruction pass in the compiler always
// runs; on top of that this file tries to translate the whole function into x86-64 machine code.
// It only accepts functions it can prove safe - integer locals, arithmetic, comparisons, branches
// and returns - and anything else keeps running on the interpreter, so the marker never changes
// what a program means.  Before calling the native code the VM checks that every local the code
// reads currently holds a plain integer; otherwise the call falls back to the interpreter.
#pragma once
#include "bytecode.hpp"
#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <memory>
#include <unordered_map>
#include <vector>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#else
#include <sys/mman.h>
#include <unistd.h>
#endif

namespace annota {

using JitFn = int64_t (*)(const int64_t*, void*);

// The native code writes the returned value here.  `kind`: 0 = int (untyped), 1 = int64,
// 2 = bool, 3 = null - the same Value the interpreter would have produced.
struct JitOut {
    int64_t value = 0;
    int64_t kind = 3;
};

struct JitCode {
    std::vector<uint8_t> bytes;
    std::vector<uint8_t> readSlots;      // locals that must hold an int when the native code runs
    // loop headers that can be entered directly (hot-loop promotion): bytecode ip -> entry
    std::map<size_t, JitFn> osrEntries;
    JitFn fn = nullptr;
    void* mapping = nullptr;
    size_t mappingSize = 0;

    ~JitCode() {
        if (mapping) {
#ifdef _WIN32
            VirtualFree(mapping, 0, MEM_RELEASE);
#else
            munmap(mapping, mappingSize);
#endif
        }
    }
};

inline void* jitAllocExec(size_t size) {
#ifdef _WIN32
    void* p = VirtualAlloc(nullptr, size, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
    return p;
#else
    void* p = mmap(nullptr, size, PROT_READ | PROT_WRITE | PROT_EXEC,
                   MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    return p == MAP_FAILED ? nullptr : p;
#endif
}

// ---------------------------------------------------------------- static analysis
// Kinds are tracked exactly where possible: `None` (an untyped int, what `new x = 0` makes),
// `I64` (what integer arithmetic produces, matching promoteNum(None, None)), `Bool` and
// `Unknown` (a parameter before it is narrowed).  Arithmetic on `Unknown` makes the function
// ineligible, which keeps every translated function observably identical to the interpreter.
// A local's kind is tracked as a *set* of possible integer kinds: a parameter could be any of
// them (the runtime check only guarantees it is an int), while `new x = 0` narrows it to `None`
// and arithmetic narrows it again.  `promoteNum(None, None)` is I64, so `x = x + y` has an exact
// kind even inside a loop - which is what makes the return value provably identical to the
// interpreter's.
using JitKindSet = uint16_t;

static inline JitKindSet jitBit(NumKind k) { return (JitKindSet)1 << (unsigned)k; }

static const JitKindSet kJitAllInts =
    jitBit(NumKind::None) | jitBit(NumKind::I8) | jitBit(NumKind::I16) | jitBit(NumKind::I32) |
    jitBit(NumKind::I64) | jitBit(NumKind::U8) | jitBit(NumKind::U16) | jitBit(NumKind::U32) |
    jitBit(NumKind::U64);

static const JitKindSet kJitNone = jitBit(NumKind::None);
static const JitKindSet kJitBool = (JitKindSet)1 << 15;      // a comparison result

// promote every pair and collect the outcomes
static inline JitKindSet jitPromoteSet(JitKindSet a, JitKindSet b) {
    JitKindSet out = 0;
    for (int i = 0; i < 16; i++) {
        if (!(a & ((JitKindSet)1 << i))) continue;
        for (int j = 0; j < 16; j++) {
            if (!(b & ((JitKindSet)1 << j))) continue;
            NumKind ka = i == 15 ? NumKind::None : (NumKind)i;
            NumKind kb = j == 15 ? NumKind::None : (NumKind)j;
            if (i == 15 || j == 15) { out |= kJitBool; continue; }     // comparisons produce bool
            NumKind r = promoteNum(ka, kb);
            out |= jitBit(r);
        }
    }
    return out;
}

// -1 when the set does not pin down exactly one kind
static inline int jitSoleKind(JitKindSet s) {
    if (s == kJitBool) return 15;
    if (s == 0 || (s & (s - 1)) != 0) return -1;
    for (int i = 0; i < 15; i++) if (s == ((JitKindSet)1 << i)) return i;
    return -1;
}

static inline int jitOpBytesUnused(uint8_t op) {
    switch (op) {
        case OP_NOP: return 1;
        case OP_INT1: case OP_GET_LOCAL: case OP_SET_LOCAL: case OP_INIT_LOCAL:
        case OP_LOCAL_ADD_IMM: case OP_LOCAL_SUB_IMM: case OP_LOCAL_ADD_LOCAL:
        case OP_JUMP_IF_NOT_LT_LOCAL_IMM: return 0;      // variable, handled below
        default: return 0;
    }
}

// one decoded instruction
struct JitIns {
    uint8_t op = 0;
    size_t at = 0;          // bytecode offset
    size_t next = 0;        // offset of the following instruction
    int a = 0, b = 0, imm = 0, target = 0;
};

// ---------------------------------------------------------------- emitter
class JitEmitter {
public:
    std::vector<uint8_t> c;
    std::vector<std::pair<size_t, size_t>> pending;    // (rel32 patch position, target bytecode ip)
    std::unordered_map<size_t, size_t> labelAt;        // bytecode ip -> native offset
    size_t frameBytes = 0;
    size_t kindBase = 0;                     // byte offset of the per-local kind slots

    void storeKindByte(size_t byteOff, int kind) {
        u8(0xC6); u8(0x44); u8(0x24); u8((uint8_t)byteOff); u8((uint8_t)kind);
    }
    void loadKindByteToR8(size_t byteOff) {
        u8(0x44); u8(0x0F); u8(0xB6); u8(0x44); u8(0x24); u8((uint8_t)byteOff);  // movzx r8d,[rsp+off]
    }
    // out->value = rax (the value stays intact), out->kind = r8
    void storeResultWithKindInR8() {
        u8(0x48); u8(0x89);
        if (kOut == 0x02) u8(0x02); else u8(0x06);            // mov [out], rax
        u8(0x4C); u8(0x89);
        if (kOut == 0x02) u8(0x42); else u8(0x46);            // mov [out+8], r8
        u8(0x08);
    }

#ifdef _WIN32
    static constexpr uint8_t kBase = 0x01;             // rcx
    static constexpr uint8_t kOut = 0x02;              // rdx
#else
    static constexpr uint8_t kBase = 0x07;             // rdi
    static constexpr uint8_t kOut = 0x06;              // rsi
#endif

    void u8(uint8_t v) { c.push_back(v); }
    void u32(uint32_t v) {
        c.push_back((uint8_t)(v & 0xff));
        c.push_back((uint8_t)((v >> 8) & 0xff));
        c.push_back((uint8_t)((v >> 16) & 0xff));
        c.push_back((uint8_t)((v >> 24) & 0xff));
    }

    // --- locals (base register) ---
    void loadLocalToRax(int slot) {
        u8(0x48); u8(0x8B);
        if (slot * 8 < 128) { u8(0x40 | kBase); u8((uint8_t)(slot * 8)); }
        else { u8(0x80 | kBase); u32((uint32_t)(slot * 8)); }
    }
    void storeRaxToLocal(int slot) {
        u8(0x48); u8(0x89);
        if (slot * 8 < 128) { u8(0x40 | kBase); u8((uint8_t)(slot * 8)); }
        else { u8(0x80 | kBase); u32((uint32_t)(slot * 8)); }
    }
    void loadLocalToR8(int slot) {
        u8(0x4C); u8(0x8B);
        if (slot * 8 < 128) { u8(0x40 | kBase); u8((uint8_t)(slot * 8)); }
        else { u8(0x80 | kBase); u32((uint32_t)(slot * 8)); }
    }
    void cmpRaxWithLocal(int slot) {
        u8(0x48); u8(0x3B);
        if (slot * 8 < 128) { u8(0x40 | kBase); u8((uint8_t)(slot * 8)); }
        else { u8(0x80 | kBase); u32((uint32_t)(slot * 8)); }
    }

    // --- the operand stack lives in the native stack frame ---
    void storeRaxToVr(int vr) {
        u8(0x48); u8(0x89); u8(0x44); u8(0x24); u8((uint8_t)(vr * 8));
    }
    void loadVrToRax(int vr) {
        u8(0x48); u8(0x8B); u8(0x44); u8(0x24); u8((uint8_t)(vr * 8));
    }
    void loadVrToR8(int vr) {
        u8(0x4C); u8(0x8B); u8(0x44); u8(0x24); u8((uint8_t)(vr * 8));
    }

    void movRaxImm(int32_t v) { u8(0x48); u8(0xC7); u8(0xC0); u32((uint32_t)v); }
    void movR8Imm(int32_t v) { u8(0x49); u8(0xC7); u8(0xC0); u32((uint32_t)v); }
    void addRaxR8() { u8(0x4C); u8(0x01); u8(0xC0); }
    void subRaxR8() { u8(0x4C); u8(0x29); u8(0xC0); }
    void imulRaxR8() { u8(0x49); u8(0x0F); u8(0xAF); u8(0xC0); }
    void cmpRaxR8() { u8(0x4C); u8(0x39); u8(0xC0); }
    void cmpRaxImm(int32_t v) { u8(0x48); u8(0x3D); u32((uint32_t)v); }
    void testRaxRax() { u8(0x48); u8(0x85); u8(0xC0); }
    void setcc(uint8_t cc) { u8(0x0F); u8(cc); u8(0xC0); u8(0x0F); u8(0xB6); u8(0xC0); }

    void jmpPlaceholder(size_t targetIp) { u8(0xE9); pending.push_back({c.size(), targetIp}); u32(0); }
    void jccPlaceholder(uint8_t cc, size_t targetIp) {
        u8(0x0F); u8(cc); pending.push_back({c.size(), targetIp}); u32(0);
    }

    // out->value = rax ; out->kind = kind
    void storeResult(int kind) {
        u8(0x48); u8(0x89);
        if (kOut == 0x02) u8(0x02); else u8(0x06);          // [rdx] / [rsi]
        u8(0x48); u8(0xC7);
        if (kOut == 0x02) u8(0x42); else u8(0x46);          // [rdx+8] / [rsi+8]
        u8(0x08);
        u32((uint32_t)kind);
    }
    void epilogue() {
        u8(0x48); u8(0x81); u8(0xC4); u32((uint32_t)frameBytes);   // add rsp, frameBytes
        u8(0xC3);                                                  // ret
    }
    void nullReturn() {
        u8(0x48); u8(0xC7); u8(0xC0); u32(0);                      // mov rax, 0
        storeResult(3);
        epilogue();
    }

    bool finish() {
        for (auto& p : pending) {
            auto it = labelAt.find(p.second);
            if (it == labelAt.end()) return false;
            int64_t rel = (int64_t)it->second - (int64_t)(p.first + 4);
            if (rel < -2000000000LL || rel > 2000000000LL) return false;
            int32_t r32 = (int32_t)rel;
            for (int i = 0; i < 4; i++) c[p.first + (size_t)i] = (uint8_t)((r32 >> (8 * i)) & 0xff);
        }
        return true;
    }
};

// ---------------------------------------------------------------- compile
inline std::shared_ptr<JitCode> jitFail(int line) {
    if (std::getenv("ANNOTA_JIT_DEBUG")) std::fprintf(stderr, "[jit] not eligible at line %d\n", line);
    return nullptr;
}
inline std::shared_ptr<JitCode> jitCompileX64(const std::shared_ptr<Chunk>& ch) {
    if (!ch) return jitFail(__LINE__);
    const std::vector<uint8_t>& code = ch->code;
    if (ch->numLocals > 64 || code.empty()) return jitFail(__LINE__);
    if (std::getenv("ANNOTA_NO_JIT")) return jitFail(__LINE__);

    // ---- decode and classify (only the integer subset is translatable)
    std::vector<JitIns> ins;
    std::unordered_map<size_t, size_t> indexOf;
    size_t ip = 0;
    while (ip < code.size()) {
        JitIns x;
        x.op = code[ip];
        x.at = ip;
        x.next = ip;
        switch (x.op) {
            case OP_NOP: x.next = ip + 1; break;
            case OP_INT1: x.next = ip + 2; x.imm = (int8_t)code[ip + 1]; break;
            case OP_GET_LOCAL: case OP_SET_LOCAL: case OP_INIT_LOCAL:
                x.next = ip + 2; x.a = code[ip + 1]; break;
            case OP_ADD: case OP_SUB: case OP_MUL: case OP_EQ: case OP_NE:
            case OP_LT: case OP_GT: case OP_LE: case OP_GE:
                x.next = ip + 1; break;
            case OP_LOCAL_ADD_IMM: case OP_LOCAL_SUB_IMM:
                x.next = ip + 3; x.a = code[ip + 1]; x.imm = (int8_t)code[ip + 2]; break;
            case OP_LOCAL_ADD_LOCAL:
                x.next = ip + 3; x.a = code[ip + 1]; x.b = code[ip + 2]; break;
            case OP_JUMP: case OP_LOOP:
                x.next = ip + 3;
                x.target = (int)((int64_t)(ip + 3) + (int16_t)((code[ip + 1] << 8) | code[ip + 2]));
                break;
            case OP_JUMP_IF_FALSE:
                x.next = ip + 3;
                x.target = (int)((int64_t)(ip + 3) + (int16_t)((code[ip + 1] << 8) | code[ip + 2]));
                break;
            case OP_JUMP_IF_NOT_LT_LOCAL_LOCAL:
                x.next = ip + 5; x.a = code[ip + 1]; x.b = code[ip + 2];
                x.target = (int)((int64_t)(ip + 5) + (int16_t)((code[ip + 3] << 8) | code[ip + 4]));
                break;
            case OP_JUMP_IF_NOT_LT_LOCAL_IMM:
                x.next = ip + 5; x.a = code[ip + 1]; x.imm = (int8_t)code[ip + 2];
                x.target = (int)((int64_t)(ip + 5) + (int16_t)((code[ip + 3] << 8) | code[ip + 4]));
                break;
            case OP_RETURN: case OP_RETURN_NULL: x.next = ip + 1; break;
            default: return jitFail(__LINE__);                       // not translatable: keep interpreting
        }
        if (x.next > code.size()) return jitFail(__LINE__);
        if (std::getenv("ANNOTA_JIT_DEBUG"))
            std::fprintf(stderr, "[jit]   ip %zu op %d next %zu\n", x.at, (int)x.op, x.next);
        indexOf[x.at] = ins.size();
        ins.push_back(x);
        ip = x.next;
    }
    if (ins.empty()) return jitFail(__LINE__);
    if (ch->isMethod) {
        for (auto& x : ins) {
            bool writes = x.op == OP_SET_LOCAL || x.op == OP_INIT_LOCAL ||
                          x.op == OP_LOCAL_ADD_IMM || x.op == OP_LOCAL_SUB_IMM ||
                          x.op == OP_LOCAL_ADD_LOCAL;
            if (writes && x.a == 0) return jitFail(__LINE__);      // `this` is not an integer
        }
    }

    // ---- forward analysis: stack depth, local kinds, definite assignment, ceiling on stack
    size_t n = ins.size();
    const int kUnset = -1;
    std::vector<int> depth(n, kUnset);
    std::vector<std::vector<JitKindSet>> kindAt(n, std::vector<JitKindSet>(ch->numLocals, kJitAllInts));
    std::vector<std::vector<uint8_t>> assignedAt(n, std::vector<uint8_t>(ch->numLocals, 0));
    std::vector<std::vector<JitKindSet>> vrKind(n);            // kinds of the operand stack per entry
    int maxDepth = 0;
    depth[0] = 0;
    for (size_t i = 0; i < ch->numLocals; i++) {
        // parameters are unknown until narrowed; every other local starts unassigned
        bool isParam = i < ch->params.size() && !(ch->isMethod && i == 0);
        kindAt[0][i] = kJitAllInts;
        assignedAt[0][i] = 0;                       // parameters are checked at call time
        (void)isParam;
    }
    if (ch->isMethod && ch->numLocals > 0) kindAt[0][0] = kJitAllInts;

    auto meet = [](JitKindSet a, JitKindSet b) -> JitKindSet { return (JitKindSet)(a | b); };

    bool changed = true;
    int rounds = 0;
    while (changed && rounds++ < 64) {
        changed = false;
        for (size_t i = 0; i < n; i++) {
            if (depth[i] == kUnset) continue;
            const JitIns& x = ins[i];
            int d = depth[i];
            std::vector<JitKindSet> vk = vrKind[i];
            auto kindOfLocal = [&](int s) { return kindAt[i][(size_t)s]; };
            auto pushKind = [&](JitKindSet k) { vk.push_back(k); };
            switch (x.op) {
                case OP_NOP: break;
                case OP_INT1: pushKind(kJitNone); break;
                case OP_GET_LOCAL: pushKind(kindOfLocal(x.a)); break;
                case OP_SET_LOCAL: case OP_INIT_LOCAL:
                    if (d < 1) return jitFail(__LINE__);
                    vk.pop_back();
                    break;
                case OP_LOCAL_ADD_IMM: case OP_LOCAL_SUB_IMM:
                case OP_LOCAL_ADD_LOCAL:
                    break;                                  // raw int64 work, verified at call time
                case OP_ADD: case OP_SUB: case OP_MUL: {
                    if (d < 2) return jitFail(__LINE__);
                    JitKindSet ka = vk[(size_t)d - 2], kb = vk[(size_t)d - 1];
                    vk.pop_back();
                    vk.back() = jitPromoteSet(ka, kb);
                    break;
                }
                case OP_EQ: case OP_NE: case OP_LT: case OP_GT: case OP_LE: case OP_GE: {
                    if (d < 2) return jitFail(__LINE__);
                    vk.pop_back();
                    vk.back() = kJitBool;
                    break;
                }
                case OP_JUMP: case OP_LOOP: break;
                case OP_JUMP_IF_FALSE: {
                    if (d < 1) return jitFail(__LINE__);
                    vk.pop_back();
                    break;
                }
                case OP_JUMP_IF_NOT_LT_LOCAL_LOCAL: case OP_JUMP_IF_NOT_LT_LOCAL_IMM: break;
                case OP_RETURN:
                    if (d < 1) return jitFail(__LINE__);
                    break;                                   // exact kind handled at emit time
                case OP_RETURN_NULL: break;
                default: return jitFail(__LINE__);
            }
            maxDepth = std::max(maxDepth, (int)vk.size());
            // propagate to the fallthrough
            size_t to = i + 1;
            bool jumpOnly = (x.op == OP_JUMP || x.op == OP_LOOP);
            // local updates along the fallthrough
            auto applyLocals = [&](std::vector<JitKindSet>& k, std::vector<uint8_t>& asg) {
                switch (x.op) {
                    case OP_SET_LOCAL: case OP_INIT_LOCAL: {
                        if (d < 1) return;
                        k[(size_t)x.a] = vk[(size_t)d - 1];
                        asg[(size_t)x.a] = 1;
                        break;
                    }
                    case OP_LOCAL_ADD_IMM: case OP_LOCAL_SUB_IMM:
                        k[(size_t)x.a] = jitPromoteSet(k[(size_t)x.a], kJitNone);
                        asg[(size_t)x.a] = 1;
                        break;
                    case OP_LOCAL_ADD_LOCAL:
                        k[(size_t)x.a] = jitPromoteSet(k[(size_t)x.a], k[(size_t)x.b]);
                        asg[(size_t)x.a] = 1;
                        break;
                    default: break;
                }
            };
            int newDepth = x.op == OP_SET_LOCAL || x.op == OP_INIT_LOCAL ? d - 1
                         : x.op == OP_JUMP_IF_FALSE ? d - 1
                         : (x.op == OP_ADD || x.op == OP_SUB || x.op == OP_MUL || x.op == OP_EQ ||
                            x.op == OP_NE || x.op == OP_LT || x.op == OP_GT || x.op == OP_LE ||
                            x.op == OP_GE) ? d - 1
                         : (int)vk.size();
            if ((int)vk.size() != newDepth) return jitFail(__LINE__);   // codegen safety net
            if (!jumpOnly && to < n) {
                std::vector<JitKindSet> nk = kindAt[i];
                std::vector<uint8_t> na = assignedAt[i];
                applyLocals(nk, na);
                std::vector<JitKindSet> nv = vk;
                if (depth[to] == kUnset) {
                    depth[to] = newDepth;
                    kindAt[to] = nk;
                    assignedAt[to] = na;
                    vrKind[to] = nv;
                    changed = true;
                } else if (depth[to] != newDepth) {
                    return jitFail(__LINE__);                        // inconsistent stack: bail out
                } else {
                    for (size_t s = 0; s < nk.size(); s++) {
                        JitKindSet m = meet(kindAt[to][s], nk[s]);
                        if (m != kindAt[to][s]) { kindAt[to][s] = m; changed = true; }
                        uint8_t a2 = assignedAt[to][s] && na[s];
                        if (a2 != assignedAt[to][s]) { assignedAt[to][s] = a2; changed = true; }
                    }
                    for (size_t s = 0; s < nv.size() && s < vrKind[to].size(); s++) {
                        JitKindSet m = meet(vrKind[to][s], nv[s]);
                        if (m != vrKind[to][s]) { vrKind[to][s] = m; changed = true; }
                    }
                }
            }
            bool isJump = x.op == OP_JUMP || x.op == OP_LOOP || x.op == OP_JUMP_IF_FALSE ||
                          x.op == OP_JUMP_IF_NOT_LT_LOCAL_LOCAL ||
                          x.op == OP_JUMP_IF_NOT_LT_LOCAL_IMM;
            if (isJump && x.target >= 0 && (size_t)x.target <= code.size()) {
                auto it = indexOf.find((size_t)x.target);
                if (it == indexOf.end() && (size_t)x.target != code.size()) return jitFail(__LINE__);
                if ((size_t)x.target < code.size()) {
                    size_t ti = it->second;
                    std::vector<JitKindSet> nk = kindAt[i];
                    std::vector<uint8_t> na = assignedAt[i];
                    applyLocals(nk, na);
                    if (depth[ti] == kUnset) {
                        depth[ti] = newDepth;
                        kindAt[ti] = nk;
                        assignedAt[ti] = na;
                        vrKind[ti] = vk;
                        changed = true;
                    } else if (depth[ti] != newDepth) {
                        return jitFail(__LINE__);
                    } else {
                        for (size_t s = 0; s < nk.size(); s++) {
                            JitKindSet m = meet(kindAt[ti][s], nk[s]);
                            if (m != kindAt[ti][s]) { kindAt[ti][s] = m; changed = true; }
                            uint8_t a2 = assignedAt[ti][s] && na[s];
                            if (a2 != assignedAt[ti][s]) { assignedAt[ti][s] = a2; changed = true; }
                        }
                    }
                }
            }
        }
    }
    if (maxDepth > 48) return jitFail(__LINE__);

    // ---- which locals must be plain ints when the native code starts?
    std::vector<uint8_t> readSlots(ch->numLocals, 0);
    auto markRead = [&](size_t i, int slot) {
        if (slot < 0 || (size_t)slot >= ch->numLocals) return;
        // A slot is safe without a runtime check when it was definitely assigned an integer
        // before this read (the assignment produced the value), otherwise the caller must hand
        // us an int - e.g. a parameter that is only compared.
        if (assignedAt[i][(size_t)slot] && (kindAt[i][(size_t)slot] & ~kJitAllInts) == 0) return;
        readSlots[(size_t)slot] = 1;
    };
    for (size_t i = 0; i < n; i++) {
        if (depth[i] == kUnset) continue;
        const JitIns& x = ins[i];
        switch (x.op) {
            case OP_GET_LOCAL: markRead(i, x.a); break;
            case OP_LOCAL_ADD_IMM: case OP_LOCAL_SUB_IMM: markRead(i, x.a); break;
            case OP_LOCAL_ADD_LOCAL: markRead(i, x.a); markRead(i, x.b); break;
            case OP_JUMP_IF_NOT_LT_LOCAL_LOCAL: markRead(i, x.a); markRead(i, x.b); break;
            case OP_JUMP_IF_NOT_LT_LOCAL_IMM: markRead(i, x.a); break;
            default: break;
        }
    }

    // ---- locals that are returned directly keep a kind byte: the interpreter's `typeof` for
    //      them depends on which assignment ran last, so the native code records it at runtime
    std::vector<uint8_t> tracked(ch->numLocals, 0);
    for (size_t i = 0; i < n; i++) {
        if (ins[i].op != OP_RETURN || i == 0) continue;
        if (ins[i - 1].op == OP_GET_LOCAL) tracked[(size_t)ins[i - 1].a] = 1;
    }
    if (ch->isMethod && ch->numLocals > 0) tracked[0] = 0;

    // every assignment to a tracked local must have exactly one possible kind
    for (size_t i = 0; i < n; i++) {
        if (depth[i] == kUnset) continue;
        const JitIns& x = ins[i];
        bool writes = x.op == OP_SET_LOCAL || x.op == OP_INIT_LOCAL ||
                      x.op == OP_LOCAL_ADD_IMM || x.op == OP_LOCAL_SUB_IMM ||
                      x.op == OP_LOCAL_ADD_LOCAL;
        if (!writes || !tracked[(size_t)x.a]) continue;
        if (x.op == OP_LOCAL_ADD_IMM || x.op == OP_LOCAL_SUB_IMM) continue;  // kind is preserved
        JitKindSet assigned = x.op == OP_LOCAL_ADD_LOCAL
                                  ? jitPromoteSet(kindAt[i][(size_t)x.a], kindAt[i][(size_t)x.b])
                                  : vrKind[i][(size_t)depth[i] - 1];
        if (jitSoleKind(assigned) < 0) return jitFail(__LINE__);
    }

    // ---- emit
    JitEmitter e;
    size_t kindSlots = 0;
    for (size_t i = 0; i < ch->numLocals; i++) if (tracked[i]) kindSlots++;
    e.kindBase = (size_t)std::max(1, maxDepth + 1) * 8;
    e.frameBytes = (size_t)std::max(16, ((int)(e.kindBase + kindSlots + 15)) / 16 * 16);
    e.u8(0x48); e.u8(0x81); e.u8(0xEC); e.u32((uint32_t)e.frameBytes);      // sub rsp, frameBytes
    for (size_t i = 0; i < n; i++) {
        const JitIns& x = ins[i];
        e.labelAt[x.at] = e.c.size();
        if (depth[i] == kUnset) { e.nullReturn(); continue; }
        int d = depth[i];
        switch (x.op) {
            case OP_NOP: break;
            case OP_INT1:
                e.movRaxImm(x.imm);
                e.storeRaxToVr(d);
                break;
            case OP_GET_LOCAL:
                e.loadLocalToRax(x.a);
                e.storeRaxToVr(d);
                break;
            case OP_SET_LOCAL: case OP_INIT_LOCAL:
                e.loadVrToRax(d - 1);
                e.storeRaxToLocal(x.a);
                if (tracked[(size_t)x.a]) {
                    int sole = jitSoleKind(vrKind[i][(size_t)d - 1]);
                    if (sole < 0) return jitFail(__LINE__);
                    e.storeKindByte(e.kindBase + (size_t)x.a,
                                    sole == 15 ? 2 : sole == (int)NumKind::I64 ? 1 : 0);
                }
                break;
            case OP_LOCAL_ADD_IMM:
            case OP_LOCAL_SUB_IMM:
                e.loadLocalToRax(x.a);
                e.movR8Imm(x.imm);
                if (x.op == OP_LOCAL_ADD_IMM) e.addRaxR8(); else e.subRaxR8();
                e.storeRaxToLocal(x.a);
                break;                                  // x = x + k preserves the kind byte
            case OP_LOCAL_ADD_LOCAL:
                e.loadLocalToRax(x.a);
                e.loadLocalToR8(x.b);
                e.addRaxR8();
                e.storeRaxToLocal(x.a);
                if (tracked[(size_t)x.a]) {
                    int sole = jitSoleKind(jitPromoteSet(kindAt[i][(size_t)x.a],
                                                         kindAt[i][(size_t)x.b]));
                    if (sole < 0) return jitFail(__LINE__);
                    e.storeKindByte(e.kindBase + (size_t)x.a,
                                    sole == 15 ? 2 : sole == (int)NumKind::I64 ? 1 : 0);
                }
                break;
            case OP_ADD: case OP_SUB: case OP_MUL:
                e.loadVrToRax(d - 2);
                e.loadVrToR8(d - 1);
                if (x.op == OP_ADD) e.addRaxR8();
                else if (x.op == OP_SUB) e.subRaxR8();
                else e.imulRaxR8();
                e.storeRaxToVr(d - 2);
                break;
            case OP_EQ: case OP_NE: case OP_LT: case OP_GT: case OP_LE: case OP_GE: {
                e.loadVrToRax(d - 2);
                e.loadVrToR8(d - 1);
                e.cmpRaxR8();
                uint8_t cc = x.op == OP_EQ ? 0x94 : x.op == OP_NE ? 0x95 : x.op == OP_LT ? 0x9C
                           : x.op == OP_GT ? 0x9F : x.op == OP_LE ? 0x9E : 0x9D;
                e.setcc(cc);
                e.storeRaxToVr(d - 2);
                break;
            }
            case OP_JUMP: case OP_LOOP:
                e.jmpPlaceholder((size_t)x.target);
                break;
            case OP_JUMP_IF_FALSE:
                e.loadVrToRax(d - 1);
                e.testRaxRax();
                e.jccPlaceholder(0x84, (size_t)x.target);            // jz
                break;
            case OP_JUMP_IF_NOT_LT_LOCAL_LOCAL:
                e.loadLocalToRax(x.a);
                e.cmpRaxWithLocal(x.b);
                e.jccPlaceholder(0x8D, (size_t)x.target);            // jge (not less)
                break;
            case OP_JUMP_IF_NOT_LT_LOCAL_IMM:
                e.loadLocalToRax(x.a);
                e.cmpRaxImm(x.imm);
                e.jccPlaceholder(0x8D, (size_t)x.target);
                break;
            case OP_RETURN: {
                e.loadVrToRax(d - 1);
                int sole = jitSoleKind(vrKind[i][(size_t)d - 1]);
                if (i > 0 && ins[i - 1].op == OP_GET_LOCAL &&
                    tracked[(size_t)ins[i - 1].a]) {
                    // the value came straight from a tracked local: its kind byte is exact
                    e.loadKindByteToR8(e.kindBase + (size_t)ins[i - 1].a);
                    e.storeResultWithKindInR8();
                } else if (sole >= 0) {
                    e.storeResult(sole == 15 ? 2 : sole == (int)NumKind::I64 ? 1 : 0);
                } else {
                    return jitFail(__LINE__);                 // kind would not match the interpreter
                }
                e.epilogue();
                break;
            }
            case OP_RETURN_NULL:
                e.nullReturn();
                break;
            default:
                return jitFail(__LINE__);
        }
    }
    e.labelAt[code.size()] = e.c.size();
    e.nullReturn();

    if (e.pending.size() > 0 && !e.finish()) return jitFail(__LINE__);
    if (e.c.size() > 60000) return jitFail(__LINE__);

    if (std::getenv("ANNOTA_JIT_DEBUG"))
        std::fprintf(stderr, "[jit] compiled %s chunk=%p bytes=%zu\n", ch->fnName.c_str(),
                     (void*)ch.get(), e.c.size());
    // Hot loops can be entered directly: for every backward jump target with an empty operand
    // stack the interpreter may switch to this native code mid-function.  A trampoline runs the
    // prologue (so the epilogue stays balanced) and jumps to the loop header.
    std::vector<std::pair<size_t, size_t>> tramp;      // (target ip, patch position)
    for (auto& kv : e.labelAt) {
        size_t ip = kv.first;
        if (ip >= code.size()) continue;
        auto di = indexOf.find(ip);
        if (di == indexOf.end()) continue;
        bool isLoopHeader = false;
        for (auto& x : ins) {
            bool backward = (x.op == OP_LOOP || x.op == OP_JUMP) && x.target >= 0 &&
                            (size_t)x.target < x.at;
            if (backward && (size_t)x.target == ip) { isLoopHeader = true; break; }
        }
        if (!isLoopHeader) continue;
        if (depth[di->second] != 0) continue;            // only enter with an empty operand stack
        size_t off = e.c.size();
        e.u8(0x48); e.u8(0x81); e.u8(0xEC); e.u32((uint32_t)e.frameBytes);   // sub rsp, frame
        e.u8(0xE9);
        int64_t rel = (int64_t)e.labelAt[ip] - (int64_t)(e.c.size() + 4);
        e.u32((uint32_t)(int32_t)rel);                                       // jmp loop header
        tramp.push_back({ip, off});
    }

    auto jc = std::make_shared<JitCode>();
    jc->bytes = e.c;
    void* mem = jitAllocExec(jc->bytes.size());
    if (!mem) return jitFail(__LINE__);
    std::memcpy(mem, jc->bytes.data(), jc->bytes.size());
    jc->mapping = mem;
    jc->mappingSize = jc->bytes.size();
    jc->fn = reinterpret_cast<JitFn>(mem);
    for (auto& t : tramp)
        jc->osrEntries[t.first] = reinterpret_cast<JitFn>((uint8_t*)mem + t.second);
    for (size_t s = 0; s < readSlots.size(); s++)
        if (readSlots[s]) jc->readSlots.push_back((uint8_t)s);
    return jc;
}

} // namespace annota
