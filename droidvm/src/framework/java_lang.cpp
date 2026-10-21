// Native implementations of the core Java library (java.lang, java.util).
//
// A real Android device runs the full libcore written in Java. We implement
// a useful subset directly in C++, which keeps the emulator small and fast.
// Anything missing becomes a stub that logs "not implemented" when called.
#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cstring>
#include <functional>
#include <cmath>
#include <cstdio>
#include <iostream>
#include <thread>

#include "framework/natives.h"

namespace droid {

// ------------------------------------------------------ number formatting

std::string javaDoubleToString(double v, bool isFloat) {
    if (std::isnan(v)) return "NaN";
    if (std::isinf(v)) return v > 0 ? "Infinity" : "-Infinity";
    if (v == 0) return std::signbit(v) ? "-0.0" : "0.0";

    // Find the shortest precision that reads back as the same value.
    char buf[64];
    int maxPrec = isFloat ? 9 : 17;
    int prec = 1;
    for (; prec < maxPrec; ++prec) {
        snprintf(buf, sizeof buf, "%.*e", prec - 1, v);
        double back = std::strtod(buf, nullptr);
        if (isFloat ? float(back) == float(v) : back == v) break;
    }
    snprintf(buf, sizeof buf, "%.*e", prec - 1, v);
    // buf looks like "-1.2345e+03": split into sign, digits and exponent.
    std::string s(buf);
    bool neg = s[0] == '-';
    if (neg) s = s.substr(1);
    size_t e = s.find('e');
    int exp = std::atoi(s.c_str() + e + 1);
    std::string digits;
    for (size_t i = 0; i < e; ++i)
        if (s[i] != '.') digits += s[i];
    while (digits.size() > 1 && digits.back() == '0') digits.pop_back();

    std::string out;
    double mag = std::fabs(v);
    if (mag >= 1e-3 && mag < 1e7) {
        if (exp >= 0) {
            std::string ip = digits.substr(0, std::min<size_t>(digits.size(), size_t(exp) + 1));
            while (ip.size() < size_t(exp) + 1) ip += '0';
            std::string fp = digits.size() > size_t(exp) + 1 ? digits.substr(size_t(exp) + 1) : "0";
            out = ip + "." + fp;
        } else {
            out = "0." + std::string(size_t(-exp - 1), '0') + digits;
        }
    } else {
        out = digits.substr(0, 1) + "." + (digits.size() > 1 ? digits.substr(1) : "0") + "E" + std::to_string(exp);
    }
    return neg ? "-" + out : out;
}

Reg unboxValue(Object* box) {
    if (!box) return 0;
    Field* f = box->klass->findField("value");
    return f && !f->isStatic ? box->fields[f->slot] : 0;
}

std::u16string javaFormat(VM& vm, const std::u16string& fmt, Object* argsArray) {
    auto* args = static_cast<ArrayObject*>(argsArray);
    size_t next = 0;
    std::string out;
    std::string f = utf16ToUtf8(fmt);
    for (size_t i = 0; i < f.size(); ++i) {
        if (f[i] != '%') { out += f[i]; continue; }
        size_t j = i + 1;
        std::string spec = "%";
        while (j < f.size() && std::strchr("-+ 0#,.123456789", f[j])) {
            if (f[j] != ',') spec += f[j];
            ++j;
        }
        if (j >= f.size()) break;
        char conv = f[j];
        i = j;
        if (conv == '%') { out += '%'; continue; }
        if (conv == 'n') { out += '\n'; continue; }
        Object* arg = args && next < args->data.size() ? asObj(args->data[next++]) : nullptr;
        char buf[512];
        std::string cls = arg ? arg->klass->descriptor : "";
        bool isFloatBox = cls == "Ljava/lang/Float;" || cls == "Ljava/lang/Double;";
        auto numberAsDouble = [&]() -> double {
            Reg v = unboxValue(arg);
            if (cls == "Ljava/lang/Float;") return asFloat(v);
            if (cls == "Ljava/lang/Double;") return asDouble(v);
            if (cls == "Ljava/lang/Long;") return double(asLong(v));
            return asInt(v);
        };
        auto numberAsLong = [&]() -> long long {
            Reg v = unboxValue(arg);
            if (cls == "Ljava/lang/Long;") return asLong(v);
            if (isFloatBox) return (long long)numberAsDouble();
            return asInt(v);
        };
        switch (conv) {
        case 'd': snprintf(buf, sizeof buf, (spec + "lld").c_str(), numberAsLong()); out += buf; break;
        case 'x': case 'X': {
            long long v = numberAsLong();
            if (cls != "Ljava/lang/Long;") v = (long long)uint32_t(v);
            snprintf(buf, sizeof buf, (spec + "ll" + conv).c_str(), v);
            out += buf;
            break;
        }
        case 'f': case 'e': case 'g':
            snprintf(buf, sizeof buf, (spec + conv).c_str(), numberAsDouble());
            out += buf;
            break;
        case 'c': out += utf16ToUtf8(std::u16string(1, char16_t(asInt(unboxValue(arg))))); break;
        case 'b': out += arg ? (cls == "Ljava/lang/Boolean;" ? (asInt(unboxValue(arg)) ? "true" : "false") : "true") : "false"; break;
        default: {  // %s and anything unknown
            std::string s = arg ? vm.stringOf(arg) : "null";
            snprintf(buf, sizeof buf, (spec + "s").c_str(), s.c_str());
            out += buf;
        }
        }
    }
    return utf8ToUtf16(out);
}

namespace {

// ----------------------------------------------------------- peers

struct StringBuilderPeer : NativePeer {
    std::u16string buf;
};

struct ThrowablePeer : NativePeer {
    std::vector<std::string> trace;
};

struct ListPeer : NativePeer {
    std::vector<Object*> items;
    void trace(std::vector<Object*>& out) const override { out.insert(out.end(), items.begin(), items.end()); }
};

struct IteratorPeer : NativePeer {
    Object* list = nullptr;
    size_t index = 0;
    void trace(std::vector<Object*>& out) const override { out.push_back(list); }
};

struct MapPeer : NativePeer {
    std::vector<std::pair<Object*, Object*>> entries;  // insertion order
    void trace(std::vector<Object*>& out) const override {
        for (auto& [k, v] : entries) { out.push_back(k); out.push_back(v); }
    }
};

struct RandomPeer : NativePeer {
    int64_t seed = 0;
    void setSeed(int64_t s) { seed = (s ^ 0x5DEECE66DLL) & ((1LL << 48) - 1); }
    int32_t next(int bits) {  // the exact algorithm of java.util.Random
        seed = (seed * 0x5DEECE66DLL + 0xBLL) & ((1LL << 48) - 1);
        return int32_t(seed >> (48 - bits));
    }
};

struct PrintStreamPeer : NativePeer {
    bool err = false;
};

std::u16string& sb(Reg self) { return asObj(self)->peerAs<StringBuilderPeer>()->buf; }
std::vector<Object*>& items(Reg self) { return asObj(self)->peerAs<ListPeer>()->items; }

int32_t javaHash(const std::u16string& s) {
    uint32_t h = 0;
    for (char16_t c : s) h = 31 * h + c;
    return int32_t(h);
}

bool javaEquals(VM& vm, Object* a, Object* b) {
    if (a == b) return true;
    if (!a || !b) return false;
    return asInt(vm.callVirtual(a, "equals", "(Ljava/lang/Object;)Z", {fromObj(b)})) != 0;
}

int32_t identityHash(Object* o) { return int32_t(uint32_t(reinterpret_cast<uintptr_t>(o) >> 4)); }

std::u16string toU16(const std::string& s) { return utf8ToUtf16(s); }

}  // namespace

char16_t javaToUpper(char16_t c) {
    if (c >= 'a' && c <= 'z') return char16_t(c - 32);
    if (c >= 0xe0 && c <= 0xfe && c != 0xf7) return char16_t(c - 32);
    if (c >= 0x100 && c <= 0x17f) {
        // Latin Extended-A mostly pairs upper (even) / lower (odd), except a block that is shifted.
        bool shifted = (c >= 0x139 && c <= 0x148) || (c >= 0x179 && c <= 0x17e);
        if (shifted ? (c % 2 == 0) : (c % 2 == 1)) return char16_t(c - 1);
    }
    return c;
}

char16_t javaToLower(char16_t c) {
    if (c >= 'A' && c <= 'Z') return char16_t(c + 32);
    if (c >= 0xc0 && c <= 0xde && c != 0xd7) return char16_t(c + 32);
    if (c >= 0x100 && c <= 0x17f) {
        bool shifted = (c >= 0x139 && c <= 0x148) || (c >= 0x179 && c <= 0x17e);
        if (shifted ? (c % 2 == 1) : (c % 2 == 0)) return char16_t(c + 1);
    }
    return c;
}

namespace {

char16_t toUpper(char16_t c) { return javaToUpper(c); }
char16_t toLower(char16_t c) { return javaToLower(c); }

int32_t parseInt(VM& vm, const std::u16string& s, int radix) {
    std::string u = utf16ToUtf8(s);
    if (u.empty()) vm.throwNew("Ljava/lang/NumberFormatException;", "For input string: \"\"");
    char* end = nullptr;
    errno = 0;
    long long v = std::strtoll(u.c_str(), &end, radix);
    if (*end || errno || v < INT32_MIN || v > INT32_MAX || u[0] == ' ')
        vm.throwNew("Ljava/lang/NumberFormatException;", "For input string: \"" + u + "\"");
    return int32_t(v);
}

std::u16string appendText(VM& vm, char type, Reg* a) {
    switch (type) {
    case 'I': case 'S': case 'B': return toU16(std::to_string(asInt(a[0])));
    case 'J': return toU16(std::to_string(asLong(a[0])));
    case 'F': return toU16(javaDoubleToString(asFloat(a[0]), true));
    case 'D': return toU16(javaDoubleToString(asDouble(a[0]), false));
    case 'C': return std::u16string(1, char16_t(asInt(a[0])));
    case 'Z': return asInt(a[0]) ? u"true" : u"false";
    default: return vm.string16Of(asObj(a[0]));
    }
}

// Defines a box class like java.lang.Integer with valueOf/xxxValue/toString.
void defineBox(VM& vm, const std::string& desc, char prim) {
    Class* c = vm.defineNativeClass(desc, prim == 'Z' || prim == 'C' ? "Ljava/lang/Object;" : "Ljava/lang/Number;",
                                    {"Ljava/lang/Comparable;"}, kPublic | kFinal);
    std::string p(1, prim);
    c->addField("value", p);
    bool wide = prim == 'J' || prim == 'D';
    c->addStaticNative("valueOf", "(" + p + ")" + desc, NATIVE_CAPTURE(c) {
        Object* o = vm.alloc(c);
        o->fields[0] = a[0];
        return fromObj(o);
    });
    c->addNative("<init>", "(" + p + ")V", NATIVE { asObj(a[0])->fields[0] = a[1]; return 0; });
    auto get = [](Reg self) { return asObj(self)->fields[0]; };
    auto asD = [prim](Reg v) -> double {
        switch (prim) {
        case 'J': return double(asLong(v));
        case 'F': return asFloat(v);
        case 'D': return asDouble(v);
        default: return asInt(v);
        }
    };
    c->addNative("intValue", "()I", NATIVE_CAPTURE(get, asD, prim) {
        return prim == 'F' || prim == 'D' ? fromInt(int32_t(asD(get(a[0])))) : fromInt(int32_t(get(a[0])));
    });
    c->addNative("longValue", "()J", NATIVE_CAPTURE(get, asD, prim) {
        return prim == 'J' ? get(a[0]) : fromLong(int64_t(asD(get(a[0]))));
    });
    c->addNative("floatValue", "()F", NATIVE_CAPTURE(get, asD) { return fromFloat(float(asD(get(a[0])))); });
    c->addNative("doubleValue", "()D", NATIVE_CAPTURE(get, asD) { return fromDouble(asD(get(a[0]))); });
    if (prim == 'Z') c->addNative("booleanValue", "()Z", NATIVE_CAPTURE(get) { return get(a[0]); });
    if (prim == 'C') c->addNative("charValue", "()C", NATIVE_CAPTURE(get) { return get(a[0]); });
    c->addNative("toString", "()Ljava/lang/String;", NATIVE_CAPTURE(get, prim) {
        Reg v = get(a[0]);
        return newStr(vm, appendText(vm, prim, &v));
    });
    c->addStaticNative("toString", "(" + p + ")Ljava/lang/String;", NATIVE_CAPTURE(prim) {
        return newStr(vm, appendText(vm, prim, a));
    });
    c->addNative("hashCode", "()I", NATIVE_CAPTURE(get, wide) {
        Reg v = get(a[0]);
        return fromInt(wide ? int32_t(v ^ (v >> 32)) : asInt(v));
    });
    c->addNative("equals", "(Ljava/lang/Object;)Z", NATIVE_CAPTURE(c, get, wide) {
        Object* o = asObj(a[1]);
        if (!o || o->klass != c) return 0;
        Reg x = get(a[0]), y = get(a[1]);
        return wide ? Reg(x == y) : Reg(uint32_t(x) == uint32_t(y));
    });
    c->addNative("compareTo", "(" + desc + ")I", NATIVE_CAPTURE(get, asD) {
        double x = asD(get(a[0])), y = asD(get(a[1]));
        return fromInt(x < y ? -1 : x > y ? 1 : 0);
    });
}

Class* defineThrowable(VM& vm, const std::string& desc, const std::string& super) {
    return vm.defineNativeClass(desc, super);
}

}  // namespace

// ------------------------------------------------------------ java.lang

void registerJavaLang(VM& vm) {
    // --- Object ------------------------------------------------------------
    Class* object = vm.objectClass = vm.defineNativeClass("Ljava/lang/Object;", "");
    object->addNative("<init>", "()V", NATIVE { return 0; });
    object->addNative("hashCode", "()I", NATIVE { return fromInt(identityHash(asObj(a[0]))); });
    object->addNative("equals", "(Ljava/lang/Object;)Z", NATIVE { return a[0] == a[1]; });
    object->addNative("toString", "()Ljava/lang/String;", NATIVE {
        Object* o = asObj(a[0]);
        char buf[32];
        snprintf(buf, sizeof buf, "@%x", uint32_t(asInt(vm.callVirtual(o, "hashCode", "()I"))));
        return newStr(vm, o->klass->javaName() + buf);
    });
    object->addNative("getClass", "()Ljava/lang/Class;", NATIVE { return fromObj(vm.classMirror(asObj(a[0])->klass)); });
    object->addNative("clone", "()Ljava/lang/Object;", NATIVE {
        Object* o = asObj(a[0]);
        if (o->kind == Object::Array) {
            auto* src = static_cast<ArrayObject*>(o);
            ArrayObject* copy = vm.newArray(o->klass, int32_t(src->data.size()));
            copy->data = src->data;
            return fromObj(copy);
        }
        if (o->peer) vm.throwNew("Ljava/lang/CloneNotSupportedException;", o->klass->javaName());
        Object* copy = vm.alloc(o->klass);
        copy->fields = o->fields;
        return fromObj(copy);
    });
    object->addNative("notify", "()V", NATIVE { return 0; });
    object->addNative("notifyAll", "()V", NATIVE { return 0; });
    object->addNative("wait", "()V", NATIVE { return 0; });

    for (const char* i : {"Ljava/lang/CharSequence;", "Ljava/lang/Comparable;", "Ljava/lang/Cloneable;",
                          "Ljava/io/Serializable;", "Ljava/lang/Runnable;", "Ljava/lang/Iterable;",
                          "Ljava/util/Iterator;", "Ljava/util/Collection;", "Ljava/util/Map;",
                          "Ljava/lang/AutoCloseable;"})
        vm.defineNativeClass(i, "Ljava/lang/Object;", {}, kPublic | kInterface | kAbstract);
    vm.defineNativeClass("Ljava/util/List;", "Ljava/lang/Object;", {"Ljava/util/Collection;"}, kPublic | kInterface | kAbstract);
    vm.defineNativeClass("Ljava/util/Set;", "Ljava/lang/Object;", {"Ljava/util/Collection;"}, kPublic | kInterface | kAbstract);

    // --- Class -------------------------------------------------------------
    Class* klass = vm.classClass = vm.defineNativeClass("Ljava/lang/Class;", "Ljava/lang/Object;");
    auto mirrorOf = [](Reg r) { return VM::classFromMirror(asObj(r)); };
    klass->addNative("getName", "()Ljava/lang/String;", NATIVE_CAPTURE(mirrorOf) { return newStr(vm, mirrorOf(a[0])->javaName()); });
    klass->addNative("getSimpleName", "()Ljava/lang/String;", NATIVE_CAPTURE(mirrorOf) {
        std::string n = mirrorOf(a[0])->javaName();
        size_t p = n.find_last_of(".$");
        return newStr(vm, p == std::string::npos ? n : n.substr(p + 1));
    });
    klass->addNative("toString", "()Ljava/lang/String;", NATIVE_CAPTURE(mirrorOf) {
        Class* c = mirrorOf(a[0]);
        return newStr(vm, std::string(c->isInterface() ? "interface " : "class ") + c->javaName());
    });

    // --- String ------------------------------------------------------------
    Class* string = vm.stringClass = vm.defineNativeClass(
        "Ljava/lang/String;", "Ljava/lang/Object;",
        {"Ljava/lang/CharSequence;", "Ljava/lang/Comparable;", "Ljava/io/Serializable;"}, kPublic | kFinal);
    auto self = [](Reg r) -> std::u16string& { return static_cast<StringObject*>(asObj(r))->value; };
    string->addNative("<init>", "()V", NATIVE { return 0; });
    string->addNative("<init>", "(Ljava/lang/String;)V", NATIVE_CAPTURE(self) { self(a[0]) = str(vm, a[1]); return 0; });
    string->addNative("<init>", "([C)V", NATIVE_CAPTURE(self) {
        auto* arr = static_cast<ArrayObject*>(asObj(a[1]));
        if (!arr) vm.throwNPE("char[] is null");
        for (Reg c : arr->data) self(a[0]).push_back(char16_t(c));
        return 0;
    });
    string->addNative("<init>", "([CII)V", NATIVE_CAPTURE(self) {
        auto* arr = static_cast<ArrayObject*>(asObj(a[1]));
        if (!arr) vm.throwNPE("char[] is null");
        for (int32_t i = 0; i < asInt(a[3]); ++i) self(a[0]).push_back(char16_t(arr->data.at(size_t(asInt(a[2]) + i))));
        return 0;
    });
    string->addNative("length", "()I", NATIVE_CAPTURE(self) { return fromInt(int32_t(self(a[0]).size())); });
    string->addNative("isEmpty", "()Z", NATIVE_CAPTURE(self) { return self(a[0]).empty(); });
    string->addNative("charAt", "(I)C", NATIVE_CAPTURE(self) {
        auto& s = self(a[0]);
        int32_t i = asInt(a[1]);
        if (i < 0 || size_t(i) >= s.size())
            vm.throwNew("Ljava/lang/StringIndexOutOfBoundsException;", "index=" + std::to_string(i));
        return s[size_t(i)];
    });
    string->addNative("toString", "()Ljava/lang/String;", NATIVE { return a[0]; });
    string->addNative("hashCode", "()I", NATIVE_CAPTURE(self) { return fromInt(javaHash(self(a[0]))); });
    string->addNative("equals", "(Ljava/lang/Object;)Z", NATIVE_CAPTURE(self) {
        StringObject* o = asString(a[1]);
        return o && o->value == self(a[0]);
    });
    string->addNative("equalsIgnoreCase", "(Ljava/lang/String;)Z", NATIVE_CAPTURE(self) {
        StringObject* o = asString(a[1]);
        if (!o || o->value.size() != self(a[0]).size()) return 0;
        for (size_t i = 0; i < o->value.size(); ++i)
            if (toLower(o->value[i]) != toLower(self(a[0])[i])) return 0;
        return 1;
    });
    string->addNative("compareTo", "(Ljava/lang/String;)I", NATIVE_CAPTURE(self) {
        auto& x = self(a[0]);
        auto& y = str(vm, a[1]);
        size_t n = std::min(x.size(), y.size());
        for (size_t i = 0; i < n; ++i)
            if (x[i] != y[i]) return fromInt(int32_t(x[i]) - int32_t(y[i]));
        return fromInt(int32_t(x.size()) - int32_t(y.size()));
    });
    string->addNative("compareTo", "(Ljava/lang/Object;)I", NATIVE {
        return vm.callVirtual(asObj(a[0]), "compareTo", "(Ljava/lang/String;)I", {a[1]});
    });
    string->addNative("concat", "(Ljava/lang/String;)Ljava/lang/String;", NATIVE_CAPTURE(self) {
        return newStr(vm, self(a[0]) + str(vm, a[1]));
    });
    auto substring = [self](VM& vm, Reg s, int32_t b, int32_t e) {
        auto& v = self(s);
        if (b < 0 || e > int32_t(v.size()) || b > e)
            vm.throwNew("Ljava/lang/StringIndexOutOfBoundsException;",
                        "begin " + std::to_string(b) + ", end " + std::to_string(e) + ", length " + std::to_string(v.size()));
        return newStr(vm, v.substr(size_t(b), size_t(e - b)));
    };
    string->addNative("substring", "(I)Ljava/lang/String;", NATIVE_CAPTURE(self, substring) {
        return substring(vm, a[0], asInt(a[1]), int32_t(self(a[0]).size()));
    });
    string->addNative("substring", "(II)Ljava/lang/String;", NATIVE_CAPTURE(substring) {
        return substring(vm, a[0], asInt(a[1]), asInt(a[2]));
    });
    string->addNative("subSequence", "(II)Ljava/lang/CharSequence;", NATIVE_CAPTURE(substring) {
        return substring(vm, a[0], asInt(a[1]), asInt(a[2]));
    });
    auto indexOf = [](const std::u16string& s, const std::u16string& what, int32_t from) {
        size_t p = s.find(what, size_t(std::max(from, 0)));
        return fromInt(p == std::u16string::npos ? -1 : int32_t(p));
    };
    string->addNative("indexOf", "(Ljava/lang/String;)I", NATIVE_CAPTURE(self, indexOf) { return indexOf(self(a[0]), str(vm, a[1]), 0); });
    string->addNative("indexOf", "(Ljava/lang/String;I)I", NATIVE_CAPTURE(self, indexOf) { return indexOf(self(a[0]), str(vm, a[1]), asInt(a[2])); });
    string->addNative("indexOf", "(I)I", NATIVE_CAPTURE(self, indexOf) { return indexOf(self(a[0]), std::u16string(1, char16_t(asInt(a[1]))), 0); });
    string->addNative("indexOf", "(II)I", NATIVE_CAPTURE(self, indexOf) { return indexOf(self(a[0]), std::u16string(1, char16_t(asInt(a[1]))), asInt(a[2])); });
    string->addNative("lastIndexOf", "(I)I", NATIVE_CAPTURE(self) {
        size_t p = self(a[0]).rfind(char16_t(asInt(a[1])));
        return fromInt(p == std::u16string::npos ? -1 : int32_t(p));
    });
    string->addNative("lastIndexOf", "(Ljava/lang/String;)I", NATIVE_CAPTURE(self) {
        size_t p = self(a[0]).rfind(str(vm, a[1]));
        return fromInt(p == std::u16string::npos ? -1 : int32_t(p));
    });
    string->addNative("contains", "(Ljava/lang/CharSequence;)Z", NATIVE_CAPTURE(self) {
        return self(a[0]).find(vm.string16Of(asObj(a[1]))) != std::u16string::npos;
    });
    string->addNative("startsWith", "(Ljava/lang/String;)Z", NATIVE_CAPTURE(self) { return self(a[0]).rfind(str(vm, a[1]), 0) == 0; });
    string->addNative("endsWith", "(Ljava/lang/String;)Z", NATIVE_CAPTURE(self) {
        auto& s = self(a[0]);
        auto& e = str(vm, a[1]);
        return s.size() >= e.size() && s.compare(s.size() - e.size(), e.size(), e) == 0;
    });
    string->addNative("toUpperCase", "()Ljava/lang/String;", NATIVE_CAPTURE(self) {
        std::u16string s = self(a[0]);
        for (auto& c : s) c = toUpper(c);
        return newStr(vm, s);
    });
    string->addNative("toLowerCase", "()Ljava/lang/String;", NATIVE_CAPTURE(self) {
        std::u16string s = self(a[0]);
        for (auto& c : s) c = toLower(c);
        return newStr(vm, s);
    });
    string->addNative("trim", "()Ljava/lang/String;", NATIVE_CAPTURE(self) {
        auto& s = self(a[0]);
        size_t b = 0, e = s.size();
        while (b < e && s[b] <= ' ') ++b;
        while (e > b && s[e - 1] <= ' ') --e;
        return newStr(vm, s.substr(b, e - b));
    });
    string->addNative("replace", "(CC)Ljava/lang/String;", NATIVE_CAPTURE(self) {
        std::u16string s = self(a[0]);
        for (auto& c : s) if (c == char16_t(asInt(a[1]))) c = char16_t(asInt(a[2]));
        return newStr(vm, s);
    });
    string->addNative("replace", "(Ljava/lang/CharSequence;Ljava/lang/CharSequence;)Ljava/lang/String;", NATIVE_CAPTURE(self) {
        std::u16string s = self(a[0]), from = vm.string16Of(asObj(a[1])), to = vm.string16Of(asObj(a[2]));
        if (from.empty()) return newStr(vm, s);
        std::u16string out;
        size_t pos = 0, hit;
        while ((hit = s.find(from, pos)) != std::u16string::npos) {
            out += s.substr(pos, hit - pos) + to;
            pos = hit + from.size();
        }
        return newStr(vm, out + s.substr(pos));
    });
    // split() takes a regex in Java; we treat it as a literal separator.
    string->addNative("split", "(Ljava/lang/String;)[Ljava/lang/String;", NATIVE_CAPTURE(self) {
        std::u16string s = self(a[0]), sep = str(vm, a[1]);
        if (sep == u"\\s+" || sep == u" +") sep = u" ";
        std::vector<std::u16string> parts;
        size_t pos = 0, hit;
        while (!sep.empty() && (hit = s.find(sep, pos)) != std::u16string::npos) {
            parts.push_back(s.substr(pos, hit - pos));
            pos = hit + sep.size();
        }
        parts.push_back(s.substr(pos));
        while (parts.size() > 1 && parts.back().empty()) parts.pop_back();
        ArrayObject* arr = vm.newArray("[Ljava/lang/String;", int32_t(parts.size()));
        for (size_t i = 0; i < parts.size(); ++i) arr->data[i] = newStr(vm, parts[i]);
        return fromObj(arr);
    });
    string->addNative("toCharArray", "()[C", NATIVE_CAPTURE(self) {
        auto& s = self(a[0]);
        ArrayObject* arr = vm.newArray("[C", int32_t(s.size()));
        for (size_t i = 0; i < s.size(); ++i) arr->data[i] = s[i];
        return fromObj(arr);
    });
    string->addNative("intern", "()Ljava/lang/String;", NATIVE { return a[0]; });
    string->addStaticNative("format", "(Ljava/lang/String;[Ljava/lang/Object;)Ljava/lang/String;", NATIVE {
        return newStr(vm, javaFormat(vm, str(vm, a[0]), asObj(a[1])));
    });
    for (char t : std::string("IJFDCZ")) {
        std::string sig = std::string("(") + t + ")Ljava/lang/String;";
        string->addStaticNative("valueOf", sig, NATIVE_CAPTURE(t) { return newStr(vm, appendText(vm, t, a)); });
    }
    string->addStaticNative("valueOf", "(Ljava/lang/Object;)Ljava/lang/String;", NATIVE { return newStr(vm, vm.string16Of(asObj(a[0]))); });
    string->addStaticNative("valueOf", "([C)Ljava/lang/String;", NATIVE {
        std::u16string s;
        for (Reg c : static_cast<ArrayObject*>(asObj(a[0]))->data) s.push_back(char16_t(c));
        return newStr(vm, s);
    });

    // --- StringBuilder -------------------------------------------------------
    for (const char* name : {"Ljava/lang/StringBuilder;", "Ljava/lang/StringBuffer;"}) {
        std::string desc = name;
        Class* b = vm.defineNativeClass(desc, "Ljava/lang/Object;", {"Ljava/lang/CharSequence;"}, kPublic | kFinal);
        b->peerFactory = [] { return std::make_unique<StringBuilderPeer>(); };
        b->addNative("<init>", "()V", NATIVE { return 0; });
        b->addNative("<init>", "(I)V", NATIVE { return 0; });
        b->addNative("<init>", "(Ljava/lang/String;)V", NATIVE { sb(a[0]) = str(vm, a[1]); return 0; });
        b->addNative("<init>", "(Ljava/lang/CharSequence;)V", NATIVE { sb(a[0]) = vm.string16Of(asObj(a[1])); return 0; });
        for (std::string t : {"I", "J", "F", "D", "C", "Z", "Ljava/lang/String;", "Ljava/lang/Object;", "Ljava/lang/CharSequence;"}) {
            char kind = t[0];
            b->addNative("append", "(" + t + ")" + desc, NATIVE_CAPTURE(kind) {
                sb(a[0]) += appendText(vm, kind, a + 1);
                return a[0];
            });
        }
        b->addNative("append", "([C)" + desc, NATIVE {
            for (Reg c : static_cast<ArrayObject*>(asObj(a[1]))->data) sb(a[0]).push_back(char16_t(c));
            return a[0];
        });
        b->addNative("toString", "()Ljava/lang/String;", NATIVE { return newStr(vm, sb(a[0])); });
        b->addNative("length", "()I", NATIVE { return fromInt(int32_t(sb(a[0]).size())); });
        b->addNative("charAt", "(I)C", NATIVE { return sb(a[0]).at(size_t(asInt(a[1]))); });
        b->addNative("setLength", "(I)V", NATIVE { sb(a[0]).resize(size_t(asInt(a[1])), u'\0'); return 0; });
        b->addNative("setCharAt", "(IC)V", NATIVE { sb(a[0]).at(size_t(asInt(a[1]))) = char16_t(asInt(a[2])); return 0; });
        b->addNative("deleteCharAt", "(I)" + desc, NATIVE { sb(a[0]).erase(size_t(asInt(a[1])), 1); return a[0]; });
        b->addNative("delete", "(II)" + desc, NATIVE {
            auto& s = sb(a[0]);
            size_t from = size_t(asInt(a[1])), to = std::min(s.size(), size_t(asInt(a[2])));
            if (from < to) s.erase(from, to - from);
            return a[0];
        });
        b->addNative("insert", "(ILjava/lang/String;)" + desc, NATIVE {
            sb(a[0]).insert(size_t(asInt(a[1])), vm.string16Of(asObj(a[2])));
            return a[0];
        });
        b->addNative("reverse", "()" + desc, NATIVE {
            auto& s = sb(a[0]);
            std::reverse(s.begin(), s.end());
            return a[0];
        });
    }

    // --- boxes ---------------------------------------------------------------
    Class* number = vm.defineNativeClass("Ljava/lang/Number;", "Ljava/lang/Object;", {"Ljava/io/Serializable;"}, kPublic | kAbstract);
    (void)number;
    defineBox(vm, "Ljava/lang/Integer;", 'I');
    defineBox(vm, "Ljava/lang/Long;", 'J');
    defineBox(vm, "Ljava/lang/Float;", 'F');
    defineBox(vm, "Ljava/lang/Double;", 'D');
    defineBox(vm, "Ljava/lang/Boolean;", 'Z');
    defineBox(vm, "Ljava/lang/Character;", 'C');
    defineBox(vm, "Ljava/lang/Short;", 'S');
    defineBox(vm, "Ljava/lang/Byte;", 'B');

    Class* integer = vm.findClass("Ljava/lang/Integer;");
    integer->addStaticNative("parseInt", "(Ljava/lang/String;)I", NATIVE { return fromInt(parseInt(vm, str(vm, a[0]), 10)); });
    integer->addStaticNative("parseInt", "(Ljava/lang/String;I)I", NATIVE { return fromInt(parseInt(vm, str(vm, a[0]), asInt(a[1]))); });
    integer->addStaticNative("valueOf", "(Ljava/lang/String;)Ljava/lang/Integer;", NATIVE {
        Reg v = fromInt(parseInt(vm, str(vm, a[0]), 10));
        return vm.callStatic(vm.findClass("Ljava/lang/Integer;"), "valueOf", "(I)Ljava/lang/Integer;", {v});
    });
    integer->addStaticNative("toHexString", "(I)Ljava/lang/String;", NATIVE {
        char buf[16];
        snprintf(buf, sizeof buf, "%x", uint32_t(asInt(a[0])));
        return newStr(vm, std::string(buf));
    });
    integer->addStaticNative("toBinaryString", "(I)Ljava/lang/String;", NATIVE {
        uint32_t v = uint32_t(asInt(a[0]));
        std::string s;
        do { s.insert(s.begin(), char('0' + (v & 1))); v >>= 1; } while (v);
        return newStr(vm, s);
    });
    integer->addStaticNative("max", "(II)I", NATIVE { return fromInt(std::max(asInt(a[0]), asInt(a[1]))); });
    integer->addStaticNative("min", "(II)I", NATIVE { return fromInt(std::min(asInt(a[0]), asInt(a[1]))); });
    integer->addStaticNative("compare", "(II)I", NATIVE { return fromInt(asInt(a[0]) < asInt(a[1]) ? -1 : asInt(a[0]) > asInt(a[1])); });
    vm.findClass("Ljava/lang/Long;")->addStaticNative("parseLong", "(Ljava/lang/String;)J", NATIVE {
        std::string s = utf16ToUtf8(str(vm, a[0]));
        char* end;
        long long v = std::strtoll(s.c_str(), &end, 10);
        if (s.empty() || *end) vm.throwNew("Ljava/lang/NumberFormatException;", "For input string: \"" + s + "\"");
        return fromLong(v);
    });
    vm.findClass("Ljava/lang/Float;")->addStaticNative("parseFloat", "(Ljava/lang/String;)F", NATIVE {
        std::string s = utf16ToUtf8(str(vm, a[0]));
        char* end;
        float v = std::strtof(s.c_str(), &end);
        if (s.empty() || *end) vm.throwNew("Ljava/lang/NumberFormatException;", "For input string: \"" + s + "\"");
        return fromFloat(v);
    });
    vm.findClass("Ljava/lang/Double;")->addStaticNative("parseDouble", "(Ljava/lang/String;)D", NATIVE {
        std::string s = utf16ToUtf8(str(vm, a[0]));
        char* end;
        double v = std::strtod(s.c_str(), &end);
        if (s.empty() || *end) vm.throwNew("Ljava/lang/NumberFormatException;", "For input string: \"" + s + "\"");
        return fromDouble(v);
    });
    vm.findClass("Ljava/lang/Float;")->addStaticNative("isNaN", "(F)Z", NATIVE { return std::isnan(asFloat(a[0])); });
    vm.findClass("Ljava/lang/Double;")->addStaticNative("isNaN", "(D)Z", NATIVE { return std::isnan(asDouble(a[0])); });
    vm.findClass("Ljava/lang/Boolean;")->addStaticNative("parseBoolean", "(Ljava/lang/String;)Z", NATIVE {
        StringObject* s = asString(a[0]);
        if (!s) return 0;
        std::u16string v = s->value;
        for (auto& c : v) c = toLower(c);
        return v == u"true";
    });
    Class* character = vm.findClass("Ljava/lang/Character;");
    character->addStaticNative("isDigit", "(C)Z", NATIVE { return asInt(a[0]) >= '0' && asInt(a[0]) <= '9'; });
    character->addStaticNative("isLetter", "(C)Z", NATIVE {
        char16_t c = char16_t(asInt(a[0]));
        return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= 0xc0 && c != 0xd7 && c != 0xf7);
    });
    character->addStaticNative("isLetterOrDigit", "(C)Z", NATIVE {
        char16_t c = char16_t(asInt(a[0]));
        return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c >= 0xc0;
    });
    character->addStaticNative("isWhitespace", "(C)Z", NATIVE { int c = asInt(a[0]); return c == ' ' || (c >= 9 && c <= 13); });
    character->addStaticNative("isUpperCase", "(C)Z", NATIVE { char16_t c = char16_t(asInt(a[0])); return toLower(c) != c; });
    character->addStaticNative("isLowerCase", "(C)Z", NATIVE { char16_t c = char16_t(asInt(a[0])); return toUpper(c) != c; });
    character->addStaticNative("toUpperCase", "(C)C", NATIVE { return toUpper(char16_t(asInt(a[0]))); });
    character->addStaticNative("toLowerCase", "(C)C", NATIVE { return toLower(char16_t(asInt(a[0]))); });
    character->addStaticNative("getNumericValue", "(C)I", NATIVE {
        int c = asInt(a[0]);
        return fromInt(c >= '0' && c <= '9' ? c - '0' : -1);
    });

