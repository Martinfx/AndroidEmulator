// Shared helpers for writing native (C++) implementations of Java classes.
//
// A native method receives the VM and its arguments laid out exactly like
// Dalvik registers: args[0] is `this` for instance methods, and a long or
// double argument takes two slots (the value sits in the first one).
#pragma once

#include <string>

#include "vm/vm.h"

namespace droid {

#define NATIVE [](VM & vm, Reg * a) -> Reg
#define NATIVE_CAPTURE(...) [__VA_ARGS__](VM & vm, Reg * a) -> Reg

inline StringObject* asString(Reg r) {
    Object* o = asObj(r);
    return o && o->kind == Object::String ? static_cast<StringObject*>(o) : nullptr;
}

// The UTF-16 contents of a String argument; throws NullPointerException for null.
inline std::u16string& str(VM& vm, Reg r) {
    StringObject* s = asString(r);
    if (!s) vm.throwNPE("String argument is null");
    return s->value;
}

inline Reg newStr(VM& vm, const std::u16string& s) { return fromObj(vm.newString(s)); }
inline Reg newStr(VM& vm, const std::string& s) { return fromObj(vm.newString(s)); }

// Java's Float.toString / Double.toString: the shortest decimal that
// round-trips, written as "1.0", "0.25", "1.0E10".
std::string javaDoubleToString(double v, bool isFloat);

// A small String.format supporting %s %d %x %f %.Nf %c %b %n %%.
std::u16string javaFormat(VM& vm, const std::u16string& fmt, Object* argsArray);

// Upper/lower case mapping for ASCII, Latin-1 and Latin Extended-A (Czech letters).
char16_t javaToUpper(char16_t c);
char16_t javaToLower(char16_t c);

// Reads the "value" field of a boxed primitive (Integer, Float, ...).
Reg unboxValue(Object* box);

void registerJavaLang(VM& vm);
void initJavaLangStatics(VM& vm);
void registerAndroid(VM& vm);
void initAndroidStatics(VM& vm);

}  // namespace droid
