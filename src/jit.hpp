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
#include <cstddef>
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

// The kind byte a compiled function writes next to its result.  The first six codes are the
// original set; the widths that used to collapse into "int" (0) now have their own code, because
// the interpreter's `typeof` distinguishes them and a native result must be indistinguishable:
//   0 = int, 1 = int64, 2 = bool, 3 = null, 4 = runtime error, 5 = uint64,
//   6 = int8, 7 = int16, 8 = int32, 9 = uint8, 10 = uint16, 11 = uint32
static inline int jitKindCode(NumKind k) {
    switch (k) {
        case NumKind::I8:  return 6;
        case NumKind::I16: return 7;
        case NumKind::I32: return 8;
        case NumKind::I64: return 1;
        case NumKind::U8:  return 9;
        case NumKind::U16: return 10;
        case NumKind::U32: return 11;
        case NumKind::U64: return 5;
        default: return 0;                     // None, and everything a JIT result cannot be
    }
}
static inline bool jitKindFromCode(int code, NumKind& out) {
    switch (code) {
        case 0:  out = NumKind::None; return true;
        case 1:  out = NumKind::I64;  return true;
        case 5:  out = NumKind::U64;  return true;
        case 6:  out = NumKind::I8;   return true;
        case 7:  out = NumKind::I16;  return true;
        case 8:  out = NumKind::I32;  return true;
        case 9:  out = NumKind::U8;   return true;
        case 10: out = NumKind::U16;  return true;
        case 11: out = NumKind::U32;  return true;
        default: return false;                 // 2 = bool, 3 = null, 4 = error
    }
}

// Backing store for arrays created inside compiled code.  The code calls this function while it
// runs; the VM frees everything it handed out once the outermost native invocation returns.
int64_t* annotaJitArrayAlloc(int64_t bytes);   // implemented by the VM
void jitArenaReset();

// `print` inside compiled code: the machine code lays its operands out as real `Value`s in its own
// frame and the VM formats them with the same `toStr` the interpreter uses.  `packed` says the
// values are the elements of the tuple the compiler built for a multi-argument `print`, which the
// interpreter would have printed as a tuple.
void annotaJitPrint(const Value* const* refs, int64_t count, int64_t packed);

// Division and modulo are the two operations whose semantics (signed truncation, a divisor of
// zero, `INT64_MIN / -1`) are kept in C++: the machine code calls these and then checks the flag
// they set.  A non-zero flag after a native call becomes a catchable Annota error.
int64_t annotaJitDiv(int64_t a, int64_t b);
int64_t annotaJitMod(int64_t a, int64_t b);
extern int gJitDivErr;

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
    // what each such entry needs on the interpreter's operand stack (one byte per live register)
    std::map<size_t, std::vector<uint8_t>> osrDesc;
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
// Three marker kinds stand for things that are *not* an integer in a virtual register.  They are
// deliberately kept out of `jitSoleKind`, so every place that needs a real width rejects them:
//   kJitRange - a range iterator's state, kept in the backend's own private slots
//   kJitTuple - the tuple the compiler builds for `print(a, b)`, alive for exactly one instruction
//   kJitConst - a pool constant of a non-integer type, only usable as a `print` operand
static const JitKindSet kJitRange = (JitKindSet)1 << 14;
static const JitKindSet kJitTuple = (JitKindSet)1 << 13;
static const JitKindSet kJitConst = (JitKindSet)1 << 12;
static const JitKindSet kJitMarkers = kJitRange | kJitTuple | kJitConst;

// an ordinary integer value: the only thing arithmetic, assignments and returns may consume
static inline bool jitPlainKind(JitKindSet k) { return (k & kJitMarkers) == 0; }

// A constant that has no pool entry of its own (`true` / `false` / `null` compile to their own
// opcodes).  A `print` may refer to it; nothing else can use it.
inline const Value& jitPrintConstant(uint8_t op) {
    static const Value kTrue = Value::boolean(true);
    static const Value kFalse = Value::boolean(false);
    static const Value kNull = Value::null();
    return op == OP_TRUE ? kTrue : op == OP_FALSE ? kFalse : kNull;
}

// ---- on-stack replacement
// When the interpreter switches to native code in the middle of a loop, every value the loop header
// expects has to be rebuilt inside the native frame.  One descriptor byte per live virtual register
// says what that register has to hold, and the VM checks it against the interpreter's stack before
// the trampoline runs - a mismatch simply keeps the loop interpreted.
constexpr uint8_t kJitOsrAny = 0;          // any integer width
constexpr uint8_t kJitOsrWide64 = 1;       // an untyped int or an int64 (the non-wrapping widths)
constexpr uint8_t kJitOsrBool = 254;       // a comparison result
constexpr uint8_t kJitOsrIter = 255;       // a range iterator (state kept in the private slots)

// What the trampoline tells the VM's reconstruction helper; the machine code fills this in its own
// frame and passes one pointer to it.
struct JitOsrFrame {
    int64_t* vr;              // where the virtual registers live
    int64_t* iter;            // the private range-iterator slots
    const uint8_t* desc;      // one entry per live virtual register
    int64_t depth;
};
void annotaJitOsrInit(const JitOsrFrame* f);