    // --- Math ------------------------------------------------------------------
    Class* math = vm.defineNativeClass("Ljava/lang/Math;", "Ljava/lang/Object;", {}, kPublic | kFinal);
    math->addStaticNative("abs", "(I)I", NATIVE { int32_t v = asInt(a[0]); return fromInt(v < 0 ? int32_t(0u - uint32_t(v)) : v); });
    math->addStaticNative("abs", "(J)J", NATIVE { int64_t v = asLong(a[0]); return fromLong(v < 0 ? -v : v); });
    math->addStaticNative("abs", "(F)F", NATIVE { return fromFloat(std::fabs(asFloat(a[0]))); });
    math->addStaticNative("abs", "(D)D", NATIVE { return fromDouble(std::fabs(asDouble(a[0]))); });
    math->addStaticNative("max", "(II)I", NATIVE { return fromInt(std::max(asInt(a[0]), asInt(a[1]))); });
    math->addStaticNative("min", "(II)I", NATIVE { return fromInt(std::min(asInt(a[0]), asInt(a[1]))); });
    math->addStaticNative("max", "(JJ)J", NATIVE { return fromLong(std::max(asLong(a[0]), asLong(a[2]))); });
    math->addStaticNative("min", "(JJ)J", NATIVE { return fromLong(std::min(asLong(a[0]), asLong(a[2]))); });
    math->addStaticNative("max", "(FF)F", NATIVE { return fromFloat(std::max(asFloat(a[0]), asFloat(a[1]))); });
    math->addStaticNative("min", "(FF)F", NATIVE { return fromFloat(std::min(asFloat(a[0]), asFloat(a[1]))); });
    math->addStaticNative("max", "(DD)D", NATIVE { return fromDouble(std::max(asDouble(a[0]), asDouble(a[2]))); });
    math->addStaticNative("min", "(DD)D", NATIVE { return fromDouble(std::min(asDouble(a[0]), asDouble(a[2]))); });
    struct { const char* name; double (*fn)(double); } unary[] = {
        {"sqrt", std::sqrt}, {"sin", std::sin}, {"cos", std::cos}, {"tan", std::tan}, {"asin", std::asin},
        {"acos", std::acos}, {"atan", std::atan}, {"exp", std::exp}, {"log", std::log}, {"log10", std::log10},
        {"floor", std::floor}, {"ceil", std::ceil}, {"cbrt", std::cbrt}, {"rint", std::rint},
    };
    for (auto& u : unary) {
        auto fn = u.fn;
        math->addStaticNative(u.name, "(D)D", NATIVE_CAPTURE(fn) { return fromDouble(fn(asDouble(a[0]))); });
    }
    math->addStaticNative("pow", "(DD)D", NATIVE { return fromDouble(std::pow(asDouble(a[0]), asDouble(a[2]))); });
    math->addStaticNative("atan2", "(DD)D", NATIVE { return fromDouble(std::atan2(asDouble(a[0]), asDouble(a[2]))); });
    math->addStaticNative("hypot", "(DD)D", NATIVE { return fromDouble(std::hypot(asDouble(a[0]), asDouble(a[2]))); });
    math->addStaticNative("toRadians", "(D)D", NATIVE { return fromDouble(asDouble(a[0]) * M_PI / 180.0); });
    math->addStaticNative("toDegrees", "(D)D", NATIVE { return fromDouble(asDouble(a[0]) * 180.0 / M_PI); });
    math->addStaticNative("round", "(F)I", NATIVE { return fromInt(int32_t(std::floor(asFloat(a[0]) + 0.5f))); });
    math->addStaticNative("round", "(D)J", NATIVE { return fromLong(int64_t(std::floor(asDouble(a[0]) + 0.5))); });
    math->addStaticNative("signum", "(F)F", NATIVE { float v = asFloat(a[0]); return fromFloat(v > 0 ? 1.f : v < 0 ? -1.f : v); });
    math->addStaticNative("random", "()D", NATIVE { return fromDouble(double(std::rand()) / (double(RAND_MAX) + 1.0)); });

