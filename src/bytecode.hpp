// Annota - bytecode.hpp : opcodes and code chunks.
#pragma once
#include "value.hpp"

namespace annota {

struct JitCode;                    // src/jit.hpp (the [[jit]] machine code backend)

enum Op : uint8_t {
    OP_NOP = 0,
    OP_CONST,             // u16 const index
    OP_NULL, OP_TRUE, OP_FALSE,
    OP_INT1,              // s8 small integer
    OP_POP, OP_DUP, OP_DUP2, OP_SWAP,
    OP_GET_LOCAL,         // u8 slot
    OP_SET_LOCAL,         // u8 slot          (deep copy)
    OP_INIT_LOCAL,        // u8 slot
    OP_DEL_LOCAL,         // u8 slot
    OP_GET_UPVAL,         // u8
    OP_SET_UPVAL,         // u8
    OP_GET_GLOBAL,        // u16 name const (falls back to a field of `this`)
    OP_SET_GLOBAL,        // u16 name const
    OP_DEF_GLOBAL,        // u16 name const
    OP_DEL_GLOBAL,        // u16 name const
    OP_GET_FIELD,         // u16 name const
    OP_SET_FIELD,         // u16 name const   (deep copy)
    OP_GET_INDEX, OP_SET_INDEX,
    OP_GET_SUPER,         // u16 name const, u16 owner-class const
    OP_SUPER_INIT,        // u16 owner-class const
    OP_ADD, OP_SUB, OP_MUL, OP_DIV, OP_MOD, OP_POW,
    OP_EQ, OP_NE, OP_LT, OP_GT, OP_LE, OP_GE,
    OP_BAND, OP_BOR, OP_BXOR, OP_SHL, OP_SHR,
    OP_NEG, OP_NOT, OP_BNOT,
    OP_JUMP,              // s16
    OP_JUMP_IF_FALSE,     // s16 (pops)
    OP_JUMP_IF_FALSE_KEEP,// s16 (keeps the value, used by &&)
    OP_JUMP_IF_TRUE_KEEP, // s16 (keeps the value, used by ||)
    OP_LOOP,              // s16 backwards
    OP_CALL,
    OP_CALL_UI,
    OP_RETURN,
    OP_RETURN_NULL,
    OP_CLOSURE,           // u16 function const index, then upvalue descriptors
    OP_BUILD_LIST,        // u16 n
    OP_BUILD_TUPLE,       // u16 n
    OP_LIST_APPEND,       // pops value, appends to the list on top
    OP_LIST_EXTEND,       // pops iterable, appends its elements to the list on top
    OP_BUILD_NAMED,       // u16 n  (pops 2n: name, value)
    OP_ITER_RANGE,        // pops start, stop
    OP_ITER_RANGE_STEP,   // pops start, stop, step
    OP_ITER_VALUE,        // pops any iterable
    OP_ITER_NEXT,         // s16 jump when exhausted, else pushes the next element
    OP_ITER_BIND2,        // u8 slotA, u8 slotB : destructure the element (falls back to index,item)
    OP_THROW,
    OP_TRY,               // s16 handler offset
    OP_POP_TRY,
    OP_PRINT,             // u8 count, u8 hasSep
    OP_INPUT,
    OP_JUMP_IF_PROVIDED,  // u8 param index, s16 target
    OP_COLLECT_VARARGS,   // u8 slot, u8 fixed count
    OP_UI_APPEND,         // u8 slot : append the popped value to the UI list
    OP_ASSERT,            // u16 message const
    OP_CLASS,             // u16 class-template const
    OP_INIT_CLASS,        // run the static initialiser of the class on top of the stack
    OP_DEEPCOPY,
    OP_CONVERT,           // u8 NumKind : numeric width conversion
    OP_NEW_ARRAY,         // u8 ndims, u8 info (bit0 init list, bit1 eltIsStr, bit2 dynamic,
                          //            bits 3..7 eltKind+1) : build T[n] / T[] arrays
    OP_GET_INDEX_FAST,    // like OP_GET_INDEX but without the bounds check (proved by the
                          // compiler: literal index in range, or the statement is [[unsafe]])
    OP_LOCAL_ADD_IMM,     // u8 slot, i8 imm   : slot = slot + imm        (fused under [[jit]])
    OP_LOCAL_SUB_IMM,     // u8 slot, i8 imm   : slot = slot - imm
    OP_LOCAL_ADD_LOCAL,   // u8 slot, u8 src   : slot = slot + src
    OP_JUMP_IF_NOT_LT_LOCAL_LOCAL,  // u8 a, u8 b, s16 target : if !(locals[a] < locals[b]) jump
    OP_JUMP_IF_NOT_LT_LOCAL_IMM,    // u8 a, i8 imm, s16 target: if !(locals[a] < imm) jump
    OP_INDEX_ADD_IMM,               // u8 arr, u8 idx, i8 imm   : arr[idx] = arr[idx] + imm
    OP_LAST
};

struct UpvalDesc { uint8_t fromLocal; uint8_t index; };

struct ParamInfo {
    std::string name;
    bool hasDefault = false;
    bool vararg = false;
};

struct Chunk {
    std::shared_ptr<JitCode> jit;       // non-null when the function was translated to machine code
    std::string file;
    std::string fnName;
    std::vector<uint8_t> code;
    std::vector<int> lines;
    std::vector<Value> consts;
    std::vector<UpvalDesc> upvals;
    std::vector<ParamInfo> params;
    int numLocals = 0;
    int fixedCount = 0;          // named parameters (variadic not included)
    int varargSlot = -1;
    bool isMethod = false;       // locals[0] is `this`
    std::vector<Value> protos;   // nested function prototypes
};

const char* opName(uint8_t op);

} // namespace annota
