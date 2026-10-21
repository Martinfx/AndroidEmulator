// Core of the Dalvik virtual machine: object model, class loading, heap, invocation.
#pragma once

#include <cstdint>
#include <cstring>
#include <functional>
#include <map>
#include <memory>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "apk/dex.h"

namespace droid {

// A Dalvik register. Narrow values live in the low 32 bits, wide values
// (long/double) occupy the whole 64 bits of the first register of a pair,
// object references are host pointers.
using Reg = uint64_t;

struct Class;
struct Method;
struct Object;
class VM;

inline int32_t asInt(Reg r) { return int32_t(uint32_t(r)); }
inline int64_t asLong(Reg r) { return int64_t(r); }
inline float asFloat(Reg r) { uint32_t u = uint32_t(r); float f; std::memcpy(&f, &u, 4); return f; }
inline double asDouble(Reg r) { double d; std::memcpy(&d, &r, 8); return d; }
inline Object* asObj(Reg r) { return reinterpret_cast<Object*>(uintptr_t(r)); }
inline Reg fromInt(int32_t v) { return uint32_t(v); }
inline Reg fromLong(int64_t v) { return uint64_t(v); }
inline Reg fromFloat(float f) { uint32_t u; std::memcpy(&u, &f, 4); return u; }
inline Reg fromDouble(double d) { Reg r; std::memcpy(&r, &d, 8); return r; }
inline Reg fromObj(const Object* o) { return Reg(reinterpret_cast<uintptr_t>(o)); }

// Native state attached to objects of framework classes (views, paints, ...).
struct NativePeer {
    virtual ~NativePeer() = default;
    // Reports Java objects referenced by this peer so the GC keeps them alive.
    virtual void trace(std::vector<Object*>& out) const { (void)out; }
};

struct Object;

// The native state of a java.lang.Class object: which class it describes.
struct ClassMirrorPeer : NativePeer {
    Class* klass;
    explicit ClassMirrorPeer(Class* c) : klass(c) {}
};

struct Object {
    enum Kind : uint8_t { Plain, String, Array };

    Class* klass = nullptr;
    Kind kind = Plain;
    bool marked = false;
    std::vector<Reg> fields;
    std::unique_ptr<NativePeer> peer;

    virtual ~Object() = default;

    template <typename T> T* peerAs() const { return static_cast<T*>(peer.get()); }
};

struct StringObject : Object {
    std::u16string value;
};

struct ArrayObject : Object {
    std::vector<Reg> data;
};

struct Field {
    Class* owner;
    std::string name;
    std::string type;  // descriptor
    bool isStatic;
    uint32_t slot = 0;  // instance fields: index into Object::fields
    Reg staticValue = 0;

    bool isRef() const { return type[0] == 'L' || type[0] == '['; }
};

using NativeFn = std::function<Reg(VM&, Reg* args)>;

enum Access : uint32_t {
    kPublic = 0x1, kPrivate = 0x2, kStatic = 0x8, kFinal = 0x10, kInterface = 0x200,
    kAbstract = 0x400, kNative = 0x100, kConstructor = 0x10000,
};

struct Method {
    Class* owner;
    std::string name;
    std::string signature;  // "(ILjava/lang/String;)V"
    std::string shorty;     // "VIL"
    uint32_t access = 0;
    const DexFile* dex = nullptr;
    DexCode code;
    bool hasCode = false;
    NativeFn native;
    uint32_t argWords = 0;  // registers taken by arguments, including 'this'

    bool isStatic() const { return access & kStatic; }
    bool isAbstract() const { return !hasCode && !native; }
    std::string prettyName() const;
};

struct Class {
    std::string descriptor;  // "Lcom/example/Foo;"
    Class* super = nullptr;
    std::vector<Class*> interfaces;
    uint32_t access = 0;
    const DexFile* dex = nullptr;
    const DexClassDef* def = nullptr;
    Class* component = nullptr;  // arrays only

    std::vector<std::unique_ptr<Field>> fields;
    std::vector<std::unique_ptr<Method>> methods;
    uint32_t instanceSlots = 0;    // including superclasses
    std::vector<uint32_t> refSlots;  // instance slots holding references

    enum State { Linked, Initializing, Initialized } state = Linked;
    bool isStub = false;  // unknown framework class we made up
    std::function<std::unique_ptr<NativePeer>()> peerFactory;
    Object* mirror = nullptr;  // java.lang.Class instance

    std::unordered_map<std::string, Method*> vcache;
    std::unordered_map<const Method*, Method*> dcache;

    bool isInterface() const { return access & kInterface; }
    bool isArray() const { return descriptor[0] == '['; }
    std::string javaName() const;  // "com.example.Foo"

    Method* declaredMethod(const std::string& name, const std::string& sig) const;
    Method* findMethod(const std::string& name, const std::string& sig);  // walks supers
    Method* findVirtual(const std::string& name, const std::string& sig);  // runtime dispatch
    // Virtual dispatch of a resolved method reference, cached by pointer.
    Method* dispatch(const Method* ref);
    Field* findField(const std::string& name);
    bool isSubclassOf(const Class* other) const;
    bool implements(const Class* iface) const;
    bool isAssignableTo(const Class* target) const;