    // --- System, PrintStream ----------------------------------------------------
    Class* ps = vm.defineNativeClass("Ljava/io/PrintStream;", "Ljava/lang/Object;");
    ps->peerFactory = [] { return std::make_unique<PrintStreamPeer>(); };
    auto streamOf = [](Reg self) -> std::ostream& {
        return asObj(self)->peerAs<PrintStreamPeer>()->err ? std::cerr : std::cout;
    };
    for (std::string t : {"", "I", "J", "F", "D", "C", "Z", "Ljava/lang/String;", "Ljava/lang/Object;", "[C"}) {
        char kind = t.empty() ? 'V' : t[0];
        for (bool ln : {false, true}) {
            if (t.empty() && !ln) continue;
            ps->addNative(ln ? "println" : "print", "(" + t + ")V", NATIVE_CAPTURE(kind, ln, streamOf) {
                std::ostream& os = streamOf(a[0]);
                if (kind == '[') {
                    for (Reg c : static_cast<ArrayObject*>(asObj(a[1]))->data) os << utf16ToUtf8(std::u16string(1, char16_t(c)));
                } else if (kind != 'V') {
                    os << utf16ToUtf8(appendText(vm, kind, a + 1));
                }
                if (ln) os << "\n";
                os.flush();
                return 0;
            });
        }
    }
    ps->addNative("printf", "(Ljava/lang/String;[Ljava/lang/Object;)Ljava/io/PrintStream;", NATIVE_CAPTURE(streamOf) {
        streamOf(a[0]) << utf16ToUtf8(javaFormat(vm, str(vm, a[1]), asObj(a[2]))) << std::flush;
        return a[0];
    });
    ps->addNative("flush", "()V", NATIVE { return 0; });

