// The Dalvik bytecode interpreter.
//
// Dalvik is a *register* machine (the JVM is a stack machine). Every method
// declares how many registers it needs; the arguments arrive in the last
// registers ("ins"). An instruction is one or more 16-bit code units; the low
// byte of the first unit is the opcode and the rest encodes register numbers,
// literals and indices in one of ~30 formats named like "22c" (2 units,
// 2 registers, constant-pool index). See
// https://source.android.com/docs/core/runtime/dalvik-bytecode and
// https://source.android.com/docs/core/runtime/instruction-formats
#include <cmath>
#include <iostream>
#include <limits>

#include "vm/vm.h"

namespace droid {

namespace {

// Java semantics for float -> integer conversion: NaN gives 0, out-of-range
// values clamp (C++ leaves these cases undefined).
template <typename To, typename From>
To javaConvert(From v) {
    if (std::isnan(v)) return 0;
    if (v >= From(std::numeric_limits<To>::max())) return std::numeric_limits<To>::max();
    if (v <= From(std::numeric_limits<To>::min())) return std::numeric_limits<To>::min();
    return To(v);
}

template <typename T>
int32_t compareFloat(T a, T b, int32_t nanResult) {
    if (a < b) return -1;
    if (a > b) return 1;
    if (a == b) return 0;
    return nanResult;
}

int32_t idiv(VM& vm, int32_t a, int32_t b) {
    if (b == 0) vm.throwNew("Ljava/lang/ArithmeticException;", "/ by zero");
    if (a == std::numeric_limits<int32_t>::min() && b == -1) return a;
    return a / b;
}

int32_t irem(VM& vm, int32_t a, int32_t b) {
    if (b == 0) vm.throwNew("Ljava/lang/ArithmeticException;", "/ by zero");
    if (b == -1) return 0;
    return a % b;
}

int64_t ldiv(VM& vm, int64_t a, int64_t b) {
    if (b == 0) vm.throwNew("Ljava/lang/ArithmeticException;", "/ by zero");
    if (a == std::numeric_limits<int64_t>::min() && b == -1) return a;
    return a / b;
}

int64_t lrem(VM& vm, int64_t a, int64_t b) {
    if (b == 0) vm.throwNew("Ljava/lang/ArithmeticException;", "/ by zero");
    if (b == -1) return 0;
    return a % b;
}

// The 11 integer binary operations share one order in every opcode group:
// add sub mul div rem and or xor shl shr ushr.
int32_t intOp(VM& vm, int op, int32_t a, int32_t b) {
    switch (op) {
    case 0: return int32_t(uint32_t(a) + uint32_t(b));
    case 1: return int32_t(uint32_t(a) - uint32_t(b));
    case 2: return int32_t(uint32_t(a) * uint32_t(b));
    case 3: return idiv(vm, a, b);
    case 4: return irem(vm, a, b);
    case 5: return a & b;
    case 6: return a | b;
    case 7: return a ^ b;
    case 8: return int32_t(uint32_t(a) << (b & 31));
    case 9: return a >> (b & 31);
    default: return int32_t(uint32_t(a) >> (b & 31));
    }
}

int64_t longOp(VM& vm, int op, int64_t a, int64_t b) {
    switch (op) {
    case 0: return int64_t(uint64_t(a) + uint64_t(b));
    case 1: return int64_t(uint64_t(a) - uint64_t(b));
    case 2: return int64_t(uint64_t(a) * uint64_t(b));
    case 3: return ldiv(vm, a, b);
    case 4: return lrem(vm, a, b);
    case 5: return a & b;
    case 6: return a | b;
    case 7: return a ^ b;
    case 8: return int64_t(uint64_t(a) << (b & 63));
    case 9: return a >> (b & 63);
    default: return int64_t(uint64_t(a) >> (b & 63));
    }
}

template <typename T>
T floatOp(int op, T a, T b) {
    switch (op) {
    case 0: return a + b;
    case 1: return a - b;
    case 2: return a * b;
    case 3: return a / b;
    default: return std::fmod(a, b);
    }
}

// Keeps vm.frames in sync with the C++ call stack, even when exceptions unwind it.
struct FrameGuard {
    VM& vm;
    FrameGuard(VM& v, Frame* f) : vm(v) { vm.frames.push_back(f); }
    ~FrameGuard() { vm.frames.pop_back(); }
};

}  // namespace

Reg VM::interpret(Method* method, Reg* args) {
    const DexCode& code = method->code;
    const uint16_t* insns = code.insns;
    const DexFile* dex = method->dex;

    if (frames.size() > 2000) throwNew("Ljava/lang/StackOverflowError;");

    // Registers. The arguments are copied into the last `ins` registers.
    std::vector<Reg> regsStore(code.registers, 0);
    Reg* regs = regsStore.data();
    for (uint16_t i = 0; i < code.ins; ++i) regs[code.registers - code.ins + i] = args[i];

    Frame frame{method, 0, regs, code.registers};
    FrameGuard guard(*this, &frame);

    uint32_t pc = 0;
    Reg result = 0;               // the value move-result picks up
    Object* pendingException = nullptr;  // the value move-exception picks up
    Reg argv[256];

    for (;;) {
        try {
            for (;;) {
                if (pc >= code.insnsSize) throwNew("Ljava/lang/VerifyError;", "pc out of range in " + method->prettyName());
                frame.pc = pc;
                const uint16_t inst = insns[pc];
                const uint8_t op = inst & 0xff;
                const uint32_t AA = inst >> 8;          // 8-bit register/literal
                const uint32_t A = (inst >> 8) & 0xf;   // 4-bit register
                const uint32_t B = inst >> 12;          // 4-bit register
                if (trace) std::cerr << "  " << method->prettyName() << " pc=" << pc << " op=0x" << std::hex << int(op) << std::dec << "\n";

                auto u16 = [&](uint32_t off) -> uint32_t { return insns[pc + off]; };
                auto s16 = [&](uint32_t off) -> int32_t { return int16_t(insns[pc + off]); };
                auto u32 = [&](uint32_t off) -> uint32_t { return insns[pc + off] | (uint32_t(insns[pc + off + 1]) << 16); };

                auto obj = [&](uint32_t r) { return asObj(regs[r]); };
                auto nonNull = [&](uint32_t r, const char* what) {
                    Object* o = asObj(regs[r]);
                    if (!o) throwNPE(what);
                    return o;
                };
                auto array = [&](uint32_t r, int32_t index) {
                    auto* a = static_cast<ArrayObject*>(nonNull(r, "array access on null"));
                    if (index < 0 || size_t(index) >= a->data.size())
                        throwNew("Ljava/lang/ArrayIndexOutOfBoundsException;",
                                 "length=" + std::to_string(a->data.size()) + "; index=" + std::to_string(index));
                    return a;
                };

                switch (op) {
                // --- moves -------------------------------------------------
                case 0x00: pc += 1; break;  // nop
                case 0x01: case 0x04: case 0x07: regs[A] = regs[B]; pc += 1; break;
                case 0x02: case 0x05: case 0x08: regs[AA] = regs[u16(1)]; pc += 2; break;
                case 0x03: case 0x06: case 0x09: regs[u16(1)] = regs[u16(2)]; pc += 3; break;
                case 0x0a: case 0x0b: case 0x0c: regs[AA] = result; pc += 1; break;
                case 0x0d: regs[AA] = fromObj(pendingException); pc += 1; break;

                // --- returns -----------------------------------------------
                case 0x0e: return 0;
                case 0x0f: case 0x10: case 0x11: return regs[AA];

                // --- constants ---------------------------------------------
                case 0x12: regs[A] = fromInt(int16_t(inst) >> 12); pc += 1; break;  // const/4: sign-extended high nibble
                case 0x13: regs[AA] = fromInt(s16(1)); pc += 2; break;
                case 0x14: regs[AA] = fromInt(int32_t(u32(1))); pc += 3; break;
                case 0x15: regs[AA] = fromInt(int32_t(u16(1) << 16)); pc += 2; break;
                case 0x16: regs[AA] = fromLong(s16(1)); pc += 2; break;
                case 0x17: regs[AA] = fromLong(int32_t(u32(1))); pc += 3; break;
                case 0x18: regs[AA] = Reg(u32(1)) | (Reg(u32(3)) << 32); pc += 5; break;
                case 0x19: regs[AA] = Reg(u16(1)) << 48; pc += 2; break;
                case 0x1a: regs[AA] = fromObj(resolveString(dex, u16(1))); pc += 2; break;
                case 0x1b: regs[AA] = fromObj(resolveString(dex, u32(1))); pc += 3; break;
                case 0x1c: regs[AA] = fromObj(classMirror(resolveType(dex, u16(1)))); pc += 2; break;

                // --- monitors (single threaded: no-ops), type checks --------
                case 0x1d: case 0x1e: pc += 1; break;
                case 0x1f: {  // check-cast
                    Object* o = obj(AA);
                    Class* c = resolveType(dex, u16(1));
                    if (o && !c->isStub && !o->klass->isAssignableTo(c))
                        throwNew("Ljava/lang/ClassCastException;", o->klass->javaName() + " cannot be cast to " + c->javaName());
                    pc += 2;
                    break;
                }
                case 0x20: {  // instance-of
                    Object* o = obj(B);
                    Class* c = resolveType(dex, u16(1));
                    regs[A] = fromInt(o && o->klass->isAssignableTo(c));
                    pc += 2;
                    break;
                }
                case 0x21: regs[A] = fromInt(int32_t(static_cast<ArrayObject*>(nonNull(B, "array-length on null"))->data.size())); pc += 1; break;

                // --- allocation --------------------------------------------
                case 0x22: regs[AA] = fromObj(alloc(resolveType(dex, u16(1)))); pc += 2; break;
                case 0x23: regs[A] = fromObj(newArray(resolveType(dex, u16(1)), asInt(regs[B]))); pc += 2; break;
                case 0x24: case 0x25: {  // filled-new-array(/range)
                    uint32_t count = op == 0x24 ? B : AA;
                    ArrayObject* a = newArray(resolveType(dex, u16(1)), int32_t(count));
                    for (uint32_t i = 0; i < count; ++i) {
                        uint32_t r = op == 0x24 ? (u16(2) >> (4 * i)) & 0xf : u16(2) + i;
                        a->data[i] = regs[r];
                    }
                    result = fromObj(a);
                    pc += 3;
                    break;
                }
                case 0x26: {  // fill-array-data: the data lives in a payload after the code
                    const uint16_t* p = insns + pc + int32_t(u32(1));
                    uint16_t width = p[1];
                    uint32_t size = p[2] | (uint32_t(p[3]) << 16);
                    auto* a = static_cast<ArrayObject*>(nonNull(AA, "fill-array-data on null"));
                    if (size > a->data.size()) throwNew("Ljava/lang/ArrayIndexOutOfBoundsException;");
                    const uint8_t* bytes = reinterpret_cast<const uint8_t*>(p + 4);
                    char kind = a->klass->component->descriptor[0];
                    for (uint32_t i = 0; i < size; ++i) {
                        const uint8_t* e = bytes + size_t(i) * width;
                        switch (width) {
                        case 1: a->data[i] = fromInt(kind == 'Z' ? e[0] : int8_t(e[0])); break;
                        case 2: { uint16_t v; std::memcpy(&v, e, 2); a->data[i] = fromInt(kind == 'C' ? int32_t(v) : int16_t(v)); break; }
                        case 4: { uint32_t v; std::memcpy(&v, e, 4); a->data[i] = v; break; }
                        default: { uint64_t v; std::memcpy(&v, e, 8); a->data[i] = v; break; }
                        }
                    }
                    pc += 3;
                    break;
                }
                case 0x27: {  // throw
                    Object* ex = nonNull(AA, "throw null");
                    throw JavaThrow{ex};
                }

                // --- branches ----------------------------------------------
                case 0x28: pc += int8_t(AA); break;
                case 0x29: pc += s16(1); break;
                case 0x2a: pc += int32_t(u32(1)); break;
                case 0x2b: {  // packed-switch: keys first_key, first_key+1, ...
                    const uint16_t* p = insns + pc + int32_t(u32(1));
                    uint16_t size = p[1];
                    int32_t first = int32_t(p[2] | (uint32_t(p[3]) << 16));
                    int64_t idx = int64_t(asInt(regs[AA])) - first;
                    if (idx >= 0 && idx < size) {
                        const uint16_t* t = p + 4 + 2 * idx;
                        pc += int32_t(t[0] | (uint32_t(t[1]) << 16));
                    } else {
                        pc += 3;
                    }
                    break;
                }
                case 0x2c: {  // sparse-switch: sorted key list + target list
                    const uint16_t* p = insns + pc + int32_t(u32(1));
                    uint16_t size = p[1];
                    int32_t key = asInt(regs[AA]);
                    uint32_t next = pc + 3;
                    for (uint16_t i = 0; i < size; ++i) {
                        int32_t k = int32_t(p[2 + 2 * i] | (uint32_t(p[3 + 2 * i]) << 16));
                        if (k == key) {
                            const uint16_t* t = p + 2 + 2 * size + 2 * i;
                            next = pc + int32_t(t[0] | (uint32_t(t[1]) << 16));
                            break;
                        }
                    }
                    pc = next;
                    break;
                }

                // --- comparisons -------------------------------------------
                case 0x2d: case 0x2e: case 0x2f: case 0x30: case 0x31: {
                    uint32_t b = u16(1) & 0xff, c = u16(1) >> 8;
                    int32_t r;
                    if (op == 0x2d) r = compareFloat(asFloat(regs[b]), asFloat(regs[c]), -1);
                    else if (op == 0x2e) r = compareFloat(asFloat(regs[b]), asFloat(regs[c]), 1);
                    else if (op == 0x2f) r = compareFloat(asDouble(regs[b]), asDouble(regs[c]), -1);
                    else if (op == 0x30) r = compareFloat(asDouble(regs[b]), asDouble(regs[c]), 1);
                    else r = asLong(regs[b]) < asLong(regs[c]) ? -1 : asLong(regs[b]) > asLong(regs[c]) ? 1 : 0;
                    regs[AA] = fromInt(r);
                    pc += 2;
                    break;
                }
                case 0x32: case 0x33: case 0x34: case 0x35: case 0x36: case 0x37: {  // if-test vA, vB
                    int32_t a = asInt(regs[A]), b = asInt(regs[B]);
                    // if-eq/ne also compare references, so compare whole registers
                    // (ints are always stored zero-extended, so this works for both).
                    bool eq = regs[A] == regs[B];
                    bool take = op == 0x32 ? eq : op == 0x33 ? !eq : op == 0x34 ? a < b
                              : op == 0x35 ? a >= b : op == 0x36 ? a > b : a <= b;
                    pc += take ? s16(1) : 2;
                    break;
                }
                case 0x38: case 0x39: case 0x3a: case 0x3b: case 0x3c: case 0x3d: {  // if-testz vAA
                    Reg r = regs[AA];
                    int32_t a = asInt(r);
                    bool zero = r == 0;
                    bool take = op == 0x38 ? zero : op == 0x39 ? !zero : op == 0x3a ? a < 0
                              : op == 0x3b ? a >= 0 : op == 0x3c ? a > 0 : a <= 0;
                    pc += take ? s16(1) : 2;
                    break;
                }

                // --- arrays ------------------------------------------------
                case 0x44: case 0x45: case 0x46: case 0x47: case 0x48: case 0x49: case 0x4a: {
                    uint32_t b = u16(1) & 0xff, c = u16(1) >> 8;
                    regs[AA] = array(b, asInt(regs[c]))->data[size_t(asInt(regs[c]))];
                    pc += 2;
                    break;
                }
                case 0x4b: case 0x4c: case 0x4d: case 0x4e: case 0x4f: case 0x50: case 0x51: {
                    uint32_t b = u16(1) & 0xff, c = u16(1) >> 8;
                    ArrayObject* a = array(b, asInt(regs[c]));
                    Reg v = regs[AA];
                    if (op == 0x4e) v = fromInt(asInt(v) != 0);
                    else if (op == 0x4f) v = fromInt(int8_t(asInt(v)));
                    else if (op == 0x50) v = fromInt(uint16_t(asInt(v)));
                    else if (op == 0x51) v = fromInt(int16_t(asInt(v)));
                    a->data[size_t(asInt(regs[c]))] = v;
                    pc += 2;
                    break;
                }

                // --- instance fields ---------------------------------------
                case 0x52: case 0x53: case 0x54: case 0x55: case 0x56: case 0x57: case 0x58: {
                    Field* f = resolveField(dex, u16(1));
                    Object* o = nonNull(B, "reading a field of null");
                    regs[A] = f->isStatic ? f->staticValue : o->fields[f->slot];
                    pc += 2;
                    break;
                }
                case 0x59: case 0x5a: case 0x5b: case 0x5c: case 0x5d: case 0x5e: case 0x5f: {
                    Field* f = resolveField(dex, u16(1));
                    Object* o = nonNull(B, "writing a field of null");
                    Reg v = regs[A];
                    if (op == 0x5d) v = fromInt(int8_t(asInt(v)));
                    else if (op == 0x5e) v = fromInt(uint16_t(asInt(v)));
                    else if (op == 0x5f) v = fromInt(int16_t(asInt(v)));
                    (f->isStatic ? f->staticValue : o->fields[f->slot]) = v;
                    pc += 2;
                    break;
                }

                // --- static fields -----------------------------------------
                case 0x60: case 0x61: case 0x62: case 0x63: case 0x64: case 0x65: case 0x66: {
                    Field* f = resolveField(dex, u16(1));
                    ensureInitialized(f->owner);
                    regs[AA] = f->staticValue;
                    pc += 2;
                    break;
                }
                case 0x67: case 0x68: case 0x69: case 0x6a: case 0x6b: case 0x6c: case 0x6d: {
                    Field* f = resolveField(dex, u16(1));
                    ensureInitialized(f->owner);
                    f->staticValue = regs[AA];
                    pc += 2;
                    break;
                }

                // --- invocations -------------------------------------------
                case 0x6e: case 0x6f: case 0x70: case 0x71: case 0x72:
                case 0x74: case 0x75: case 0x76: case 0x77: case 0x78: {
                    bool range = op >= 0x74;
                    int kind = range ? op - 0x74 : op - 0x6e;  // 0 virtual 1 super 2 direct 3 static 4 interface
                    uint32_t count = range ? AA : B;
                    uint32_t methodIdx = u16(1);
                    for (uint32_t i = 0; i < count; ++i) {
                        uint32_t r;
                        if (range) r = u16(2) + i;
                        else if (i < 4) r = (u16(2) >> (4 * i)) & 0xf;
                        else r = A;  // the fifth argument register lives in the first unit
                        argv[i] = regs[r];
                    }
                    Method* ref = resolveMethod(dex, methodIdx);
                    Method* target = ref;
                    if (kind == 3) {
                        ensureInitialized(ref->owner);
                    } else {
                        Object* self = asObj(argv[0]);
                        if (!self) throwNPE("calling " + ref->prettyName() + " on null");
                        if (kind == 0 || kind == 4) {
                            if (Method* m = self->klass->dispatch(ref)) target = m;
                        } else if (kind == 1) {
                            Class* start = resolveType(dex, dex->method(methodIdx).classIdx);
                            if (Method* m = start->findVirtual(ref->name, ref->signature)) target = m;
                        }
                    }
                    result = invoke(target, argv);
                    pc += 3;
                    break;
                }

                // --- unary operations and conversions ----------------------
                case 0x7b: regs[A] = fromInt(int32_t(0u - uint32_t(asInt(regs[B])))); pc += 1; break;
                case 0x7c: regs[A] = fromInt(~asInt(regs[B])); pc += 1; break;
                case 0x7d: regs[A] = fromLong(int64_t(0ull - uint64_t(asLong(regs[B])))); pc += 1; break;
                case 0x7e: regs[A] = fromLong(~asLong(regs[B])); pc += 1; break;
                case 0x7f: regs[A] = fromFloat(-asFloat(regs[B])); pc += 1; break;
                case 0x80: regs[A] = fromDouble(-asDouble(regs[B])); pc += 1; break;
                case 0x81: regs[A] = fromLong(asInt(regs[B])); pc += 1; break;
                case 0x82: regs[A] = fromFloat(float(asInt(regs[B]))); pc += 1; break;
                case 0x83: regs[A] = fromDouble(double(asInt(regs[B]))); pc += 1; break;
                case 0x84: regs[A] = fromInt(int32_t(asLong(regs[B]))); pc += 1; break;
                case 0x85: regs[A] = fromFloat(float(asLong(regs[B]))); pc += 1; break;
                case 0x86: regs[A] = fromDouble(double(asLong(regs[B]))); pc += 1; break;
                case 0x87: regs[A] = fromInt(javaConvert<int32_t>(asFloat(regs[B]))); pc += 1; break;
                case 0x88: regs[A] = fromLong(javaConvert<int64_t>(asFloat(regs[B]))); pc += 1; break;
                case 0x89: regs[A] = fromDouble(double(asFloat(regs[B]))); pc += 1; break;
                case 0x8a: regs[A] = fromInt(javaConvert<int32_t>(asDouble(regs[B]))); pc += 1; break;
                case 0x8b: regs[A] = fromLong(javaConvert<int64_t>(asDouble(regs[B]))); pc += 1; break;
                case 0x8c: regs[A] = fromFloat(float(asDouble(regs[B]))); pc += 1; break;
                case 0x8d: regs[A] = fromInt(int8_t(asInt(regs[B]))); pc += 1; break;
                case 0x8e: regs[A] = fromInt(uint16_t(asInt(regs[B]))); pc += 1; break;
                case 0x8f: regs[A] = fromInt(int16_t(asInt(regs[B]))); pc += 1; break;

                // --- binary operations: vAA = vBB op vCC -------------------
                case 0x90 ... 0xaf: {
                    uint32_t b = u16(1) & 0xff, c = u16(1) >> 8;
                    if (op <= 0x9a) regs[AA] = fromInt(intOp(*this, op - 0x90, asInt(regs[b]), asInt(regs[c])));
                    else if (op <= 0xa5) regs[AA] = fromLong(longOp(*this, op - 0x9b, asLong(regs[b]),
                                                                     op >= 0xa3 ? asInt(regs[c]) : asLong(regs[c])));
                    else if (op <= 0xaa) regs[AA] = fromFloat(floatOp(op - 0xa6, asFloat(regs[b]), asFloat(regs[c])));
                    else regs[AA] = fromDouble(floatOp(op - 0xab, asDouble(regs[b]), asDouble(regs[c])));
                    pc += 2;
                    break;
                }
                // --- binary operations, two-address: vA = vA op vB --------
                case 0xb0 ... 0xcf: {
                    if (op <= 0xba) regs[A] = fromInt(intOp(*this, op - 0xb0, asInt(regs[A]), asInt(regs[B])));
                    else if (op <= 0xc5) regs[A] = fromLong(longOp(*this, op - 0xbb, asLong(regs[A]),
                                                                    op >= 0xc3 ? asInt(regs[B]) : asLong(regs[B])));
                    else if (op <= 0xca) regs[A] = fromFloat(floatOp(op - 0xc6, asFloat(regs[A]), asFloat(regs[B])));
                    else regs[A] = fromDouble(floatOp(op - 0xcb, asDouble(regs[A]), asDouble(regs[B])));
                    pc += 1;
                    break;
                }
                // --- binary operations with a literal ----------------------
                case 0xd0 ... 0xd7: {  // vA = vB op #+CCCC
                    int sub = op - 0xd0;
                    int32_t lit = s16(1), v = asInt(regs[B]);
                    regs[A] = fromInt(sub == 1 ? int32_t(uint32_t(lit) - uint32_t(v)) : intOp(*this, sub, v, lit));
                    pc += 2;
                    break;
                }
                case 0xd8 ... 0xe2: {  // vAA = vBB op #+CC
                    int sub = op - 0xd8;
                    uint32_t b = u16(1) & 0xff;
                    int32_t lit = int8_t(u16(1) >> 8), v = asInt(regs[b]);
                    regs[AA] = fromInt(sub == 1 ? int32_t(uint32_t(lit) - uint32_t(v)) : intOp(*this, sub, v, lit));
                    pc += 2;
                    break;
                }

                default:
                    throwNew("Ljava/lang/VerifyError;",
                             "unsupported opcode 0x" + [&] { char b[8]; snprintf(b, sizeof b, "%02x", op); return std::string(b); }() +
                             " in " + method->prettyName());
                }
            }
        } catch (JavaThrow& t) {
            // Find a matching catch handler covering the current instruction.
            int64_t handler = -1;
            for (const DexTry& tr : code.tries) {
                if (frame.pc < tr.start || frame.pc >= tr.start + tr.count) continue;
                for (auto& [typeIdx, addr] : tr.handlers) {
                    if (t.exception->klass->isAssignableTo(resolveType(dex, typeIdx))) { handler = addr; break; }
                }
                if (handler < 0) handler = tr.catchAll;
                break;
            }
            if (handler < 0) throw;  // not ours: unwind to the caller
            pendingException = t.exception;
            pc = uint32_t(handler);
        }
    }
}

}  // namespace droid