// promote every pair and collect the outcomes
static inline JitKindSet jitPromoteSet(JitKindSet a, JitKindSet b) {
    JitKindSet out = 0;
    for (int i = 0; i < 16; i++) {
        if (!(a & ((JitKindSet)1 << i))) continue;
        if (i > (int)NumKind::U64 && i != 15) continue;      // bool, or a marker: no numeric width
        for (int j = 0; j < 16; j++) {
            if (!(b & ((JitKindSet)1 << j))) continue;
            if (j > (int)NumKind::U64 && j != 15) continue;
            NumKind ka = i == 15 ? NumKind::None : (NumKind)i;
            NumKind kb = j == 15 ? NumKind::None : (NumKind)j;
            if (i == 15 || j == 15) { out |= kJitBool; continue; }     // comparisons produce bool
            NumKind r = promoteNum(ka, kb);
            out |= jitBit(r);
        }
    }
    return out;
}

// -1 when the set does not pin down exactly one integer kind
static inline int jitSoleKind(JitKindSet s) {
    if (s == kJitBool) return 15;
    if (s & kJitMarkers) return -1;                  // not a value the arithmetic can use
    if (s == 0 || (s & (s - 1)) != 0) return -1;
    for (int i = 0; i <= (int)NumKind::U64; i++) if (s == ((JitKindSet)1 << i)) return i;
    return -1;
}

// The on-stack-replacement descriptor for one live virtual register, and the matching check.
static inline uint8_t jitOsrDescOf(JitKindSet k) {
    if (k == kJitRange) return kJitOsrIter;
    int sole = jitSoleKind(k);
    if (sole == 15) return kJitOsrBool;
    if (sole >= 0) return (uint8_t)(jitKindCode((NumKind)sole) + 2);
    if (k == kJitAllInts) return kJitOsrAny;
    return kJitOsrWide64;
}