    Class* system = vm.defineNativeClass("Ljava/lang/System;", "Ljava/lang/Object;", {}, kPublic | kFinal);
    system->addStaticField("out", "Ljava/io/PrintStream;");
    system->addStaticField("err", "Ljava/io/PrintStream;");
    system->addStaticNative("currentTimeMillis", "()J", NATIVE {
        using namespace std::chrono;
        return fromLong(duration_cast<milliseconds>(system_clock::now().time_since_epoch()).count());
    });
    system->addStaticNative("nanoTime", "()J", NATIVE {
        using namespace std::chrono;
        return fromLong(duration_cast<nanoseconds>(steady_clock::now().time_since_epoch()).count());
    });
    system->addStaticNative("arraycopy", "(Ljava/lang/Object;ILjava/lang/Object;II)V", NATIVE {
        auto* src = static_cast<ArrayObject*>(asObj(a[0]));
        auto* dst = static_cast<ArrayObject*>(asObj(a[2]));
        if (!src || !dst) vm.throwNPE("arraycopy");
        int32_t sp = asInt(a[1]), dp = asInt(a[3]), n = asInt(a[4]);
        if (sp < 0 || dp < 0 || n < 0 || size_t(sp + n) > src->data.size() || size_t(dp + n) > dst->data.size())
            vm.throwNew("Ljava/lang/ArrayIndexOutOfBoundsException;", "arraycopy");
        std::memmove(dst->data.data() + dp, src->data.data() + sp, size_t(n) * sizeof(Reg));
        return 0;
    });
    system->addStaticNative("exit", "(I)V", NATIVE { throw ExitRequest{asInt(a[0])}; });
    system->addStaticNative("identityHashCode", "(Ljava/lang/Object;)I", NATIVE { return fromInt(identityHash(asObj(a[0]))); });
    system->addStaticNative("lineSeparator", "()Ljava/lang/String;", NATIVE { return newStr(vm, std::string("\n")); });
    system->addStaticNative("getProperty", "(Ljava/lang/String;)Ljava/lang/String;", NATIVE { return 0; });
    system->addStaticNative("gc", "()V", NATIVE { return 0; });

