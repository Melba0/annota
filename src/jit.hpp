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

// Machine code cannot raise an Annota error by itself, so the two conditions it can still detect
// (an out-of-range array index, a failed allocation) are reported through `JitOut::kind`:
// 3 = null, 4 = runtime error to be raised by the VM after the native call returns.
constexpr int64_t kJitOutError = 4;

// Backing store for arrays created inside compiled code.  The code calls this function while it
// runs; the VM frees everything it handed out once the outermost native invocation returns.
int64_t* annotaJitArrayAlloc(int64_t bytes);   // implemented by the VM
void jitArenaReset();

// The native code writes the returned value here.  `kind`: 0 = int (untyped), 1 = int64,
// 2 = bool, 3 = null - the same Value the interpreter would have produced.
struct JitOut {
    int64_t value = 0;
    int64_t kind = 3;
};

struct JitCode {
    std::vector<uint8_t> bytes;
    std::vector<uint8_t> readSlots;      // locals that must hold an int when the native code runs
    std::vector<uint8_t> slotKind;       // per local: the exact NumKind+1 required, 0 = don't care
    // loop headers that can be entered directly (hot-loop promotion): bytecode ip -> entry
    std::map<size_t, JitFn> osrEntries;
    JitFn fn = nullptr;
    // kind byte every `return` writes (0 int, 1 int64, 2 bool, 3 null); -2 when it varies, which
    // makes the function unusable as a direct call target from other native code
    int retByte = -2;
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
        u8(0xC6); u8(0x84); u8(0x24); u32((uint32_t)byteOff); u8((uint8_t)kind);   // mov byte [rsp+off], imm8
    }
    void loadKindByteToR8(size_t byteOff) {
        u8(0x44); u8(0x0F); u8(0xB6); u8(0x84); u8(0x24);
        u32((uint32_t)byteOff);                                                    // movzx r8d,[rsp+off]
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
    // The machine code keeps its own copy of the locals: the interpreter's L[] is the source at
    // entry, and kBase is repointed at the private area, which is what later lets a slot hold a
    // value in its own width (or, for a 128 bit value, two consecutive slots) instead of the exact
    // 64 bit form the interpreter would see.
    void localsPrologue(int numLocals, size_t localsBase) {
        for (int i = 0; i < numLocals; i++) {
            loadLocalToRax(i);                                  // rax = L[i]
            u8(0x48); u8(0x89); u8(0x84); u8(0x24);
            u32((uint32_t)(localsBase + (size_t)i * 8));        // mov [rsp+base+i*8], rax
        }
        u8(0x48); u8(0x8D);
        u8(kBase == 0x01 ? 0x8C : 0xBC); u8(0x24);
        u32((uint32_t)localsBase);                              // lea rcx/rdi, [rsp+localsBase]
    }
    void loadLocalToEax(int slot) {                 // 32 bit: the write zeroes the upper half
        u8(0x8B);
        if (slot * 8 < 128) { u8(0x40 | kBase); u8((uint8_t)(slot * 8)); }
        else { u8(0x80 | kBase); u32((uint32_t)(slot * 8)); }
    }
    void addEaxLocal(int slot) {                    // add eax, [kBase+slot*8]
        u8(0x03);
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
        u8(0x48); u8(0x89);
        if (vr * 8 < 128) { u8(0x44); u8(0x24); u8((uint8_t)(vr * 8)); }
        else { u8(0x84); u8(0x24); u32((uint32_t)(vr * 8)); }
    }
    void loadVrToRax(int vr) {
        u8(0x48); u8(0x8B);
        if (vr * 8 < 128) { u8(0x44); u8(0x24); u8((uint8_t)(vr * 8)); }
        else { u8(0x84); u8(0x24); u32((uint32_t)(vr * 8)); }
    }
    void loadVrToR8(int vr) {
        u8(0x4C); u8(0x8B);
        if (vr * 8 < 128) { u8(0x44); u8(0x24); u8((uint8_t)(vr * 8)); }
        else { u8(0x84); u8(0x24); u32((uint32_t)(vr * 8)); }
    }

    void movRaxImm(int32_t v) { u8(0x48); u8(0xC7); u8(0xC0); u32((uint32_t)v); }
    void movRaxImm64(uint64_t v) { u8(0x48); u8(0xB8); for (int b = 0; b < 8; b++) u8((uint8_t)((v >> (8 * b)) & 0xff)); }
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
    // narrow the result of an arithmetic op back to the declared width, the way the interpreter's
    // per-kind wrapping does (the JIT only ever computes in 64 bit registers)
    void wrapRax(int kind) {
        switch ((NumKind)kind) {
            case NumKind::I8:  u8(0x48); u8(0x0F); u8(0xBE); u8(0xC0); break;   // movsx rax, al
            case NumKind::I16: u8(0x48); u8(0x0F); u8(0xBF); u8(0xC0); break;   // movsx rax, ax
            case NumKind::I32: u8(0x48); u8(0x63); u8(0xC0); break;             // movsxd rax, eax
            case NumKind::U8:  u8(0x0F); u8(0xB6); u8(0xC0); break;             // movzx eax, al
            case NumKind::U16: u8(0x0F); u8(0xB7); u8(0xC0); break;             // movzx eax, ax
            case NumKind::U32: u8(0x89); u8(0xC0); break;                       // mov eax, eax
            default: break;                                                     // 64 bit: already exact
        }
    }

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
    void loadVrToR11(int vr) {
        u8(0x4C); u8(0x8B);
        if (vr * 8 < 128) { u8(0x5C); u8(0x24); u8((uint8_t)(vr * 8)); }
        else { u8(0x9C); u8(0x24); u32((uint32_t)(vr * 8)); }
    }
    void storeR11ToVr(int vr) {
        u8(0x4C); u8(0x89);
        if (vr * 8 < 128) { u8(0x5C); u8(0x24); u8((uint8_t)(vr * 8)); }
        else { u8(0x9C); u8(0x24); u32((uint32_t)(vr * 8)); }
    }
    // a short conditional jump whose displacement is filled in once the target offset is known
    size_t jccRel8(uint8_t cc) { u8(cc); size_t p = c.size(); u8(0); return p; }
    size_t jmpRel8() { u8(0xEB); size_t p = c.size(); u8(0); return p; }   // short jmp, patched later
    void patchRel8(size_t at, size_t target) {
        c[at] = (uint8_t)(int8_t)((int)target - (int)(at + 1));
    }
    // Machine code cannot raise an Annota error itself: it reports one through JitOut and returns,
    // and the VM raises it once the native call is over.
    void errorReturn() { movRaxImm(0); storeResult((int)kJitOutError); epilogue(); }
    // Call a C helper.  `setArgs` fills the first integer argument (rcx/rdi) - it runs inside the
    // call frame, so it must not read virtual registers by stack offset.  The two registers the
    // backend keeps live (kBase, kOut) are saved around the call, and 32 bytes of shadow space are
    // reserved because the callee is ordinary C.
    void callC(uint64_t addr, const std::function<void()>& setArgs) {
        const int savedBase = 32, savedOut = 40;
        int N = 48;
        if ((N % 16) != 8) N += 8 - (N % 16);
        u8(0x48); u8(0x81); u8(0xEC); u32((uint32_t)N);                       // sub rsp, N
        u8(0x48); u8(0x89); u8(kBase == 0x01 ? 0x8C : 0xBC);
        u8(0x24); u32((uint32_t)savedBase);                                   // save kBase
        u8(0x48); u8(0x89); u8(kOut == 0x02 ? 0x94 : 0xB4);
        u8(0x24); u32((uint32_t)savedOut);                                    // save kOut
        setArgs();
        u8(0x49); u8(0xBB);
        for (int b = 0; b < 8; b++) u8((uint8_t)((addr >> (8 * b)) & 0xff));  // mov r11, addr
        u8(0x41); u8(0xFF); u8(0xD3);                                         // call r11
        u8(0x48); u8(0x8B); u8(kBase == 0x01 ? 0x8C : 0xBC);
        u8(0x24); u32((uint32_t)savedBase);                                   // restore kBase
        u8(0x48); u8(0x8B); u8(kOut == 0x02 ? 0x94 : 0xB4);
        u8(0x24); u32((uint32_t)savedOut);                                    // restore kOut
        u8(0x48); u8(0x81); u8(0xC4); u32((uint32_t)N);                       // add rsp, N
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
inline std::shared_ptr<JitCode> jitCompileX64(const std::shared_ptr<Chunk>& ch,
                                              const JitResolver& resolve = {}) {
    if (!ch) return jitFail(__LINE__);
    const std::vector<uint8_t>& code = ch->code;
    if (ch->numLocals > 64 || code.empty()) return jitFail(__LINE__);
    if (std::getenv("ANNOTA_NO_JIT")) return jitFail(__LINE__);
    if (ch->jitBusy) return jitFail(__LINE__);          // breaks cycles while resolving callees
    struct BusyGuard {
        const std::shared_ptr<Chunk>& c;
        explicit BusyGuard(const std::shared_ptr<Chunk>& x) : c(x) { c->jitBusy = true; }
        ~BusyGuard() { c->jitBusy = false; }
    } busyGuard(ch);

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
            case OP_GET_INDEX: case OP_SET_INDEX:
                x.next = ip + 1; break;
            case OP_NEW_ARRAY:
                x.next = ip + 3; x.a = code[ip + 1]; x.b = code[ip + 2]; break;
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
            case OP_CONST: x.next = ip + 3; x.a = (code[ip + 1] << 8) | code[ip + 2]; break;
            case OP_CONVERT: x.next = ip + 2; x.imm = code[ip + 1]; break;
            case OP_CALL_DIRECT:
                x.next = ip + 4;
                x.a = (code[ip + 1] << 8) | code[ip + 2];        // name constant
                x.imm = code[ip + 3];                            // argument count
                break;
            default: return jitFail(__LINE__);                       // not translatable: keep interpreting

        }
        if (x.next > code.size()) return jitFail(__LINE__);
        if (std::getenv("ANNOTA_JIT_DEBUG"))
            std::fprintf(stderr, "[jit]   ip %zu op %d next %zu\n", x.at, (int)x.op, x.next);
        indexOf[x.at] = ins.size();
        ins.push_back(x);
        ip = x.next;
    }
    // ---- arrays: only a 1-D numeric array that this chunk creates itself, stores straight into a
    // local and uses exclusively as `a[simple index]` may be compiled.  The machine code keeps such
    // an array in a private raw-int buffer (allocated while it runs, freed when the call returns),
    // so nothing else may ever see it; every other use stays interpreted.
    std::vector<uint8_t> jitArrayLocal(ch->numLocals, 0);
    std::vector<NumKind> jitArrayKind(ch->numLocals, NumKind::None);
    bool hasJitArrays = false;
    // The array support below is new and still has an open wrong-value case (returning a literal
    // index load), so it stays off unless ANNOTA_JIT_ARRAYS=1 asks for it.
    static const bool arraysEnabled = std::getenv("ANNOTA_JIT_ARRAYS") != nullptr;
    for (size_t i = 0; i < ins.size(); i++) {
        if (ins[i].op != OP_NEW_ARRAY) continue;
        uint8_t nd = (uint8_t)ins[i].a, info = (uint8_t)ins[i].b;
        int kc = info >> 3;
        NumKind ek = kc > 0 ? (NumKind)(kc - 1) : NumKind::None;
        bool plainInt = ek == NumKind::None || ek == NumKind::I8 || ek == NumKind::I16 ||
                         ek == NumKind::I32 || ek == NumKind::I64 || ek == NumKind::U8 ||
                         ek == NumKind::U16 || ek == NumKind::U32 || ek == NumKind::U64;
        if (nd != 1 || (info & 3) != 0 || !plainInt) return jitFail(__LINE__);
        if (i + 1 >= ins.size() ||
            (ins[i + 1].op != OP_SET_LOCAL && ins[i + 1].op != OP_INIT_LOCAL))
            return jitFail(__LINE__);                  // must go straight into a local
        jitArrayKind[ins[i + 1].a] = ek;
        jitArrayLocal[ins[i + 1].a] = 1;
        hasJitArrays = true;
    }
    if (hasJitArrays) {
    if (hasJitArrays && !arraysEnabled) return jitFail(__LINE__);
        for (size_t i = 0; i < ins.size(); i++)
            if ((ins[i].op == OP_SET_LOCAL || ins[i].op == OP_INIT_LOCAL) && jitArrayLocal[ins[i].a] &&
                !(i > 0 && ins[i - 1].op == OP_NEW_ARRAY))
                return jitFail(__LINE__);              // overwritten with something else
        for (size_t i = 0; i < ins.size(); i++)
            if ((ins[i].op == OP_LOCAL_ADD_IMM || ins[i].op == OP_LOCAL_SUB_IMM ||
                 ins[i].op == OP_LOCAL_ADD_LOCAL || ins[i].op == OP_LEND) &&
                ((ins[i].a < jitArrayLocal.size() && jitArrayLocal[ins[i].a]) ||
                 (ins[i].b < jitArrayLocal.size() && jitArrayLocal[ins[i].b])))
                return jitFail(__LINE__);
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

    auto resolveCall = [&](const JitIns& x) -> JitCallTarget {
        JitCallTarget none;
        if (!resolve || (size_t)x.a >= ch->consts.size() || ch->consts[(size_t)x.a].t != VT::Str)
            return none;
        const std::string nm = ch->consts[(size_t)x.a].o->str;
        JitCallTarget t = resolve(nm);
        if (std::getenv("ANNOTA_JIT_DEBUG") && (!t.code || !t.chunk || !t.code->fn))
            std::fprintf(stderr, "[jit] call target %s not compiled\n", nm.c_str());
        if (!t.code || !t.chunk || !t.code->fn) return none;
        if (std::getenv("ANNOTA_JIT_DEBUG") && (t.code->retByte < 0 || t.code->retByte > 2))
            std::fprintf(stderr, "[jit] call target %s retByte %d\n", nm.c_str(), t.code->retByte);
        if (t.code->retByte < 0 || (t.code->retByte > 2 && t.code->retByte != 5)) return none;
        if (std::getenv("ANNOTA_JIT_DEBUG") && (t.chunk->isMethod || (int)t.chunk->params.size() != x.imm))
            std::fprintf(stderr, "[jit] call target %s arity %zu vs %d (method %d)\n", nm.c_str(), t.chunk->params.size(), x.imm, (int)t.chunk->isMethod);
        if (t.chunk->isMethod || (int)t.chunk->params.size() != x.imm) return none;
        if (t.chunk->numLocals > 64) return none;
        return t;
    };
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
    // a parameter with an integer type hint has an exact kind, which is what makes the values
    // (and the result kind) statically known - `[[jit]]` requires those hints on every parameter
    std::vector<uint8_t> expectKind(ch->numLocals, 0);
    for (size_t i = 0; i < ch->params.size() && i < 64; i++) {
        NumKind k = numKindByName(ch->params[i].type);
        bool integer = k == NumKind::I8 || k == NumKind::I16 || k == NumKind::I32 ||
                       k == NumKind::I64 || k == NumKind::U8 || k == NumKind::U16 ||
                       k == NumKind::U32 || k == NumKind::U64;
        if (!integer) continue;
        size_t slot = (ch->isMethod ? 1 : 0) + i;
        if (slot >= ch->numLocals) continue;
        kindAt[0][slot] = jitBit(k);
        expectKind[slot] = (uint8_t)k + 1;
    }

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
                case OP_NEW_ARRAY:
                    // 1-D numeric array: the dimension on the stack becomes a raw buffer pointer
                    if (d < (int)x.a) return jitFail(__LINE__);
                    for (int k = 0; k < (int)x.a; k++) vk.pop_back();
                    pushKind(jitBit(NumKind::I64));
                    break;
                case OP_GET_INDEX: {
                    if (d < 2) return jitFail(__LINE__);
                    int gObj = d - 2;
                    int gEk = -1;
                    for (size_t k = i; k-- > 0;) {
                        if (depth[k] != gObj) continue;
                        if (ins[k].op == OP_GET_LOCAL && jitArrayLocal[ins[k].a])
                            gEk = (int)jitArrayKind[ins[k].a];
                        break;
                    }
                    if (gEk < 0) return jitFail(__LINE__);      // only JIT arrays may be indexed
                    vk.pop_back(); vk.pop_back();
                    pushKind(jitBit((NumKind)gEk));             // the element kind is exact
                    break;
                }
                case OP_SET_INDEX:
                    if (d < 3) return jitFail(__LINE__);
                    vk.pop_back(); vk.pop_back(); vk.pop_back();
                    break;
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
                case OP_CONST: {
                    // pool constants that are plain 64 bit integers; strings, floats, containers and
                    // the 128 bit widths stay interpreted
                    if ((size_t)x.a >= ch->consts.size()) return jitFail(__LINE__);
                    const Value& c0 = ch->consts[(size_t)x.a];
                    if (c0.t != VT::Int || c0.o || (int)c0.k > (int)NumKind::U64) return jitFail(__LINE__);
                    pushKind(jitBit(c0.k));
                    break;
                }
                case OP_CONVERT: {
                    // the compiler converts every typed parameter at entry; the JIT's entry check
                    // already requires exactly that kind, so the conversion is the identity here -
                    // any other conversion (a real `int(x)`, a boxed width) stays interpreted
                    if (d < 1 || x.imm > (int)NumKind::U64) return jitFail(__LINE__);
                    int sole = jitSoleKind(vk.back());
                    // the identity (the entry check already guarantees a typed parameter's kind), or an
                    // untyped integer being given a declared width - which is a wrap and nothing else
                    if (sole < 0) return jitFail(__LINE__);
                    if (sole != x.imm && sole != (int)NumKind::None) return jitFail(__LINE__);
                    vk.back() = jitBit((NumKind)x.imm);
                    break;
                }
                case OP_CALL_DIRECT: {
                    JitCallTarget t = resolveCall(x);
                    if (!t.code || (int)vk.size() < x.imm) return jitFail(__LINE__);
                    // the callee accepts only what its entry check allows: untyped or int64
                    const JitKindSet kArgOk = jitBit(NumKind::None) | jitBit(NumKind::I64);
                    for (int k = 0; k < x.imm; k++) {
                        JitKindSet ks = vk[(size_t)((int)vk.size() - x.imm + k)];
                        if (ks == 0 || (ks & ~kArgOk) != 0) return jitFail(__LINE__);
                    }
                    for (int k = 0; k < x.imm; k++) vk.pop_back();
                    pushKind(t.code->retByte == 2 ? kJitBool
                             : t.code->retByte == 5 ? jitBit(NumKind::U64)
                             : t.code->retByte == 1 ? jitBit(NumKind::I64) : kJitNone);
                }
                case OP_RETURN:
                    if (d < 1) return jitFail(__LINE__);
                    break;                                   // exact kind handled at emit time
                case OP_RETURN_NULL: break;
                default:
                    if (std::getenv("ANNOTA_JIT_DEBUG"))
                        std::fprintf(stderr, "[jit] analyze: unsupported op %d at ip %zu in %s\n",
                                     (int)x.op, x.at, ch->fnName.c_str());
                    return jitFail(__LINE__);
            }
            maxDepth = std::max(maxDepth, (int)vk.size());
            // propagate to the fallthrough
            bool noFall = (x.op == OP_RETURN || x.op == OP_RETURN_NULL);   // returns do not fall through
            bool jumpOnly = (x.op == OP_JUMP || x.op == OP_LOOP);
            size_t to = i + 1;
            if (noFall) to = n;                                         // nothing after a return is reachable
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
    // ---- every use of a JIT array must be an index access on the array itself: the value pushed by
    // `get_local a` has to still be on the virtual stack (at the exact depth the index op expects)
    // when `get_index`/`set_index` runs, so nothing can have consumed or copied it in between.
    if (hasJitArrays) {
        for (size_t k = 0; k < ins.size(); k++) {
            if (ins[k].op != OP_GET_LOCAL || !jitArrayLocal[ins[k].a] || depth[k] == kUnset) continue;
            bool ok = false;
            for (size_t j = k + 1; j < ins.size(); j++) {
                if (depth[j] != kUnset && depth[j] < depth[k] + 1) break;     // the array was used up
                if (ins[j].op == OP_GET_INDEX && depth[j] == depth[k] + 2) { ok = true; break; }
                if (ins[j].op == OP_SET_INDEX && depth[j] == depth[k] + 3) { ok = true; break; }
            }
            if (!ok) return jitFail(__LINE__);                               // the array escapes
        }
    }

    // a kind set that is not a single kind may only contain the 64 bit ones: wrapping a narrowed
    // value is only correct when the width is unambiguous
    for (size_t i = 0; i < n; i++) {
        if (depth[i] == kUnset) continue;
        for (size_t s = 0; s < ch->numLocals; s++) {
            JitKindSet ks = kindAt[i][s];
            // `kJitAllInts` means "an integer of unknown width": the entry check narrows it to the
            // 64 bit / untyped kinds, which do not wrap, so raw int64 work is exact.  Any *other*
            // set that mixes in a narrow width has no single wrapping rule and stays interpreted.
            if (ks == kJitAllInts || jitSoleKind(ks) >= 0) continue;
            if (ks & (jitBit(NumKind::I8) | jitBit(NumKind::I16) | jitBit(NumKind::I32) |
                      jitBit(NumKind::U8) | jitBit(NumKind::U16) | jitBit(NumKind::U32)))
                return jitFail(__LINE__);
        }
        for (auto& ks : vrKind[i]) {
            if (ks == kJitAllInts || jitSoleKind(ks) >= 0) continue;
            if (ks & (jitBit(NumKind::I8) | jitBit(NumKind::I16) | jitBit(NumKind::I32) |
                      jitBit(NumKind::U8) | jitBit(NumKind::U16) | jitBit(NumKind::U32)))
                return jitFail(__LINE__);
        }
    }

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

    for (size_t s = 0; s < ch->numLocals; s++) if (expectKind[s]) readSlots[s] = 1;

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
    int seenRetByte = -1;                                  // -2 once two returns disagree
    auto noteRetByte = [&](int b) {
        if (seenRetByte == -1) seenRetByte = b;
        else if (seenRetByte != b) seenRetByte = -2;
    };
    JitEmitter e;
    // one kind byte per local slot (they are addressed by slot number, so the area must cover
    // every slot, not just the tracked ones)
    size_t kindSlots = (size_t)ch->numLocals;
    e.kindBase = (size_t)std::max(1, maxDepth + 1) * 8;
    e.frameBytes = (size_t)std::max(16, ((int)(e.kindBase + kindSlots + 15)) / 16 * 16);
    // the locals get their own area inside the frame, above the kind bytes; offsets of the kind
    // bytes and the virtual registers are unchanged, only the total frame grows
    const size_t localsBase = e.frameBytes;
    const size_t localsBytes = ((size_t)ch->numLocals * 8 + 15) / 16 * 16;
    e.frameBytes += localsBytes;
    e.u8(0x48); e.u8(0x81); e.u8(0xEC); e.u32((uint32_t)e.frameBytes);      // sub rsp, frameBytes
    e.localsPrologue((int)ch->numLocals, localsBase);                        // private copy of L[]
    // The analysis proved that the only instruction which can have produced the array in a virtual
    // register is the `get_local` of a JIT array local: find it and report its element kind.
    // Comparisons must use the condition codes of the operands' signedness: an untyped or signed
    // width compares signed, an unsigned width compares unsigned, and a set that mixes both is
    // refused (the interpreter would promote it in a way the machine code cannot express here).
    // Is a comparison of these two kind sets unsigned?  When both operands have a single known kind
    // the language's own promotion decides (so `uint32 < uint32` compares unsigned, `int64 < uint32`
    // does not); an unknown width falls back to signed, which is what the interpreter does for
    // untyped integers and what this backend did before.  Signedness of the *result* kind is read
    // from numTraits, so there is no second copy of the language's rules here.
    // The kind byte a value carries out of compiled code, shared by every place that writes one:
    // 0 = int, 1 = int64, 2 = bool, 5 = uint64 (the bits are the value, but the interpreter must
    // build the right Value from it - a uint64 result of -4 bits prints as 18446744073709551612).
    auto retCodeOf = [](int sole) -> int {
        return sole == 15 ? 2 : sole == (int)NumKind::I64 ? 1 : sole == (int)NumKind::U64 ? 5 : 0;
    };
    auto jitUnsignedCmp = [](JitKindSet a, JitKindSet b) -> int {
        int ka = jitSoleKind(a), kb = jitSoleKind(b);
        if (ka < 0 || kb < 0 || ka > (int)NumKind::U64 || kb > (int)NumKind::U64) return 0;
        return numTraits(promoteNum((NumKind)ka, (NumKind)kb)).isSigned ? 0 : 1;
    };
    auto jitArrayKindOf = [&](size_t at, int objVr) -> int {
        for (size_t k = at; k-- > 0;) {
            if (depth[k] != objVr) continue;
            if (ins[k].op == OP_GET_LOCAL && jitArrayLocal[ins[k].a]) return (int)jitArrayKind[ins[k].a];
            return -1;
        }
        return -1;
    };

    for (size_t i = 0; i < n; i++) {
        const JitIns& x = ins[i];
        e.labelAt[x.at] = e.c.size();
        if (depth[i] == kUnset) {
            if (std::getenv("ANNOTA_JIT_DEBUG"))
                std::fprintf(stderr, "[jit]   unreachable ip %zu op %d in %s\n", x.at, (int)x.op, ch->fnName.c_str());
            e.nullReturn(); continue; }
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
                                    retCodeOf(sole));
                }
                break;
            case OP_LOCAL_ADD_IMM:
            case OP_LOCAL_SUB_IMM:
                e.loadLocalToRax(x.a);
                e.movR8Imm(x.imm);
                if (x.op == OP_LOCAL_ADD_IMM) e.addRaxR8(); else e.subRaxR8();
                { int wsole = jitSoleKind(kindAt[i][(size_t)x.a]); if (wsole >= 0) e.wrapRax(wsole); }
                e.storeRaxToLocal(x.a);
                break;                                  // x = x + k preserves the kind byte
            case OP_LOCAL_ADD_LOCAL: {
                int pkl = jitSoleKind(jitPromoteSet(kindAt[i][(size_t)x.a], kindAt[i][(size_t)x.b]));
                if (pkl == (int)NumKind::U32) {
                    // 32 bit form: writing eax zeroes the upper half, which is exactly the
                    // canonical uint32 form, so the narrowing fixup is unnecessary
                    e.loadLocalToEax(x.a);
                    e.addEaxLocal(x.b);
                } else {
                    e.loadLocalToRax(x.a);
                    e.loadLocalToR8(x.b);
                    e.addRaxR8();
                    if (pkl >= 0) e.wrapRax(pkl);
                }
                e.storeRaxToLocal(x.a);
                if (tracked[(size_t)x.a]) {
                    int sole = jitSoleKind(jitPromoteSet(kindAt[i][(size_t)x.a],
                                                         kindAt[i][(size_t)x.b]));
                    if (sole < 0) return jitFail(__LINE__);
                    e.storeKindByte(e.kindBase + (size_t)x.a,
                                    retCodeOf(sole));
                }
                break;
            }
            case OP_ADD: case OP_SUB: case OP_MUL: {
                e.loadVrToRax(d - 2);
                e.loadVrToR8(d - 1);
                int pk = jitSoleKind(jitPromoteSet(vrKind[i][(size_t)d - 2],
                                                   vrKind[i][(size_t)d - 1]));
                if (pk == (int)NumKind::U32) {
                    // A write to a 32 bit register zeroes the upper half, which is already the
                    // canonical form of a uint32, so the fixup the other widths need is free here.
                    if (x.op == OP_ADD) { e.u8(0x44); e.u8(0x01); e.u8(0xC0); }        // add eax, r8d
                    else if (x.op == OP_SUB) { e.u8(0x44); e.u8(0x29); e.u8(0xC0); }   // sub eax, r8d
                    else { e.u8(0x44); e.u8(0x0F); e.u8(0xAF); e.u8(0xC0); }           // imul eax, r8d
                } else {
                    if (x.op == OP_ADD) e.addRaxR8();
                    else if (x.op == OP_SUB) e.subRaxR8();
                    else e.imulRaxR8();
                    if (pk >= 0) e.wrapRax(pk);
                }
                e.storeRaxToVr(d - 2);
                break;
            }
                break;
            case OP_EQ: case OP_NE: case OP_LT: case OP_GT: case OP_LE: case OP_GE: {
                e.loadVrToRax(d - 2);
                e.loadVrToR8(d - 1);
                e.cmpRaxR8();
                int uns = jitUnsignedCmp(vrKind[i][(size_t)d - 2], vrKind[i][(size_t)d - 1]);
                uint8_t cc;
                if (x.op == OP_EQ) cc = 0x94;
                else if (x.op == OP_NE) cc = 0x95;
                else if (x.op == OP_LT) cc = uns ? 0x92 : 0x9C;
                else if (x.op == OP_GT) cc = uns ? 0x97 : 0x9F;
                else if (x.op == OP_LE) cc = uns ? 0x96 : 0x9E;
                else cc = uns ? 0x93 : 0x9D;
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
            case OP_JUMP_IF_NOT_LT_LOCAL_LOCAL: {
                int unsLL = jitUnsignedCmp(kindAt[i][(size_t)x.a], kindAt[i][(size_t)x.b]);
                e.loadLocalToRax(x.a);
                e.cmpRaxWithLocal(x.b);
                e.jccPlaceholder(unsLL ? 0x83 : 0x8D, (size_t)x.target);   // jae/jge (not less)
                break;
            }
            case OP_JUMP_IF_NOT_LT_LOCAL_IMM: {
                int unsLI = jitUnsignedCmp(kindAt[i][(size_t)x.a], kJitNone);
                e.loadLocalToRax(x.a);
                e.cmpRaxImm(x.imm);
                e.jccPlaceholder(unsLI ? 0x83 : 0x8D, (size_t)x.target);
                break;
            }
            case OP_NEW_ARRAY: {
                // 1-D numeric array: allocate [len][data...] in the per-invocation arena; the
                // pointer replaces the dimension on the virtual stack
                int sizeVr = d - 1;
                e.loadVrToRax(sizeVr);
                e.callC((uint64_t)(uintptr_t)&annotaJitArrayAlloc, [&] {
                    e.u8(0x48); e.u8(0x8D); e.u8(0x0C); e.u8(0xC5); e.u32(8);   // lea rcx, [rax*8+8]
                });
                e.u8(0x48); e.u8(0x85); e.u8(0xC0);                          // test rax, rax
                size_t oom = e.jccRel8(0x74);                                // jz -> error
                e.u8(0x49); e.u8(0x89); e.u8(0xC3);                          // mov r11, rax
                e.loadVrToRax(sizeVr);                                       // the length
                e.u8(0x49); e.u8(0x89); e.u8(0x03);                          // mov [r11], rax
                e.storeR11ToVr(sizeVr);                                      // keep the pointer
                size_t afterNew = e.c.size();
                size_t skipNew = e.jmpRel8();
                e.errorReturn();
                e.patchRel8(skipNew, e.c.size());
                e.patchRel8(oom, afterNew);
                break;
            }
            case OP_GET_INDEX: {
                int objVr = d - 2, idxVr = d - 1;
                int ek = jitArrayKindOf(i, objVr);
                if (ek < 0) return jitFail(__LINE__);
                e.loadVrToR11(objVr);                                        // r11 = buffer
                e.loadVrToR8(idxVr);                                         // r8  = index
                e.u8(0x4D); e.u8(0x3B); e.u8(0x03);                          // cmp r8, [r11]  (len)
                size_t badGet = e.jccRel8(0x73);                             // jae -> error
                e.u8(0x4B); e.u8(0x8B); e.u8(0x44); e.u8(0xCB); e.u8(8);     // mov rax,[r11+r8*8+8]
                e.wrapRax(ek);                                               // widen to 64 bit
                e.storeRaxToVr(objVr);
                size_t afterGet = e.c.size();
                size_t skipGet = e.jmpRel8();
                e.errorReturn();
                e.patchRel8(skipGet, e.c.size());
                e.patchRel8(badGet, afterGet);
                break;
            }
            case OP_SET_INDEX: {
                int objVr = d - 3, idxVr = d - 2, valVr = d - 1;
                int ek = jitArrayKindOf(i, objVr);
                if (ek < 0) return jitFail(__LINE__);
                e.loadVrToR11(objVr);                                        // r11 = buffer
                e.loadVrToR8(idxVr);                                         // r8  = index
                e.u8(0x4D); e.u8(0x3B); e.u8(0x03);                          // cmp r8, [r11]
                size_t badSet = e.jccRel8(0x73);                             // jae -> error
                e.loadVrToRax(valVr);
                e.wrapRax(ek);                                               // wrap to the element width
                e.u8(0x4B); e.u8(0x89); e.u8(0x44); e.u8(0xCB); e.u8(8);     // mov [r11+r8*8+8], rax
                size_t afterSet = e.c.size();
                size_t skipSet = e.jmpRel8();
                e.errorReturn();
                e.patchRel8(skipSet, e.c.size());
                e.patchRel8(badSet, afterSet);
                break;
            }
            case OP_CALL_DIRECT: {
                // A native call to another compiled function: its L[] and JitOut live on this native
                // frame (so recursion works) and the arguments are copied out of the virtual
                // registers.  The layout is [L[]][JitOut][saved kBase][saved kOut]: `kBase` (this
                // function's L[] pointer, used by every local access) and `kOut` (where this
                // function writes its own result) are caller-saved, so the call would destroy them.
                // No Windows shadow space is reserved: the callee is machine code from this same
                // backend and never uses one.
                JitCallTarget t = resolveCall(x);
                if (!t.code) return jitFail(__LINE__);
                const int Lbytes = (int)t.chunk->numLocals * 8;
                const int savedBase = Lbytes + 16;                     // saved kBase
                const int savedOut = savedBase + 8;                    // saved kOut
                int N = savedOut + 8;                                  // L[] + JitOut + 2 saves
                if ((N % 16) != 8) N += 8 - (N % 16);                  // keep rsp 16-aligned at the call
                e.u8(0x48); e.u8(0x81); e.u8(0xEC); e.u32((uint32_t)N);       // sub rsp, N
                e.u8(0x48); e.u8(0x89);
                e.u8(JitEmitter::kBase == 0x01 ? 0x8C : 0xBC);                 // mov [rsp+savedBase], rcx/rdi
                e.u8(0x24); e.u32((uint32_t)savedBase);
                e.u8(0x48); e.u8(0x89);
                e.u8(JitEmitter::kOut == 0x02 ? 0x94 : 0xB4);                  // mov [rsp+savedOut], rdx/rsi
                e.u8(0x24); e.u32((uint32_t)savedOut);
                for (int k = 0; k < x.imm; k++) {
                    int src = N + (d - x.imm + k) * 8;                        // the argument's VR
                    e.u8(0x48); e.u8(0x8B); e.u8(0x84); e.u8(0x24); e.u32((uint32_t)src);
                    e.u8(0x48); e.u8(0x89); e.u8(0x84); e.u8(0x24); e.u32((uint32_t)(k * 8));
                }
                if (JitEmitter::kBase == 0x01) { e.u8(0x48); e.u8(0x89); e.u8(0xE1); }   // mov rcx, rsp (L)
                else { e.u8(0x48); e.u8(0x89); e.u8(0xE7); }                            // mov rdi, rsp (L)
                e.u8(0x48); e.u8(0x8D);
                e.u8(JitEmitter::kOut == 0x02 ? 0x94 : 0xB4);
                e.u8(0x24); e.u32((uint32_t)Lbytes);                                    // lea rdx/rsi, out
                e.u8(0x49); e.u8(0xBB);
                uint64_t fnAddr = (uint64_t)(uintptr_t)&t.code->fn;
                for (int b = 0; b < 8; b++) e.u8((uint8_t)((fnAddr >> (8 * b)) & 0xff));  // mov r11, &fn
                e.u8(0x4D); e.u8(0x8B); e.u8(0x1B);                                      // mov r11, [r11]
                e.u8(0x41); e.u8(0xFF); e.u8(0xD3);                                      // call r11
                e.u8(0x48); e.u8(0x8B); e.u8(0x84); e.u8(0x24);
                e.u32((uint32_t)Lbytes);                                                 // mov rax, out.value
                e.u8(0x48); e.u8(0x8B);
                e.u8(JitEmitter::kBase == 0x01 ? 0x8C : 0xBC);                           // restore kBase
                e.u8(0x24); e.u32((uint32_t)savedBase);
                e.u8(0x48); e.u8(0x8B);
                e.u8(JitEmitter::kOut == 0x02 ? 0x94 : 0xB4);                            // restore kOut
                e.u8(0x24); e.u32((uint32_t)savedOut);
                e.u8(0x48); e.u8(0x81); e.u8(0xC4); e.u32((uint32_t)N);                  // add rsp, N
                int resVr = d - x.imm;
                e.u8(0x48); e.u8(0x89); e.u8(0x84); e.u8(0x24); e.u32((uint32_t)(resVr * 8));
                // the kind byte of the destination local is written by the SET_LOCAL that follows
                break;
            }
            case OP_RETURN: {
                e.loadVrToRax(d - 1);
                int sole = jitSoleKind(vrKind[i][(size_t)d - 1]);
                if (i > 0 && ins[i - 1].op == OP_GET_LOCAL &&
                    tracked[(size_t)ins[i - 1].a]) {
                    // the value came straight from a tracked local: its kind byte is exact
                    e.loadKindByteToR8(e.kindBase + (size_t)ins[i - 1].a);
                    e.storeResultWithKindInR8();
                    noteRetByte(-2);
                } else if (sole >= 0) {
                    int byte = retCodeOf(sole);
                    e.storeResult(byte);
                    noteRetByte(byte);
                } else {
                    return jitFail(__LINE__);                 // kind would not match the interpreter
                }
                e.epilogue();
                break;
            }
            case OP_CONST: {
                // a pool constant the analysis proved to be a plain 64 bit integer
                const Value& c1 = ch->consts[(size_t)x.a];
                if (c1.i >= -2147483648LL && c1.i <= 2147483647LL) e.movRaxImm((int32_t)c1.i);
                else e.movRaxImm64((uint64_t)c1.i);
                e.storeRaxToVr(d);
                break;
            }
            case OP_CONVERT:
                // a typed parameter's conversion is the identity (the entry check proved it); an
                // untyped integer being given a declared width only wraps into that width
                if (jitSoleKind(vrKind[i][(size_t)d - 1]) == (int)NumKind::None) e.wrapRax(x.imm);
                break;
            case OP_RETURN_NULL:
                e.nullReturn();
                noteRetByte(3);
                break;
            default:
                if (std::getenv("ANNOTA_JIT_DEBUG"))
                    std::fprintf(stderr, "[jit] emit: unsupported op %d at ip %zu in %s\n",
                                 (int)x.op, x.at, ch->fnName.c_str());
                return jitFail(__LINE__);
        }
    }
    e.labelAt[code.size()] = e.c.size();
    e.nullReturn();
    // only a reachable fall-through can return null: an explicit `= expr` body ends in a
    // RETURN, so its trailing nullReturn is dead code and must not spoil the return kind
    {   int lastReachable = -1;
        for (size_t i2 = n; i2-- > 0;) if (depth[i2] != kUnset) { lastReachable = (int)i2; break; }
        if (lastReachable < 0) noteRetByte(3);
        else { uint8_t o = ins[(size_t)lastReachable].op;
               if (o != OP_JUMP && o != OP_LOOP && o != OP_RETURN && o != OP_RETURN_NULL) noteRetByte(3); } }

    if (e.pending.size() > 0 && !e.finish()) return jitFail(__LINE__);
    if (e.c.size() > 60000) return jitFail(__LINE__);

    if (std::getenv("ANNOTA_JIT_DEBUG"))
        std::fprintf(stderr, "[jit] compiled %s chunk=%p bytes=%zu\n", ch->fnName.c_str(),
                     (void*)ch.get(), e.c.size());
    // Hot loops can be entered directly: for every backward jump target with an empty operand
    // stack the interpreter may switch to this native code mid-function.  A trampoline runs the
    std::vector<std::pair<size_t, size_t>> tramp;     // (target ip, patch position)
    // prologue (so the epilogue stays balanced) and jumps to the loop header.
    // A chunk that builds its own arrays keeps them in a private buffer that exists only while the
    // native code runs, so it must not be entered in the middle of a loop: on-stack replacement
    // would hand it the interpreter's array object, which the machine code cannot use.
    if (!hasJitArrays) {
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
        e.localsPrologue((int)ch->numLocals, localsBase);                    // and the same locals
        e.u8(0xE9);
        int64_t rel = (int64_t)e.labelAt[ip] - (int64_t)(e.c.size() + 4);
        e.u32((uint32_t)(int32_t)rel);                                       // jmp loop header
        tramp.push_back({ip, off});
    }

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
    jc->retByte = seenRetByte;                 // -2 when the returns disagree
    jc->slotKind = expectKind;
    return jc;
}

} // namespace annota