static inline bool jitOsrDescOk(uint8_t d, NumKind k) {
    if (d == kJitOsrAny) return true;
    if (d == kJitOsrWide64) return k == NumKind::None || k == NumKind::I64;
    if (d == kJitOsrBool || d == kJitOsrIter) return false;
    return (int)d - 2 == jitKindCode(k);
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
    // A slot whose kind is 32 bit may hold a raw value: a 32 bit operation leaves the low half exact
    // and zeroes the upper half, so anything that needs the 64 bit value widens it first.  This is
    // what lets `int32` arithmetic skip the narrowing fixup.
    void sextRaxFromI32() { u8(0x48); u8(0x63); u8(0xC0); }             // movsxd rax, eax
    void sextR8FromI32() { u8(0x4D); u8(0x63); u8(0xC0); }              // movsxd r8, r8d
    void cmpEaxR8d() { u8(0x44); u8(0x39); u8(0xC0); }                 // cmp eax, r8d
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
    void loadVrToR10(int vr) {
        u8(0x4C); u8(0x8B);
        if (vr * 8 < 128) { u8(0x54); u8(0x24); u8((uint8_t)(vr * 8)); }
        else { u8(0x94); u8(0x24); u32((uint32_t)(vr * 8)); }
    }
    void sextR10FromI32() { u8(0x4D); u8(0x63); u8(0xD2); }             // movsxd r10, r10d
    void sextR11FromI32() { u8(0x4D); u8(0x63); u8(0xDB); }             // movsxd r11, r11d
    void storeR11ToVr(int vr) {
        u8(0x4C); u8(0x89);
        if (vr * 8 < 128) { u8(0x5C); u8(0x24); u8((uint8_t)(vr * 8)); }
        else { u8(0x9C); u8(0x24); u32((uint32_t)(vr * 8)); }
    }
    // Call a C helper that takes two integers: r10 holds the first, r11 the second (both must be
    // loaded before the frame moves), and the result comes back in rax.  kBase and kOut are saved
    // around the call exactly as in callC; rax carries the callee address so r10/r11 stay live.
    void callC2(uint64_t addr) {
        const int savedBase = 32, savedOut = 40;
        int N = 48;
        if ((N % 16) != 8) N += 8 - (N % 16);
        u8(0x48); u8(0x81); u8(0xEC); u32((uint32_t)N);                       // sub rsp, N
        u8(0x48); u8(0x89); u8(kBase == 0x01 ? 0x8C : 0xBC);
        u8(0x24); u32((uint32_t)savedBase);                                   // save kBase
        u8(0x48); u8(0x89); u8(kOut == 0x02 ? 0x94 : 0xB4);
        u8(0x24); u32((uint32_t)savedOut);                                    // save kOut
        u8(0x4C); u8(0x89); u8(kBase == 0x01 ? 0xD1 : 0xD7);                  // mov rcx/rdi, r10
        u8(0x4C); u8(0x89); u8(kOut == 0x02 ? 0xDA : 0xDE);                   // mov rdx/rsi, r11
        u8(0x48); u8(0xB8);
        for (int b = 0; b < 8; b++) u8((uint8_t)((addr >> (8 * b)) & 0xff));  // mov rax, addr
        u8(0xFF); u8(0xD0);                                                   // call rax
        u8(0x48); u8(0x8B); u8(kBase == 0x01 ? 0x8C : 0xBC);
        u8(0x24); u32((uint32_t)savedBase);                                   // restore kBase
        u8(0x48); u8(0x8B); u8(kOut == 0x02 ? 0x94 : 0xB4);
        u8(0x24); u32((uint32_t)savedOut);                                    // restore kOut
        u8(0x48); u8(0x81); u8(0xC4); u32((uint32_t)N);                       // add rsp, N
    }
    // Call a C helper with two integers plus a constant third argument.  Everything else is the
    // same as callC2; the third argument is an immediate, which is what `print` needs to tell the
    // VM whether the values are one operand list or a tuple.
    void callC3(uint64_t addr, int32_t imm3) {
        const int savedBase = 32, savedOut = 40;
        int N = 48;
        if ((N % 16) != 8) N += 8 - (N % 16);
        u8(0x48); u8(0x81); u8(0xEC); u32((uint32_t)N);                       // sub rsp, N
        u8(0x48); u8(0x89); u8(kBase == 0x01 ? 0x8C : 0xBC);
        u8(0x24); u32((uint32_t)savedBase);                                   // save kBase
        u8(0x48); u8(0x89); u8(kOut == 0x02 ? 0x94 : 0xB4);
        u8(0x24); u32((uint32_t)savedOut);                                    // save kOut
        u8(0x4C); u8(0x89); u8(kBase == 0x01 ? 0xD1 : 0xD7);                  // mov rcx/rdi, r10
        u8(0x4C); u8(0x89); u8(kOut == 0x02 ? 0xDA : 0xDE);                   // mov rdx/rsi, r11
        if (kBase == 0x01) { u8(0x41); u8(0xB8); } else { u8(0xBA); }
        u32((uint32_t)imm3);                                                  // mov r8d/edx, imm3
        u8(0x48); u8(0xB8);
        for (int b = 0; b < 8; b++) u8((uint8_t)((addr >> (8 * b)) & 0xff));  // mov rax, addr
        u8(0xFF); u8(0xD0);                                                   // call rax
        u8(0x48); u8(0x8B); u8(kBase == 0x01 ? 0x8C : 0xBC);
        u8(0x24); u32((uint32_t)savedBase);                                   // restore kBase
        u8(0x48); u8(0x8B); u8(kOut == 0x02 ? 0x94 : 0xB4);
        u8(0x24); u32((uint32_t)savedOut);                                    // restore kOut
        u8(0x48); u8(0x81); u8(0xC4); u32((uint32_t)N);                       // add rsp, N
    }
    // The C helper above reports a failure (a zero divisor) through a global flag; the caller
    // follows this with a conditional jump over a cold `errorReturn()` block.
    void loadFlagToR10(uint64_t addr) {
        u8(0x49); u8(0xBA);
        for (int b = 0; b < 8; b++) u8((uint8_t)((addr >> (8 * b)) & 0xff));  // movabs r10, addr
        u8(0x41); u8(0x83); u8(0x3A); u8(0x00);                               // cmp dword [r10], 0
    }
    // Frame-relative helpers for the temporary `Value` array that `print` hands to the VM.
    void storeRaxToFrame(size_t off) { u8(0x48); u8(0x89); u8(0x84); u8(0x24); u32((uint32_t)off); }
    void loadFrameToRax(size_t off) { u8(0x48); u8(0x8B); u8(0x84); u8(0x24); u32((uint32_t)off); }
    void loadFrameToR8(size_t off) { u8(0x4C); u8(0x8B); u8(0x84); u8(0x24); u32((uint32_t)off); }
    void loadFrameToR11(size_t off) { u8(0x4C); u8(0x8B); u8(0x9C); u8(0x24); u32((uint32_t)off); }
    void storeR11ToFrame(size_t off) { u8(0x4C); u8(0x89); u8(0x9C); u8(0x24); u32((uint32_t)off); }
    void addR11Imm8(uint8_t v) { u8(0x49); u8(0x83); u8(0xC3); u8(v); }
    void storeAlToFrame(size_t off) { u8(0x88); u8(0x84); u8(0x24); u32((uint32_t)off); }
    void storeByteToFrame(size_t off, uint8_t v) {
        u8(0xC6); u8(0x84); u8(0x24); u32((uint32_t)off); u8(v);
    }
    void storeZeroQwordToFrame(size_t off) {
        u8(0x48); u8(0xC7); u8(0x84); u8(0x24); u32((uint32_t)off); u32(0);
    }
    void leaR10FromFrame(size_t off) { u8(0x4C); u8(0x8D); u8(0x94); u8(0x24); u32((uint32_t)off); }
    void leaRaxFromFrame(size_t off) { u8(0x48); u8(0x8D); u8(0x84); u8(0x24); u32((uint32_t)off); }
    void setneAl() { u8(0x0F); u8(0x95); u8(0xC0); }
    void movR11Imm64(uint64_t v) {
        u8(0x49); u8(0xBB);
        for (int b = 0; b < 8; b++) u8((uint8_t)((v >> (8 * b)) & 0xff));
    }
    // A short conditional jump whose displacement is filled in once the target offset is known
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
    int maxPrintArgs = 0;                              // frame space for the values `print` passes
    bool hasPrint = false;
    bool hasRange = false;                             // frame space for the range iterators
    size_t ip = 0;
    while (ip < code.size()) {
        JitIns x;
        x.op = code[ip];
        x.at = ip;
        x.next = ip;
        switch (x.op) {
            case OP_NOP: x.next = ip + 1; break;
            case OP_TRUE: case OP_FALSE: case OP_NULL: x.next = ip + 1; break;
            case OP_INT1: x.next = ip + 2; x.imm = (int8_t)code[ip + 1]; break;
            case OP_POP: x.next = ip + 1; break;
            case OP_ITER_RANGE: x.next = ip + 1; hasRange = true; break;
            case OP_ITER_NEXT:
                // s16: where to go once the range is exhausted; otherwise the next element follows
                x.next = ip + 3;
                x.target = (int)((int64_t)(ip + 3) + (int16_t)((code[ip + 1] << 8) | code[ip + 2]));
                break;
            case OP_GET_LOCAL: case OP_SET_LOCAL: case OP_INIT_LOCAL:
                x.next = ip + 2; x.a = code[ip + 1]; break;
            case OP_GET_INDEX: case OP_SET_INDEX:
                x.next = ip + 1; break;
            case OP_NEW_ARRAY:
                x.next = ip + 3; x.a = code[ip + 1]; x.b = code[ip + 2]; break;
            case OP_ADD: case OP_SUB: case OP_MUL: case OP_DIV: case OP_MOD:
            case OP_EQ: case OP_NE: case OP_LT: case OP_GT: case OP_LE: case OP_GE:
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
            case OP_PRINT:
                x.next = ip + 3; x.a = code[ip + 1]; x.b = code[ip + 2];
                maxPrintArgs = std::max(maxPrintArgs, (int)x.a);
                hasPrint = true;
                break;
            case OP_BUILD_TUPLE:
                // `print(a, b)` compiles to a tuple followed by a one-operand print; the backend
                // recognizes exactly that pair and never lets a tuple value exist on its own
                x.next = ip + 3; x.a = (code[ip + 1] << 8) | code[ip + 2];
                maxPrintArgs = std::max(maxPrintArgs, (int)x.a);
                break;
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
        // the result kind has to be a single, known one - otherwise this callee cannot be a direct
        // call target (a `null` body, an error return, or two returns that disagree)
        { NumKind rk;
          if (t.code->retByte != 2 && !jitKindFromCode(t.code->retByte, rk)) {
              if (std::getenv("ANNOTA_JIT_DEBUG"))
                  std::fprintf(stderr, "[jit] call target %s retByte %d\n", nm.c_str(), t.code->retByte);
              return none;
          } }
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
    std::vector<int> packedPrint(n, 0);                        // print whose operands are a tuple
    std::vector<std::vector<JitKindSet>> printKinds(n);        // the operand kinds of each print
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
                case OP_TRUE: case OP_FALSE: case OP_NULL: pushKind(kJitConst); break;
                case OP_GET_LOCAL: pushKind(kindOfLocal(x.a)); break;
                case OP_NEW_ARRAY:
                    // 1-D numeric array: the dimension on the stack becomes a raw buffer pointer
                    if (d < (int)x.a) return jitFail(__LINE__);
                    for (int k = 0; k < (int)x.a; k++) vk.pop_back();
                    pushKind(jitBit(NumKind::I64));
                    break;
                case OP_GET_INDEX: {
                    if (d < 2) return jitFail(__LINE__);
                    if (!jitPlainKind(vk[(size_t)d - 1]) || !jitPlainKind(vk[(size_t)d - 2]))
                        return jitFail(__LINE__);
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
                    if (!jitPlainKind(vk[(size_t)d - 1]) || !jitPlainKind(vk[(size_t)d - 2]) ||
                        !jitPlainKind(vk[(size_t)d - 3]))
                        return jitFail(__LINE__);
                    vk.pop_back(); vk.pop_back(); vk.pop_back();
                    break;
                case OP_SET_LOCAL: case OP_INIT_LOCAL:
                    if (d < 1 || !jitPlainKind(vk.back())) return jitFail(__LINE__);
                    vk.pop_back();
                    break;
                case OP_LOCAL_ADD_IMM: case OP_LOCAL_SUB_IMM:
                case OP_LOCAL_ADD_LOCAL:
                    break;                                  // raw int64 work, verified at call time
                case OP_ADD: case OP_SUB: case OP_MUL:
                case OP_DIV: case OP_MOD: {
                    if (d < 2) return jitFail(__LINE__);
                    JitKindSet ka = vk[(size_t)d - 2], kb = vk[(size_t)d - 1];
                    if (!jitPlainKind(ka) || !jitPlainKind(kb)) return jitFail(__LINE__);
                    vk.pop_back();
                    vk.back() = jitPromoteSet(ka, kb);
                    break;
                }
                case OP_EQ: case OP_NE: case OP_LT: case OP_GT: case OP_LE: case OP_GE: {
                    if (d < 2) return jitFail(__LINE__);
                    if (!jitPlainKind(vk[(size_t)d - 2]) || !jitPlainKind(vk[(size_t)d - 1]))
                        return jitFail(__LINE__);
                    vk.pop_back();
                    vk.back() = kJitBool;
                    break;
                }
                case OP_JUMP: case OP_LOOP: break;
                case OP_ITER_RANGE: {
                    // `for i in a to b`: the interpreter builds a heap iterator whose bounds are
                    // evaluated once; the backend keeps the same state in two private slots (the
                    // upper bound and the current value) and leaves a marker in the slot the
                    // interpreter's iterator object would occupy, so every depth still lines up.
                    if (d < 2) return jitFail(__LINE__);
                    if (!jitPlainKind(vk[(size_t)d - 2]) || !jitPlainKind(vk[(size_t)d - 1]))
                        return jitFail(__LINE__);
                    vk.pop_back();
                    vk.back() = kJitRange;
                    break;
                }
                case OP_ITER_NEXT: {
                    if (d < 1 || vk.back() != kJitRange) return jitFail(__LINE__);
                    vk.push_back(kJitNone);             // Value::integer(cur): an untyped int
                    break;
                }
                case OP_POP:
                    if (d < 1) return jitFail(__LINE__);
                    vk.pop_back();
                    break;
                case OP_JUMP_IF_FALSE: {
                    if (d < 1 || !jitPlainKind(vk.back())) return jitFail(__LINE__);
                    vk.pop_back();
                    break;
                }
                case OP_JUMP_IF_NOT_LT_LOCAL_LOCAL: case OP_JUMP_IF_NOT_LT_LOCAL_IMM: break;
                case OP_CONST: {
                    // pool constants that are plain 64 bit integers take part in the arithmetic; any
                    // other constant is a real Value the backend cannot hold, but a `print` can
                    // still refer to it, so it becomes a marker instead of rejecting the function
                    if ((size_t)x.a >= ch->consts.size()) return jitFail(__LINE__);
                    const Value& c0 = ch->consts[(size_t)x.a];
                    if (c0.t == VT::Int && !c0.o && (int)c0.k <= (int)NumKind::U64) {
                        pushKind(jitBit(c0.k));
                    } else {
                        pushKind(kJitConst);
                    }
                    break;
                }
                case OP_CONVERT: {
                    // An integer conversion is `wrapToKind` and nothing else (the interpreter's
                    // convertNum wraps an Int into the target width), so the machine code just wraps
                    // the raw value.  Any non-integer target - a float, a 128 bit width, a string -
                    // stays interpreted.
                    if (d < 1 || x.imm > (int)NumKind::U64) return jitFail(__LINE__);
                    if (jitSoleKind(vk.back()) < 0) return jitFail(__LINE__);
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
                    if (t.code->retByte == 2) pushKind(kJitBool);
                    else {
                        NumKind rk;
                        if (!jitKindFromCode(t.code->retByte, rk)) return jitFail(__LINE__);
                        pushKind(jitBit(rk));
                    }
                }
                case OP_RETURN:
                    if (d < 1 || !jitPlainKind(vk.back())) return jitFail(__LINE__);
                    break;                                   // exact kind handled at emit time
                case OP_RETURN_NULL: break;
                case OP_PRINT: {
                    // `print` hands its operands to the VM as ordinary Values, so every one of them
                    // needs one static kind: a sole width (or the None/int64 pair, which print
                    // identically).  A separator is a value of its own and stays interpreted.
                    if (x.b) return jitFail(__LINE__);
                    if (x.a == 1 && i > 0 && ins[i - 1].op == OP_BUILD_TUPLE && !vk.empty() &&
                        vk.back() == kJitTuple) {
                        // the operands are the tuple the previous instruction built
                        packedPrint[i] = ins[i - 1].a;
                        vk.pop_back();
                        break;
                    }
                    if (d < x.a) return jitFail(__LINE__);
                    for (int k = 0; k < x.a; k++) {
                        JitKindSet ks = vk[(size_t)(d - x.a + k)];
                        if (ks == kJitConst) { printKinds[i].push_back(ks); continue; }
                        if (!jitPlainKind(ks)) return jitFail(__LINE__);
                        if (jitSoleKind(ks) < 0 &&
                            (ks & ~(jitBit(NumKind::None) | jitBit(NumKind::I64))) != 0)
                            return jitFail(__LINE__);
                        printKinds[i].push_back(ks);
                    }
                    for (int k = 0; k < x.a; k++) vk.pop_back();
                    break;
                }
                case OP_BUILD_TUPLE: {
                    // Only ever the operand list of the `print` that follows it: the tuple itself
                    // never becomes a value the backend would have to represent.
                    if (x.a < 1 || i + 1 >= n) return jitFail(__LINE__);
                    if (ins[i + 1].op != OP_PRINT || ins[i + 1].a != 1 || ins[i + 1].b)
                        return jitFail(__LINE__);
                    if (d < x.a) return jitFail(__LINE__);
                    for (int k = 0; k < x.a; k++) {
                        JitKindSet ks = vk[(size_t)(d - x.a + k)];
                        if (ks == kJitConst) { printKinds[i].push_back(ks); continue; }
                        if (!jitPlainKind(ks)) return jitFail(__LINE__);
                        if (jitSoleKind(ks) < 0 &&
                            (ks & ~(jitBit(NumKind::None) | jitBit(NumKind::I64))) != 0)
                            return jitFail(__LINE__);
                        printKinds[i].push_back(ks);
                    }
                    for (int k = 0; k < x.a; k++) vk.pop_back();
                    vk.push_back(kJitTuple);
                    break;
                }
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
                         : (x.op == OP_ADD || x.op == OP_SUB || x.op == OP_MUL ||
                            x.op == OP_DIV || x.op == OP_MOD ||
                            x.op == OP_EQ || x.op == OP_NE || x.op == OP_LT || x.op == OP_GT ||
                            x.op == OP_LE || x.op == OP_GE) ? d - 1
                         : (int)vk.size();
            if ((int)vk.size() != newDepth) return jitFail(__LINE__);   // codegen safety net
            // `iter_next` is the one instruction whose two edges leave the stack at different
            // depths: the fallthrough pushes the element, the exhausted jump keeps only the
            // iterator.  Both edges inherit the same locals, but their stacks differ.
            int jumpDepth = newDepth;
            std::vector<JitKindSet> jumpVk = vk;
            if (x.op == OP_ITER_NEXT) {
                jumpDepth = d;
                jumpVk = vrKind[i];
            }
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
                          x.op == OP_ITER_NEXT ||
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
                        depth[ti] = jumpDepth;
                        kindAt[ti] = nk;
                        assignedAt[ti] = na;
                        vrKind[ti] = jumpVk;
                        changed = true;
                    } else if (depth[ti] != jumpDepth) {
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
    // `print` builds a real Value per operand here, inside the frame, so nothing is allocated and
    // the VM keeps owning the formatting rules.  A pool constant is referred to directly (its
    // address is stable for as long as the code is), everything else is materialized.
    const size_t printCount = hasPrint ? (size_t)std::max(1, maxPrintArgs) : 0;
    const size_t printValuesBytes = ((printCount * sizeof(Value) + 15) / 16) * 16;
    const size_t printRefsBytes = ((printCount * 8 + 15) / 16) * 16;
    const size_t printBase = e.frameBytes;
    const size_t printRefBase = printBase + printValuesBytes;
    e.frameBytes += printValuesBytes + printRefsBytes;
    // a range iterator lives in two private slots per virtual register (the current value and the
    // upper bound); the interpreter's heap object is never represented in machine code
    const size_t iterBase = e.frameBytes;
    e.frameBytes += hasRange ? ((size_t)std::max(1, maxDepth) * 16 + 15) / 16 * 16 : 0;
    // the trampoline's on-stack-replacement descriptor (a JitOsrFrame) lives here too
    const size_t osrFrameBase = e.frameBytes;
    e.frameBytes += 32;
    auto iterCurOff = [&](int vr) { return iterBase + (size_t)vr * 16; };
    auto iterStopOff = [&](int vr) { return iterBase + (size_t)vr * 16 + 8; };
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
        return sole == 15 ? 2 : jitKindCode((NumKind)sole);
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
            case OP_TRUE: case OP_FALSE: case OP_NULL:
                break;                                  // only reachable as a print operand
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
                if (pk == (int)NumKind::U32 || pk == (int)NumKind::I32) {
                    // A write to a 32 bit register zeroes the upper half, so the low 32 bits already
                    // are the exact value and no narrowing fixup is needed.  A slot written this way
                    // holds a "raw" 32 bit value; every consumer that needs 64 bits widens it first.
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
            case OP_DIV: case OP_MOD: {
                // The semantics live in the two C helpers (a zero divisor is a catchable error the
                // hardware would instead turn into a fault), so the machine code just calls one.
                int ka = jitSoleKind(vrKind[i][(size_t)d - 2]);
                int kb = jitSoleKind(vrKind[i][(size_t)d - 1]);
                int pkD = jitSoleKind(jitPromoteSet(vrKind[i][(size_t)d - 2],
                                                    vrKind[i][(size_t)d - 1]));
                if (pkD < 0) return jitFail(__LINE__);
                e.loadVrToR10(d - 2);
                if (ka == (int)NumKind::I32) e.sextR10FromI32();      // a raw 32 bit slot
                e.loadVrToR11(d - 1);
                if (kb == (int)NumKind::I32) e.sextR11FromI32();
                e.callC2((uint64_t)(uintptr_t)(x.op == OP_DIV ? &annotaJitDiv : &annotaJitMod));
                e.loadFlagToR10((uint64_t)(uintptr_t)&gJitDivErr);
                size_t noErr = e.jccRel8(0x74);                       // jz -> the result is valid
                e.errorReturn();                                      // otherwise report and return
                e.patchRel8(noErr, e.c.size());
                // 32 bit kinds already have their exact value in the low half (which is all their
                // slot uses); every other width wraps the way the interpreter's typedInt would
                if (pkD != (int)NumKind::I32 && pkD != (int)NumKind::U32) e.wrapRax(pkD);
                e.storeRaxToVr(d - 2);
                break;
            }
            case OP_EQ: case OP_NE: case OP_LT: case OP_GT: case OP_LE: case OP_GE: {
                e.loadVrToRax(d - 2);
                e.loadVrToR8(d - 1);
                // A 32 bit comparison is correct whether the slots hold the exact 64 bit value or only
                // the low 32 bits, and it is shorter than the 64 bit form.
                int pkC = jitSoleKind(jitPromoteSet(vrKind[i][(size_t)d - 2],
                                                    vrKind[i][(size_t)d - 1]));
                if (pkC == (int)NumKind::I32 || pkC == (int)NumKind::U32) e.cmpEaxR8d();
                else {
                    // a raw 32 bit operand has to be widened before a 64 bit comparison
                    if (jitSoleKind(vrKind[i][(size_t)d - 2]) == (int)NumKind::I32) e.sextRaxFromI32();
                    if (jitSoleKind(vrKind[i][(size_t)d - 1]) == (int)NumKind::I32) e.sextR8FromI32();
                    e.cmpRaxR8();
                }
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
            case OP_ITER_RANGE: {
                // `for i in a to b` counts upwards (`step` is always +1 here): the upper bound goes
                // into the iterator's stop slot, the lower bound becomes the current value
                e.loadVrToRax(d - 1);
                e.storeRaxToFrame(iterStopOff(d - 2));
                e.loadVrToRax(d - 2);
                e.storeRaxToFrame(iterCurOff(d - 2));
                break;
            }
            case OP_ITER_NEXT: {
                int vr = d - 1;
                e.loadFrameToRax(iterCurOff(vr));                    // rax = cur
                e.loadFrameToR8(iterStopOff(vr));                    // r8  = stop
                e.cmpRaxR8();
                e.jccPlaceholder(0x8F, (size_t)x.target);            // jg -> exhausted
                e.storeRaxToVr(d);                                   // push the element
                e.loadFrameToR11(iterCurOff(vr));
                e.addR11Imm8(1);                                     // cur += 1 (after the push)
                e.storeR11ToFrame(iterCurOff(vr));
                break;
            }
            case OP_POP:
                break;                                               // the value is dead
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
                    // the callee checks kinds against L[], so a raw 32 bit argument must be exact
                    if (jitSoleKind(vrKind[i][(size_t)(d - x.imm + k)]) == (int)NumKind::I32)
                        e.sextRaxFromI32();
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
                // a raw 32 bit slot only carries the low half: make the value exact for the VM
                if (sole == (int)NumKind::I32) e.sextRaxFromI32();
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
                // an integer conversion wraps into the target width and *stores* the result: the
                // slot now holds a value of the new kind, so every later read sees the wrapped one
                e.loadVrToRax(d - 1);
                // a raw 32 bit slot has to be widened first so the wrap sees the exact value
                if (jitSoleKind(vrKind[i][(size_t)d - 1]) == (int)NumKind::I32) e.sextRaxFromI32();
                e.wrapRax(x.imm);
                e.storeRaxToVr(d - 1);
                break;
            case OP_RETURN_NULL:
                e.nullReturn();
                noteRetByte(3);
                break;
            case OP_BUILD_TUPLE:
                break;                                  // folded into the `print` that follows it
            case OP_PRINT: {
                // One reference per operand: a pool constant is used as it is, an integer in a
                // virtual register is materialized as a real Value with its static kind.  The VM
                // then formats them with the interpreter's own rules.
                int arity = packedPrint[i] ? packedPrint[i] : x.a;
                int firstVr = packedPrint[i] ? d - 1 : d - x.a;
                const std::vector<JitKindSet>& oks = printKinds[packedPrint[i] ? (size_t)i - 1 : i];
                for (int k = 0; k < arity; k++) {
                    int src = firstVr + k;
                    int ks = jitSoleKind(oks[(size_t)k]);
                    size_t vb = printBase + (size_t)k * sizeof(Value);
                    size_t rb = printRefBase + (size_t)k * 8;
                    // the producer of this virtual register: a pool constant or one of the
                    // constant opcodes is a Value the code can refer to directly
                    int ck = -1;
                    uint8_t cop = 0;
                    for (size_t s = i; s-- > 0;) {
                        if (depth[s] != src) continue;
                        if (ins[s].op == OP_CONST) ck = ins[s].a;
                        else if (ins[s].op == OP_TRUE || ins[s].op == OP_FALSE || ins[s].op == OP_NULL)
                            cop = ins[s].op;
                        break;
                    }
                    if (ck >= 0) {
                        e.movRaxImm64((uint64_t)(uintptr_t)&ch->consts[(size_t)ck]);
                    } else if (cop) {
                        e.movRaxImm64((uint64_t)(uintptr_t)&jitPrintConstant(cop));
                    } else {
                        e.loadVrToRax(src);
                        if (ks == (int)NumKind::I32) e.sextRaxFromI32();
                        e.storeRaxToFrame(vb + offsetof(Value, i));
                        // the shared_ptr the Value holds must read as null; nothing ever releases it
                        for (size_t z = offsetof(Value, o); z < sizeof(Value); z += 8)
                            e.storeZeroQwordToFrame(vb + z);
                        if (ks == 15) {
                            e.testRaxRax();
                            e.setneAl();
                            e.storeAlToFrame(vb + offsetof(Value, b));
                            e.storeByteToFrame(vb + offsetof(Value, t), (uint8_t)VT::Bool);
                            e.storeByteToFrame(vb + offsetof(Value, k), 0);
                        } else {
                            e.storeByteToFrame(vb + offsetof(Value, b), 0);
                            e.storeByteToFrame(vb + offsetof(Value, t), (uint8_t)VT::Int);
                            // a non-sole set can only be {None, int64} here, which prints the same
                            e.storeByteToFrame(vb + offsetof(Value, k),
                                               (uint8_t)(ks < 0 ? (int)NumKind::None : ks));
                        }
                        e.leaRaxFromFrame(vb);
                    }
                    e.storeRaxToFrame(rb);
                }
                e.leaR10FromFrame(printRefBase);
                e.movR11Imm64((uint64_t)arity);
                e.callC3((uint64_t)(uintptr_t)&annotaJitPrint, packedPrint[i] ? 1 : 0);
                break;
            }
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
    // Hot loops can be entered directly: for every backward jump target whose live values the
    // backend can rebuild, the interpreter may switch to this native code mid-function.  A
    // trampoline runs the prologue (so the epilogue stays balanced), asks the VM to rebuild the
    // virtual registers from the interpreter's operand stack, and jumps to the loop header.
    // A chunk that builds its own arrays keeps them in a private buffer that exists only while the
    // native code runs, so it must not be entered in the middle of a loop: on-stack replacement
    // would hand it the interpreter's array object, which the machine code cannot use.
    std::vector<std::pair<size_t, size_t>> tramp;          // (target ip, native offset)
    std::vector<std::vector<uint8_t>> descs;               // one descriptor per OSR entry
    std::vector<std::pair<size_t, size_t>> descPatches;    // (imm64 position, descriptor index)
    if (!hasJitArrays) {
    for (auto& kv : e.labelAt) {
        size_t ip = kv.first;
        if (ip >= code.size()) continue;
        auto di = indexOf.find(ip);
        if (di == indexOf.end()) continue;
        size_t ti = di->second;
        if (depth[ti] == kUnset) continue;
        bool isLoopHeader = false;
        for (auto& x : ins) {
            bool backward = (x.op == OP_LOOP || x.op == OP_JUMP) && x.target >= 0 &&
                            (size_t)x.target < x.at;
            if (backward && (size_t)x.target == ip) { isLoopHeader = true; break; }
        }
        if (!isLoopHeader) continue;
        // every live register has to be something the VM can rebuild from the interpreter's stack:
        // an integer, a boolean or a range iterator
        std::vector<uint8_t> desc;
        bool ok = true;
        for (int r = 0; r < depth[ti]; r++) {
            JitKindSet ks = vrKind[ti][(size_t)r];
            if (!jitPlainKind(ks) && ks != kJitRange) { ok = false; break; }
            desc.push_back(jitOsrDescOf(ks));
        }
        if (!ok) continue;
        size_t off = e.c.size();
        e.u8(0x48); e.u8(0x81); e.u8(0xEC); e.u32((uint32_t)e.frameBytes);   // sub rsp, frame
        e.localsPrologue((int)ch->numLocals, localsBase);                    // and the same locals
        e.leaRaxFromFrame(0);                                                // the virtual registers
        e.storeRaxToFrame(osrFrameBase);
        if (hasRange) e.leaRaxFromFrame(iterBase);
        else e.movRaxImm(0);                                                 // no iterators at all
        e.storeRaxToFrame(osrFrameBase + 8);
        e.u8(0x48); e.u8(0xB8);                                              // mov rax, <descriptor>
        size_t patchAt = e.c.size();
        for (int b = 0; b < 8; b++) e.u8(0);
        e.storeRaxToFrame(osrFrameBase + 16);
        e.movRaxImm((int32_t)depth[ti]);
        e.storeRaxToFrame(osrFrameBase + 24);
        e.leaR10FromFrame(osrFrameBase);
        e.callC2((uint64_t)(uintptr_t)&annotaJitOsrInit);
        e.u8(0xE9);
        int64_t rel = (int64_t)e.labelAt[ip] - (int64_t)(e.c.size() + 4);
        e.u32((uint32_t)(int32_t)rel);                                       // jmp loop header
        descPatches.push_back({patchAt, descs.size()});
        descs.push_back(std::move(desc));
        tramp.push_back({ip, off});
    }

    }
    // the descriptors travel inside the code buffer: their displacement from the code never changes,
    // so only the absolute address the trampoline loads has to be filled in once it is mapped
    std::vector<size_t> descOff(descs.size(), 0);
    for (size_t k = 0; k < descs.size(); k++) {
        descOff[k] = e.c.size();
        for (uint8_t b : descs[k]) e.c.push_back(b);
        while (e.c.size() % 16 != 0) e.c.push_back(0);
    }
    auto jc = std::make_shared<JitCode>();
    jc->bytes = e.c;
    void* mem = jitAllocExec(jc->bytes.size());
    if (!mem) return jitFail(__LINE__);
    std::memcpy(mem, jc->bytes.data(), jc->bytes.size());
    for (auto& p : descPatches) {
        uint64_t addr = (uint64_t)(uintptr_t)((uint8_t*)mem + descOff[p.second]);
        std::memcpy((uint8_t*)mem + p.first, &addr, 8);
    }
    jc->mapping = mem;
    jc->mappingSize = jc->bytes.size();
    jc->fn = reinterpret_cast<JitFn>(mem);
    for (auto& t : tramp)
        jc->osrEntries[t.first] = reinterpret_cast<JitFn>((uint8_t*)mem + t.second);
    for (size_t k = 0; k < tramp.size(); k++) jc->osrDesc[tramp[k].first] = descs[k];
    for (size_t s = 0; s < readSlots.size(); s++)
        if (readSlots[s]) jc->readSlots.push_back((uint8_t)s);
    jc->retByte = seenRetByte;                 // -2 when the returns disagree
    jc->slotKind = expectKind;
    return jc;
}

} // namespace annota