    Class* thread = vm.defineNativeClass("Ljava/lang/Thread;", "Ljava/lang/Object;", {"Ljava/lang/Runnable;"});
    thread->addStaticNative("sleep", "(J)V", NATIVE {
        // Single-threaded emulator: sleeping blocks the UI just like on a real main thread.
        std::this_thread::sleep_for(std::chrono::milliseconds(asLong(a[0])));
        return 0;
    });

    // --- Enum ------------------------------------------------------------------
    Class* en = vm.defineNativeClass("Ljava/lang/Enum;", "Ljava/lang/Object;",
                                     {"Ljava/lang/Comparable;", "Ljava/io/Serializable;"}, kPublic | kAbstract);
    Field* enumName = en->addField("name", "Ljava/lang/String;");
    Field* enumOrdinal = en->addField("ordinal", "I");
    en->addNative("<init>", "(Ljava/lang/String;I)V", NATIVE_CAPTURE(enumName, enumOrdinal) {
        Object* o = asObj(a[0]);
        o->fields[enumName->slot] = a[1];
        o->fields[enumOrdinal->slot] = a[2];
        return 0;
    });
    en->addNative("name", "()Ljava/lang/String;", NATIVE_CAPTURE(enumName) { return asObj(a[0])->fields[enumName->slot]; });
    en->addNative("toString", "()Ljava/lang/String;", NATIVE_CAPTURE(enumName) { return asObj(a[0])->fields[enumName->slot]; });
    en->addNative("ordinal", "()I", NATIVE_CAPTURE(enumOrdinal) { return asObj(a[0])->fields[enumOrdinal->slot]; });
    en->addNative("compareTo", "(Ljava/lang/Enum;)I", NATIVE_CAPTURE(enumOrdinal) {
        return fromInt(asInt(asObj(a[0])->fields[enumOrdinal->slot]) - asInt(asObj(a[1])->fields[enumOrdinal->slot]));
    });
    en->addStaticNative("valueOf", "(Ljava/lang/Class;Ljava/lang/String;)Ljava/lang/Enum;", NATIVE_CAPTURE(enumName) {
        Class* c = VM::classFromMirror(asObj(a[0]));
        vm.ensureInitialized(c);
        for (auto& f : c->fields) {
            if (!f->isStatic || f->type != c->descriptor) continue;
            Object* v = asObj(f->staticValue);
            if (v && asString(v->fields[enumName->slot])->value == str(vm, a[1])) return fromObj(v);
        }
        vm.throwNew("Ljava/lang/IllegalArgumentException;", "No enum constant " + c->javaName() + "." + VM::utf8(asObj(a[1])));
    });