    // Native class building helpers.
    Method* addNative(const std::string& name, const std::string& sig, NativeFn fn, bool isStatic = false);
    Method* addStaticNative(const std::string& name, const std::string& sig, NativeFn fn) {
        return addNative(name, sig, std::move(fn), true);
    }
    Field* addField(const std::string& name, const std::string& type);
    Field* addStaticField(const std::string& name, const std::string& type, Reg value = 0);
};

// A Java exception travelling through C++ frames.
struct JavaThrow {
    Object* exception;
};

// Signals that the app asked to stop (Activity.finish on the last activity, System.exit).
struct ExitRequest {
    int code;
};

struct Frame {
    Method* method;
    uint32_t pc;
    Reg* regs;
    uint16_t nregs;
};

class VM {
public:
    VM();
    ~VM();

    void loadDex(std::vector<uint8_t> bytes);

    // Class lookup by descriptor; unknown classes become stubs (and are logged).
    Class* findClass(const std::string& descriptor);
    Class* findClassOrNull(const std::string& descriptor);
    Class* defineNativeClass(const std::string& descriptor, const std::string& superDescriptor,
                             std::vector<std::string> interfaces = {}, uint32_t access = kPublic);
    void ensureInitialized(Class* c);

    // Allocation.
    Object* alloc(Class* c);
    StringObject* newString(const std::u16string& s);
    StringObject* newString(const std::string& utf8) { return newString(utf8ToUtf16(utf8)); }
    ArrayObject* newArray(Class* arrayClass, int32_t length);
    ArrayObject* newArray(const std::string& descriptor, int32_t length) { return newArray(findClass(descriptor), length); }
    Object* classMirror(Class* c);
    static Class* classFromMirror(Object* mirror) {
        return mirror ? static_cast<ClassMirrorPeer*>(mirror->peer.get())->klass : nullptr;
    }

    // Invocation.
    Reg invoke(Method* m, Reg* args);
    Reg callVirtual(Object* receiver, const std::string& name, const std::string& sig,
                    std::vector<Reg> extraArgs = {});
    Reg callStatic(Class* c, const std::string& name, const std::string& sig, std::vector<Reg> args = {});
    // Creates an object and runs a constructor on it.
    Object* construct(Class* c, const std::string& sig = "()V", std::vector<Reg> args = {});

    // Exceptions.
    [[noreturn]] void throwNew(const std::string& descriptor, const std::string& message = "");
    [[noreturn]] void throwNPE(const std::string& what) { throwNew("Ljava/lang/NullPointerException;", what); }
    std::string describeException(Object* ex);

    // Helpers for natives.
    std::string stringOf(Object* o);       // String -> UTF-8, other objects via toString()
    std::u16string string16Of(Object* o);
    static std::string utf8(Object* str);  // must be a String or null

    // Garbage collection: only safe when no Java frames are active, or with
    // the frame registers scanned conservatively (which we do).
    void collectGarbage();
    size_t heapSize() const { return heap_.size(); }
    size_t allocatedSinceGc() const { return allocSinceGc_; }
    void addRoot(Object* o) { if (o) roots_[o]++; }
    void removeRoot(Object* o);

    // Interpreter state (used by interp.cpp and stack traces).
    std::vector<Frame*> frames;
    bool trace = false;
    std::vector<std::string> stackTrace() const;

    // Commonly used classes.
    Class* objectClass = nullptr;
    Class* stringClass = nullptr;
    Class* classClass = nullptr;

    // Interned dex strings and resolution caches.
    struct DexCache {
        std::unique_ptr<DexFile> dex;
        std::vector<StringObject*> strings;
        std::vector<Class*> types;
        std::vector<Method*> methods;
        std::vector<Field*> fields;
        std::vector<std::string> methodSigs;  // name + signature, for virtual dispatch
    };
    DexCache& cacheFor(const DexFile* dex);
    StringObject* resolveString(const DexFile* dex, uint32_t idx);
    Class* resolveType(const DexFile* dex, uint32_t idx);
    Method* resolveMethod(const DexFile* dex, uint32_t idx);
    Field* resolveField(const DexFile* dex, uint32_t idx);
    const std::string& methodKey(const DexFile* dex, uint32_t idx);

    void warnOnce(const std::string& msg);

private:
    Class* defineDexClass(DexCache& cache, const DexClassDef& def);
    Class* makeArrayClass(const std::string& descriptor);
    Class* makeStubClass(const std::string& descriptor);
    void link(Class* c);
    Reg interpret(Method* m, Reg* args);

    std::unordered_map<std::string, std::unique_ptr<Class>> classes_;
    std::unordered_map<std::string, std::pair<DexCache*, const DexClassDef*>> dexClasses_;
    std::vector<std::unique_ptr<DexCache>> dexes_;
    std::vector<Class*> nativeOrder_;
    std::vector<Object*> heap_;
    std::unordered_set<Object*> heapSet_;
    std::unordered_map<std::u16string, StringObject*> interned_;
    std::unordered_map<Object*, int> roots_;
    std::unordered_set<std::string> warned_;
    size_t allocSinceGc_ = 0;
};

// Descriptor helpers.
std::string descriptorToJavaName(const std::string& d);
std::string javaNameToDescriptor(const std::string& n);
uint32_t argWordsForShorty(const std::string& shorty, bool isStatic);
std::string shortyFromSignature(const std::string& sig);

}  // namespace droid