    // --- Throwable and the standard exceptions -------------------------------
    Class* throwable = vm.defineNativeClass("Ljava/lang/Throwable;", "Ljava/lang/Object;", {"Ljava/io/Serializable;"});
    throwable->peerFactory = [] { return std::make_unique<ThrowablePeer>(); };
    Field* message = throwable->addField("detailMessage", "Ljava/lang/String;");
    Field* cause = throwable->addField("cause", "Ljava/lang/Throwable;");
    auto record = [](VM& vm, Object* t) { t->peerAs<ThrowablePeer>()->trace = vm.stackTrace(); };
    throwable->addNative("<init>", "()V", NATIVE_CAPTURE(record) { record(vm, asObj(a[0])); return 0; });
    throwable->addNative("<init>", "(Ljava/lang/String;)V", NATIVE_CAPTURE(record, message) {
        record(vm, asObj(a[0]));
        asObj(a[0])->fields[message->slot] = a[1];
        return 0;
    });
    throwable->addNative("<init>", "(Ljava/lang/String;Ljava/lang/Throwable;)V", NATIVE_CAPTURE(record, message, cause) {
        record(vm, asObj(a[0]));
        asObj(a[0])->fields[message->slot] = a[1];
        asObj(a[0])->fields[cause->slot] = a[2];
        return 0;
    });
    throwable->addNative("<init>", "(Ljava/lang/Throwable;)V", NATIVE_CAPTURE(record, cause) {
        record(vm, asObj(a[0]));
        asObj(a[0])->fields[cause->slot] = a[1];
        return 0;
    });
    throwable->addNative("getMessage", "()Ljava/lang/String;", NATIVE_CAPTURE(message) { return asObj(a[0])->fields[message->slot]; });
    throwable->addNative("getLocalizedMessage", "()Ljava/lang/String;", NATIVE_CAPTURE(message) { return asObj(a[0])->fields[message->slot]; });
    throwable->addNative("getCause", "()Ljava/lang/Throwable;", NATIVE_CAPTURE(cause) { return asObj(a[0])->fields[cause->slot]; });
    throwable->addNative("toString", "()Ljava/lang/String;", NATIVE { return newStr(vm, vm.describeException(asObj(a[0]))); });
    throwable->addNative("printStackTrace", "()V", NATIVE {
        Object* t = asObj(a[0]);
        std::cerr << vm.describeException(t) << "\n";
        for (auto& line : t->peerAs<ThrowablePeer>()->trace) std::cerr << "\tat " << line << "\n";
        return 0;
    });

    defineThrowable(vm, "Ljava/lang/Exception;", "Ljava/lang/Throwable;");
    defineThrowable(vm, "Ljava/lang/Error;", "Ljava/lang/Throwable;");
    defineThrowable(vm, "Ljava/lang/RuntimeException;", "Ljava/lang/Exception;");
    for (const char* e : {"Ljava/lang/NullPointerException;", "Ljava/lang/ArithmeticException;",
                          "Ljava/lang/ClassCastException;", "Ljava/lang/IllegalArgumentException;",
                          "Ljava/lang/IllegalStateException;", "Ljava/lang/UnsupportedOperationException;",
                          "Ljava/lang/NegativeArraySizeException;", "Ljava/lang/IndexOutOfBoundsException;",
                          "Ljava/lang/ArrayStoreException;", "Ljava/lang/SecurityException;",
                          "Ljava/util/NoSuchElementException;", "Ljava/util/ConcurrentModificationException;"})
        defineThrowable(vm, e, "Ljava/lang/RuntimeException;");
    defineThrowable(vm, "Ljava/lang/ArrayIndexOutOfBoundsException;", "Ljava/lang/IndexOutOfBoundsException;");
    defineThrowable(vm, "Ljava/lang/StringIndexOutOfBoundsException;", "Ljava/lang/IndexOutOfBoundsException;");
    defineThrowable(vm, "Ljava/lang/NumberFormatException;", "Ljava/lang/IllegalArgumentException;");
    for (const char* e : {"Ljava/lang/InterruptedException;", "Ljava/io/IOException;",
                          "Ljava/lang/CloneNotSupportedException;", "Ljava/lang/ReflectiveOperationException;"})
        defineThrowable(vm, e, "Ljava/lang/Exception;");
    defineThrowable(vm, "Ljava/lang/ClassNotFoundException;", "Ljava/lang/ReflectiveOperationException;");
    defineThrowable(vm, "Ljava/lang/VirtualMachineError;", "Ljava/lang/Error;");
    defineThrowable(vm, "Ljava/lang/StackOverflowError;", "Ljava/lang/VirtualMachineError;");
    defineThrowable(vm, "Ljava/lang/OutOfMemoryError;", "Ljava/lang/VirtualMachineError;");
    defineThrowable(vm, "Ljava/lang/LinkageError;", "Ljava/lang/Error;");
    for (const char* e : {"Ljava/lang/VerifyError;", "Ljava/lang/IncompatibleClassChangeError;", "Ljava/lang/NoClassDefFoundError;"})
        defineThrowable(vm, e, "Ljava/lang/LinkageError;");
    defineThrowable(vm, "Ljava/lang/AbstractMethodError;", "Ljava/lang/IncompatibleClassChangeError;");
    defineThrowable(vm, "Ljava/lang/NoSuchMethodError;", "Ljava/lang/IncompatibleClassChangeError;");
    defineThrowable(vm, "Ljava/lang/AssertionError;", "Ljava/lang/Error;");

    // --- java.util.ArrayList ----------------------------------------------------
    Class* list = vm.defineNativeClass("Ljava/util/ArrayList;", "Ljava/lang/Object;",
                                       {"Ljava/util/List;", "Ljava/lang/Iterable;", "Ljava/io/Serializable;"});
    list->peerFactory = [] { return std::make_unique<ListPeer>(); };
    auto checkIndex = [](VM& vm, Reg self, int32_t i, size_t extra = 0) {
        if (i < 0 || size_t(i) >= items(self).size() + extra)
            vm.throwNew("Ljava/lang/IndexOutOfBoundsException;",
                        "Index: " + std::to_string(i) + ", Size: " + std::to_string(items(self).size()));
    };
    list->addNative("<init>", "()V", NATIVE { return 0; });
    list->addNative("<init>", "(I)V", NATIVE { return 0; });
    list->addNative("<init>", "(Ljava/util/Collection;)V", NATIVE {
        Object* src = asObj(a[1]);
        if (src && src->peer && dynamic_cast<ListPeer*>(src->peer.get())) items(a[0]) = items(a[1]);
        else vm.warnOnce("ArrayList(Collection) supports only lists");
        return 0;
    });
    list->addNative("add", "(Ljava/lang/Object;)Z", NATIVE { items(a[0]).push_back(asObj(a[1])); return 1; });
    list->addNative("add", "(ILjava/lang/Object;)V", NATIVE_CAPTURE(checkIndex) {
        checkIndex(vm, a[0], asInt(a[1]), 1);
        items(a[0]).insert(items(a[0]).begin() + asInt(a[1]), asObj(a[2]));
        return 0;
    });
    list->addNative("addAll", "(Ljava/util/Collection;)Z", NATIVE {
        Object* src = asObj(a[1]);
        if (!src || !dynamic_cast<ListPeer*>(src->peer.get())) { vm.warnOnce("addAll supports only lists"); return 0; }
        auto copy = items(a[1]);
        items(a[0]).insert(items(a[0]).end(), copy.begin(), copy.end());
        return !copy.empty();
    });
    list->addNative("get", "(I)Ljava/lang/Object;", NATIVE_CAPTURE(checkIndex) {
        checkIndex(vm, a[0], asInt(a[1]));
        return fromObj(items(a[0])[size_t(asInt(a[1]))]);
    });
    list->addNative("set", "(ILjava/lang/Object;)Ljava/lang/Object;", NATIVE_CAPTURE(checkIndex) {
        checkIndex(vm, a[0], asInt(a[1]));
        Object*& slot = items(a[0])[size_t(asInt(a[1]))];
        Object* old = slot;
        slot = asObj(a[2]);
        return fromObj(old);
    });
    list->addNative("remove", "(I)Ljava/lang/Object;", NATIVE_CAPTURE(checkIndex) {
        checkIndex(vm, a[0], asInt(a[1]));
        auto& v = items(a[0]);
        Object* old = v[size_t(asInt(a[1]))];
        v.erase(v.begin() + asInt(a[1]));
        return fromObj(old);
    });
    list->addNative("remove", "(Ljava/lang/Object;)Z", NATIVE {
        auto& v = items(a[0]);
        for (size_t i = 0; i < v.size(); ++i)
            if (javaEquals(vm, asObj(a[1]), v[i])) { v.erase(v.begin() + long(i)); return 1; }
        return 0;
    });
    list->addNative("indexOf", "(Ljava/lang/Object;)I", NATIVE {
        auto& v = items(a[0]);
        for (size_t i = 0; i < v.size(); ++i)
            if (javaEquals(vm, asObj(a[1]), v[i])) return fromInt(int32_t(i));
        return fromInt(-1);
    });
    list->addNative("contains", "(Ljava/lang/Object;)Z", NATIVE {
        for (Object* o : items(a[0])) if (javaEquals(vm, asObj(a[1]), o)) return 1;
        return 0;
    });
    list->addNative("size", "()I", NATIVE { return fromInt(int32_t(items(a[0]).size())); });
    list->addNative("isEmpty", "()Z", NATIVE { return items(a[0]).empty(); });
    list->addNative("clear", "()V", NATIVE { items(a[0]).clear(); return 0; });
    list->addNative("toString", "()Ljava/lang/String;", NATIVE {
        std::u16string s = u"[";
        auto copy = items(a[0]);
        for (size_t i = 0; i < copy.size(); ++i) s += (i ? u", " : u"") + vm.string16Of(copy[i]);
        return newStr(vm, s + u"]");
    });
    Class* itr = vm.defineNativeClass("Ljava/util/ArrayList$Itr;", "Ljava/lang/Object;", {"Ljava/util/Iterator;"});
    itr->peerFactory = [] { return std::make_unique<IteratorPeer>(); };
    list->addNative("iterator", "()Ljava/util/Iterator;", NATIVE_CAPTURE(itr) {
        Object* it = vm.alloc(itr);
        it->peerAs<IteratorPeer>()->list = asObj(a[0]);
        return fromObj(it);
    });
    itr->addNative("hasNext", "()Z", NATIVE {
        auto* p = asObj(a[0])->peerAs<IteratorPeer>();
        return p->index < p->list->peerAs<ListPeer>()->items.size();
    });
    itr->addNative("next", "()Ljava/lang/Object;", NATIVE {
        auto* p = asObj(a[0])->peerAs<IteratorPeer>();
        auto& v = p->list->peerAs<ListPeer>()->items;
        if (p->index >= v.size()) vm.throwNew("Ljava/util/NoSuchElementException;");
        return fromObj(v[p->index++]);
    });
    itr->addNative("remove", "()V", NATIVE {
        auto* p = asObj(a[0])->peerAs<IteratorPeer>();
        auto& v = p->list->peerAs<ListPeer>()->items;
        if (p->index == 0) vm.throwNew("Ljava/lang/IllegalStateException;");
        v.erase(v.begin() + long(--p->index));
        return 0;
    });

    // --- java.util.HashMap (linear, insertion ordered; fine for app-sized maps) --
    Class* map = vm.defineNativeClass("Ljava/util/HashMap;", "Ljava/lang/Object;", {"Ljava/util/Map;"});
    map->peerFactory = [] { return std::make_unique<MapPeer>(); };
    vm.defineNativeClass("Ljava/util/LinkedHashMap;", "Ljava/util/HashMap;");
    auto entries = [](Reg self) -> auto& { return asObj(self)->peerAs<MapPeer>()->entries; };
    auto findKey = [entries](VM& vm, Reg self, Object* key) -> long {
        auto& e = entries(self);
        for (size_t i = 0; i < e.size(); ++i)
            if (javaEquals(vm, key, e[i].first)) return long(i);
        return -1;
    };
    map->addNative("<init>", "()V", NATIVE { return 0; });
    map->addNative("<init>", "(I)V", NATIVE { return 0; });
    map->addNative("put", "(Ljava/lang/Object;Ljava/lang/Object;)Ljava/lang/Object;", NATIVE_CAPTURE(entries, findKey) {
        long i = findKey(vm, a[0], asObj(a[1]));
        if (i < 0) { entries(a[0]).push_back({asObj(a[1]), asObj(a[2])}); return 0; }
        Object* old = entries(a[0])[size_t(i)].second;
        entries(a[0])[size_t(i)].second = asObj(a[2]);
        return fromObj(old);
    });
    map->addNative("get", "(Ljava/lang/Object;)Ljava/lang/Object;", NATIVE_CAPTURE(entries, findKey) {
        long i = findKey(vm, a[0], asObj(a[1]));
        return i < 0 ? 0 : fromObj(entries(a[0])[size_t(i)].second);
    });
    map->addNative("getOrDefault", "(Ljava/lang/Object;Ljava/lang/Object;)Ljava/lang/Object;", NATIVE_CAPTURE(entries, findKey) {
        long i = findKey(vm, a[0], asObj(a[1]));
        return i < 0 ? a[2] : fromObj(entries(a[0])[size_t(i)].second);
    });
    map->addNative("containsKey", "(Ljava/lang/Object;)Z", NATIVE_CAPTURE(findKey) { return findKey(vm, a[0], asObj(a[1])) >= 0; });
    map->addNative("remove", "(Ljava/lang/Object;)Ljava/lang/Object;", NATIVE_CAPTURE(entries, findKey) {
        long i = findKey(vm, a[0], asObj(a[1]));
        if (i < 0) return 0;
        Object* old = entries(a[0])[size_t(i)].second;
        entries(a[0]).erase(entries(a[0]).begin() + i);
        return fromObj(old);
    });
    map->addNative("size", "()I", NATIVE_CAPTURE(entries) { return fromInt(int32_t(entries(a[0]).size())); });
    map->addNative("isEmpty", "()Z", NATIVE_CAPTURE(entries) { return entries(a[0]).empty(); });
    map->addNative("clear", "()V", NATIVE_CAPTURE(entries) { entries(a[0]).clear(); return 0; });
    // keySet()/values() return list snapshots: enough for iteration and size().
    map->addNative("keySet", "()Ljava/util/Set;", NATIVE_CAPTURE(entries, list) {
        Object* l = vm.alloc(list);
        for (auto& [k, v] : entries(a[0])) l->peerAs<ListPeer>()->items.push_back(k);
        return fromObj(l);
    });
    map->addNative("values", "()Ljava/util/Collection;", NATIVE_CAPTURE(entries, list) {
        Object* l = vm.alloc(list);
        for (auto& [k, v] : entries(a[0])) l->peerAs<ListPeer>()->items.push_back(v);
        return fromObj(l);
    });

    // --- java.util.Random -------------------------------------------------------
    Class* random = vm.defineNativeClass("Ljava/util/Random;", "Ljava/lang/Object;", {"Ljava/io/Serializable;"});
    random->peerFactory = [] { return std::make_unique<RandomPeer>(); };
    auto rnd = [](Reg self) { return asObj(self)->peerAs<RandomPeer>(); };
    random->addNative("<init>", "()V", NATIVE_CAPTURE(rnd) {
        rnd(a[0])->setSeed(std::chrono::steady_clock::now().time_since_epoch().count() ^ 0x2545F4914F6CDD1DLL);
        return 0;
    });
    random->addNative("<init>", "(J)V", NATIVE_CAPTURE(rnd) { rnd(a[0])->setSeed(asLong(a[1])); return 0; });
    random->addNative("setSeed", "(J)V", NATIVE_CAPTURE(rnd) { rnd(a[0])->setSeed(asLong(a[1])); return 0; });
    random->addNative("nextInt", "()I", NATIVE_CAPTURE(rnd) { return fromInt(rnd(a[0])->next(32)); });
    random->addNative("nextInt", "(I)I", NATIVE_CAPTURE(rnd) {
        int32_t bound = asInt(a[1]);
        if (bound <= 0) vm.throwNew("Ljava/lang/IllegalArgumentException;", "bound must be positive");
        RandomPeer* r = rnd(a[0]);
        if ((bound & -bound) == bound) return fromInt(int32_t((int64_t(bound) * int64_t(r->next(31))) >> 31));
        int32_t bits, val;
        do {
            bits = r->next(31);
            val = bits % bound;
        } while (bits - val + (bound - 1) < 0);
        return fromInt(val);
    });
    random->addNative("nextLong", "()J", NATIVE_CAPTURE(rnd) {
        RandomPeer* r = rnd(a[0]);
        int64_t hi = r->next(32);
        return fromLong(int64_t(uint64_t(hi) << 32) + r->next(32));
    });
    random->addNative("nextBoolean", "()Z", NATIVE_CAPTURE(rnd) { return rnd(a[0])->next(1) != 0; });
    random->addNative("nextFloat", "()F", NATIVE_CAPTURE(rnd) { return fromFloat(float(rnd(a[0])->next(24)) / float(1 << 24)); });
    random->addNative("nextDouble", "()D", NATIVE_CAPTURE(rnd) {
        RandomPeer* r = rnd(a[0]);
        int64_t hi = int64_t(r->next(26)) << 27;
        return fromDouble(double(hi + r->next(27)) * 0x1.0p-53);
    });

    // --- java.lang.reflect.Array: dx uses it for multi-dimensional arrays ------
    Class* reflectArray = vm.defineNativeClass("Ljava/lang/reflect/Array;", "Ljava/lang/Object;", {}, kPublic | kFinal);
    reflectArray->addStaticNative("newInstance", "(Ljava/lang/Class;I)Ljava/lang/Object;", NATIVE {
        Class* component = VM::classFromMirror(asObj(a[0]));
        return fromObj(vm.newArray("[" + component->descriptor, asInt(a[1])));
    });
    reflectArray->addStaticNative("newInstance", "(Ljava/lang/Class;[I)Ljava/lang/Object;", NATIVE {
        Class* component = VM::classFromMirror(asObj(a[0]));
        auto* dims = static_cast<ArrayObject*>(asObj(a[1]));
        std::function<Object*(size_t)> build = [&](size_t level) -> Object* {
            std::string desc = std::string(dims->data.size() - level, '[') + component->descriptor;
            ArrayObject* arr = vm.newArray(desc, asInt(dims->data[level]));
            if (level + 1 < dims->data.size())
                for (auto& slot : arr->data) slot = fromObj(build(level + 1));
            return arr;
        };
        return fromObj(build(0));
    });

    (void)klass;
}

void initJavaLangStatics(VM& vm) {
    Class* system = vm.findClass("Ljava/lang/System;");
    Class* ps = vm.findClass("Ljava/io/PrintStream;");
    Object* out = vm.alloc(ps);
    Object* err = vm.alloc(ps);
    err->peerAs<PrintStreamPeer>()->err = true;
    system->findField("out")->staticValue = fromObj(out);
    system->findField("err")->staticValue = fromObj(err);

    // Integer.TYPE and friends are the Class objects of the primitive types.
    for (auto [box, prim] : {std::pair{"Ljava/lang/Integer;", "I"}, {"Ljava/lang/Long;", "J"}, {"Ljava/lang/Float;", "F"},
                             {"Ljava/lang/Double;", "D"}, {"Ljava/lang/Boolean;", "Z"}, {"Ljava/lang/Character;", "C"},
                             {"Ljava/lang/Short;", "S"}, {"Ljava/lang/Byte;", "B"}})
        vm.findClass(box)->addStaticField("TYPE", "Ljava/lang/Class;", fromObj(vm.classMirror(vm.findClass(prim))));
}

}  // namespace droid
