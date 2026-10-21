// Class loading, linking, allocation, GC and invocation.
//
// How a class comes to life here:
//   1. findClass("Lcom/example/Main;") looks in the loaded classes,
//      then in the dex files, then among native (C++) framework classes.
//   2. defineDexClass() loads the superclass and interfaces first, then
//      creates Field/Method objects for everything declared in the dex.
//   3. link() assigns every instance field a slot in Object::fields.
//   4. ensureInitialized() sets static field initial values and runs
//      <clinit> the first time the class is really used (JVM spec 5.5).
#include "vm/vm.h"

#include "framework/natives.h"

#include <algorithm>
#include <iostream>

namespace droid {

// ------------------------------------------------------------- helpers

std::string descriptorToJavaName(const std::string& d) {
    if (d.size() >= 2 && d[0] == 'L' && d.back() == ';') {
        std::string n = d.substr(1, d.size() - 2);
        std::replace(n.begin(), n.end(), '/', '.');
        return n;
    }
    return d;
}

std::string javaNameToDescriptor(const std::string& n) {
    if (!n.empty() && n[0] == '[') return n;
    std::string d = n;
    std::replace(d.begin(), d.end(), '.', '/');
    return "L" + d + ";";
}

// The shorty of "(ILjava/lang/String;[J)V" is "VILL": return type first,
// every reference type collapsed to 'L'.
std::string shortyFromSignature(const std::string& sig) {
    std::string params, ret;
    size_t i = 1;
    auto one = [&](size_t& p) {
        char c = sig[p];
        while (sig[p] == '[') ++p;
        if (sig[p] == 'L') p = sig.find(';', p);
        ++p;
        return c == '[' ? 'L' : c;
    };
    while (i < sig.size() && sig[i] != ')') params += one(i);
    ++i;
    ret = one(i);
    return ret + params;
}

uint32_t argWordsForShorty(const std::string& shorty, bool isStatic) {
    uint32_t n = isStatic ? 0 : 1;
    for (size_t i = 1; i < shorty.size(); ++i) n += (shorty[i] == 'J' || shorty[i] == 'D') ? 2 : 1;
    return n;
}

std::string Method::prettyName() const {
    return owner->javaName() + "." + name + signature;
}

std::string Class::javaName() const { return descriptorToJavaName(descriptor); }

// --------------------------------------------------------------- Class

Method* Class::declaredMethod(const std::string& name, const std::string& sig) const {
    for (auto& m : methods)
        if (m->name == name && m->signature == sig) return m.get();
    return nullptr;
}

Method* Class::findMethod(const std::string& name, const std::string& sig) {
    for (Class* c = this; c; c = c->super)
        if (Method* m = c->declaredMethod(name, sig)) return m;
    for (Class* c = this; c; c = c->super)
        for (Class* i : c->interfaces)
            if (Method* m = i->findMethod(name, sig)) return m;
    return nullptr;
}

// Virtual dispatch: the most derived non-abstract implementation wins,
// then default methods from interfaces. Results are cached per class.
Method* Class::findVirtual(const std::string& name, const std::string& sig) {
    std::string key = name + sig;
    auto it = vcache.find(key);
    if (it != vcache.end()) return it->second;
    Method* found = nullptr;
    for (Class* c = this; c && !found; c = c->super) {
        Method* m = c->declaredMethod(name, sig);
        if (m && !m->isAbstract() && !m->isStatic()) found = m;
    }
    if (!found) {
        std::function<Method*(Class*)> searchIfaces = [&](Class* c) -> Method* {
            for (Class* i : c->interfaces) {
                Method* m = i->declaredMethod(name, sig);
                if (m && !m->isAbstract()) return m;
                if (Method* r = searchIfaces(i)) return r;
            }
            return nullptr;
        };
        for (Class* c = this; c && !found; c = c->super) found = searchIfaces(c);
    }
    vcache[key] = found;
    return found;
}

Method* Class::dispatch(const Method* ref) {
    auto it = dcache.find(ref);
    if (it != dcache.end()) return it->second;
    Method* m = findVirtual(ref->name, ref->signature);
    dcache[ref] = m;
    return m;
}

Field* Class::findField(const std::string& name) {
    for (Class* c = this; c; c = c->super) {
        for (auto& f : c->fields)
            if (f->name == name) return f.get();
        for (Class* i : c->interfaces)
            if (Field* f = i->findField(name)) return f;
    }
    return nullptr;
}

bool Class::isSubclassOf(const Class* other) const {
    for (const Class* c = this; c; c = c->super)
        if (c == other) return true;
    return false;
}

bool Class::implements(const Class* iface) const {
    for (const Class* c = this; c; c = c->super)
        for (const Class* i : c->interfaces)
            if (i == iface || i->implements(iface)) return true;
    return false;
}

bool Class::isAssignableTo(const Class* target) const {
    if (this == target || target->descriptor == "Ljava/lang/Object;") return true;
    if (isArray()) {
        if (!target->isArray()) return target->descriptor == "Ljava/lang/Cloneable;" ||
                                       target->descriptor == "Ljava/io/Serializable;";
        if (component->descriptor.size() == 1 || target->component->descriptor.size() == 1)
            return component == target->component;  // primitive arrays must match exactly
        return component->isAssignableTo(target->component);
    }
    return target->isInterface() ? implements(target) : isSubclassOf(target);
}

Method* Class::addNative(const std::string& name, const std::string& sig, NativeFn fn, bool isStatic) {
    auto m = std::make_unique<Method>();
    m->owner = this;
    m->name = name;
    m->signature = sig;
    m->shorty = shortyFromSignature(sig);
    m->access = kPublic | kNative | (isStatic ? kStatic : 0);
    m->native = std::move(fn);
    m->argWords = argWordsForShorty(m->shorty, isStatic);
    methods.push_back(std::move(m));
    vcache.clear();
    dcache.clear();
    return methods.back().get();
}

Field* Class::addField(const std::string& name, const std::string& type) {
    fields.push_back(std::make_unique<Field>(Field{this, name, type, false}));
    return fields.back().get();
}

Field* Class::addStaticField(const std::string& name, const std::string& type, Reg value) {
    fields.push_back(std::make_unique<Field>(Field{this, name, type, true}));
    fields.back()->staticValue = value;
    return fields.back().get();
}

// ------------------------------------------------------------------ VM

VM::VM() {
    registerJavaLang(*this);
    registerAndroid(*this);
    // Native classes get their fields added after definition, so they are
    // linked once everything is registered (parents were defined first).
    for (Class* c : nativeOrder_) link(c);
    initJavaLangStatics(*this);
    initAndroidStatics(*this);
}

VM::~VM() {
    for (Object* o : heap_) delete o;
}

void VM::warnOnce(const std::string& msg) {
    if (warned_.insert(msg).second) std::cerr << "[droidvm] " << msg << "\n";
}

void VM::loadDex(std::vector<uint8_t> bytes) {
    auto cache = std::make_unique<DexCache>();
    cache->dex = std::make_unique<DexFile>(std::move(bytes));
    const DexFile& d = *cache->dex;
    cache->strings.assign(d.stringCount(), nullptr);
    cache->types.assign(d.typeCount(), nullptr);
    cache->methods.assign(d.methodCount(), nullptr);
    cache->fields.assign(d.fieldCount(), nullptr);
    cache->methodSigs.resize(d.methodCount());
    for (auto& def : d.classDefs()) {
        const std::string& desc = d.typeDescriptor(def.classIdx);
        if (!dexClasses_.count(desc)) dexClasses_[desc] = {cache.get(), &def};
    }
    dexes_.push_back(std::move(cache));
}

VM::DexCache& VM::cacheFor(const DexFile* dex) {
    for (auto& c : dexes_)
        if (c->dex.get() == dex) return *c;
    throw std::logic_error("unknown dex file");
}

Class* VM::findClassOrNull(const std::string& descriptor) {
    auto it = classes_.find(descriptor);
    if (it != classes_.end()) return it->second.get();
    if (descriptor[0] == '[') return makeArrayClass(descriptor);
    if (descriptor.size() == 1) {  // primitive type, used as array component
        auto c = std::make_unique<Class>();
        c->descriptor = descriptor;
        c->state = Class::Initialized;
        Class* raw = c.get();
        classes_[descriptor] = std::move(c);
        return raw;
    }
    auto dit = dexClasses_.find(descriptor);
    if (dit != dexClasses_.end()) return defineDexClass(*dit->second.first, *dit->second.second);
    return nullptr;
}

Class* VM::findClass(const std::string& descriptor) {
    if (Class* c = findClassOrNull(descriptor)) return c;
    return makeStubClass(descriptor);
}

// Framework classes we have not implemented yet still need to exist so that
// apps referencing them can load. Calling their methods logs a warning.
Class* VM::makeStubClass(const std::string& descriptor) {
    warnOnce("class not implemented, using stub: " + descriptorToJavaName(descriptor));
    auto c = std::make_unique<Class>();
    c->descriptor = descriptor;
    c->super = objectClass;
    c->isStub = true;
    c->state = Class::Initialized;
    Class* raw = c.get();
    classes_[descriptor] = std::move(c);
    link(raw);
    return raw;
}

Class* VM::makeArrayClass(const std::string& descriptor) {
    auto c = std::make_unique<Class>();
    c->descriptor = descriptor;
    c->super = objectClass;
    c->access = kPublic | kFinal;
    c->state = Class::Initialized;
    Class* raw = c.get();
    classes_[descriptor] = std::move(c);
    raw->component = findClass(descriptor.substr(1));
    return raw;
}

Class* VM::defineNativeClass(const std::string& descriptor, const std::string& superDescriptor,
                             std::vector<std::string> interfaces, uint32_t access) {
    auto c = std::make_unique<Class>();
    c->descriptor = descriptor;
    c->access = access;
    c->state = Class::Initialized;
    Class* raw = c.get();
    classes_[descriptor] = std::move(c);
    if (!superDescriptor.empty()) raw->super = findClass(superDescriptor);
    for (auto& i : interfaces) raw->interfaces.push_back(findClass(i));
    if (raw->super && !raw->peerFactory) raw->peerFactory = raw->super->peerFactory;
    nativeOrder_.push_back(raw);
    return raw;
}

Class* VM::defineDexClass(DexCache& cache, const DexClassDef& def) {
    const DexFile& d = *cache.dex;
    auto c = std::make_unique<Class>();
    c->descriptor = d.typeDescriptor(def.classIdx);
    c->access = def.access;
    c->dex = &d;
    c->def = &def;
    Class* raw = c.get();
    classes_[raw->descriptor] = std::move(c);  // register first: classes may refer to themselves

    if (def.superIdx != kNoIndex) raw->super = findClass(d.typeDescriptor(def.superIdx));
    for (uint16_t i : def.interfaces) raw->interfaces.push_back(findClass(d.typeDescriptor(i)));
    if (raw->super) raw->peerFactory = raw->super->peerFactory;

    for (auto& f : def.staticFields) {
        const DexFieldId& id = d.field(f.fieldIdx);
        raw->fields.push_back(std::make_unique<Field>(
            Field{raw, d.stringUtf8(id.nameIdx), d.typeDescriptor(id.typeIdx), true}));
    }
    for (auto& f : def.instanceFields) {
        const DexFieldId& id = d.field(f.fieldIdx);
        raw->fields.push_back(std::make_unique<Field>(
            Field{raw, d.stringUtf8(id.nameIdx), d.typeDescriptor(id.typeIdx), false}));
    }
    auto addMethods = [&](const std::vector<DexEncodedMethod>& list) {
        for (auto& em : list) {
            const DexMethodId& id = d.method(em.methodIdx);
            auto m = std::make_unique<Method>();
            m->owner = raw;
            m->name = d.stringUtf8(id.nameIdx);
            m->signature = d.protoSignature(id.protoIdx);
            m->shorty = d.proto(id.protoIdx).shorty;
            m->access = em.access;
            m->dex = &d;
            if (em.codeOff) {
                m->code = d.code(em.codeOff);
                m->hasCode = true;
            }
            m->argWords = argWordsForShorty(m->shorty, m->isStatic());
            raw->methods.push_back(std::move(m));
        }
    };
    addMethods(def.directMethods);
    addMethods(def.virtualMethods);
    link(raw);
    return raw;
}

void VM::link(Class* c) {
    uint32_t slot = c->super ? c->super->instanceSlots : 0;
    if (c->super) c->refSlots = c->super->refSlots;
    for (auto& f : c->fields) {
        if (f->isStatic) continue;
        f->slot = slot++;
        if (f->isRef()) c->refSlots.push_back(f->slot);
    }
    c->instanceSlots = slot;
}

void VM::ensureInitialized(Class* c) {
    if (c->state != Class::Linked) return;  // done, or in progress on this thread
    c->state = Class::Initializing;
    if (c->super) ensureInitialized(c->super);

    // Static initial values come from the class_def's encoded array.
    auto values = c->dex->encodedArray(c->def->staticValuesOff);
    for (size_t i = 0; i < values.size() && i < c->def->staticFields.size(); ++i) {
        Field* f = resolveField(c->dex, c->def->staticFields[i].fieldIdx);
        const DexValue& v = values[i];
        switch (v.type) {
        case DexValue::String: f->staticValue = fromObj(resolveString(c->dex, uint32_t(v.bits))); break;
        case DexValue::TypeRef: f->staticValue = fromObj(classMirror(resolveType(c->dex, uint32_t(v.bits)))); break;
        case DexValue::Null: f->staticValue = 0; break;
        case DexValue::Long: case DexValue::Double: f->staticValue = v.bits; break;
        default: f->staticValue = uint32_t(v.bits); break;
        }
    }
    if (Method* clinit = c->declaredMethod("<clinit>", "()V")) invoke(clinit, nullptr);
    c->state = Class::Initialized;
}

// ---------------------------------------------------------- resolution

StringObject* VM::resolveString(const DexFile* dex, uint32_t idx) {
    DexCache& cache = cacheFor(dex);
    if (!cache.strings[idx]) {
        const std::u16string& s = dex->string(idx);
        auto it = interned_.find(s);
        if (it != interned_.end()) {
            cache.strings[idx] = it->second;
        } else {
            StringObject* so = newString(s);
            interned_[s] = so;
            cache.strings[idx] = so;
        }
    }
    return cache.strings[idx];
}

Class* VM::resolveType(const DexFile* dex, uint32_t idx) {
    DexCache& cache = cacheFor(dex);
    if (!cache.types[idx]) cache.types[idx] = findClass(dex->typeDescriptor(idx));
    return cache.types[idx];
}

const std::string& VM::methodKey(const DexFile* dex, uint32_t idx) {
    DexCache& cache = cacheFor(dex);
    if (cache.methodSigs[idx].empty()) {
        const DexMethodId& id = dex->method(idx);
        cache.methodSigs[idx] = dex->stringUtf8(id.nameIdx) + dex->protoSignature(id.protoIdx);
    }
    return cache.methodSigs[idx];
}

Method* VM::resolveMethod(const DexFile* dex, uint32_t idx) {
    DexCache& cache = cacheFor(dex);
    if (cache.methods[idx]) return cache.methods[idx];
    const DexMethodId& id = dex->method(idx);
    Class* c = resolveType(dex, id.classIdx);
    std::string name = dex->stringUtf8(id.nameIdx);
    std::string sig = dex->protoSignature(id.protoIdx);
    Method* m = c->findMethod(name, sig);
    if (!m) {
        // Not implemented anywhere: synthesize a method that logs and returns 0/null.
        std::string what = descriptorToJavaName(c->descriptor) + "." + name + sig;
        std::string shorty = dex->proto(id.protoIdx).shorty;
        m = c->addNative(name, sig, [this, what](VM&, Reg*) -> Reg {
            warnOnce("method not implemented: " + what);
            return 0;
        });
        // We cannot know whether the caller expects a static method; the
        // interpreter passes the right number of words either way.
    }
    cache.methods[idx] = m;
    return m;
}

Field* VM::resolveField(const DexFile* dex, uint32_t idx) {
    DexCache& cache = cacheFor(dex);
    if (cache.fields[idx]) return cache.fields[idx];
    const DexFieldId& id = dex->field(idx);
    Class* c = resolveType(dex, id.classIdx);
    std::string name = dex->stringUtf8(id.nameIdx);
    Field* f = c->findField(name);
    if (!f) {
        warnOnce("field not implemented: " + c->javaName() + "." + name);
        // A static placeholder keeps the program running (reads give 0/null).
        f = c->addStaticField(name, dex->typeDescriptor(id.typeIdx));
    }
    cache.fields[idx] = f;
    return f;
}

// ---------------------------------------------------------- allocation

Object* VM::alloc(Class* c) {
    if (c == stringClass) return newString(std::u16string());  // `new String(...)` in bytecode
    ensureInitialized(c);
    Object* o = new Object();
    o->klass = c;
    o->fields.assign(c->instanceSlots, 0);
    for (Class* k = c; k; k = k->super) {
        if (k->peerFactory) { o->peer = k->peerFactory(); break; }
    }
    heap_.push_back(o);
    heapSet_.insert(o);
    ++allocSinceGc_;
    return o;
}

StringObject* VM::newString(const std::u16string& s) {
    auto* o = new StringObject();
    o->klass = stringClass;
    o->kind = Object::String;
    o->value = s;
    heap_.push_back(o);
    heapSet_.insert(o);
    ++allocSinceGc_;
    return o;
}

ArrayObject* VM::newArray(Class* arrayClass, int32_t length) {
    if (length < 0) throwNew("Ljava/lang/NegativeArraySizeException;", std::to_string(length));
    auto* o = new ArrayObject();
    o->klass = arrayClass;
    o->kind = Object::Array;
    o->data.assign(size_t(length), 0);
    heap_.push_back(o);
    heapSet_.insert(o);
    ++allocSinceGc_;
    return o;
}

Object* VM::classMirror(Class* c) {
    if (!c->mirror) {
        c->mirror = alloc(classClass);
        c->mirror->peer = std::make_unique<ClassMirrorPeer>(c);
        addRoot(c->mirror);
    }
    return c->mirror;
}

// ------------------------------------------------------------- invoke

Reg VM::invoke(Method* m, Reg* args) {
    if (m->native) return m->native(*this, args);
    if (!m->hasCode) throwNew("Ljava/lang/AbstractMethodError;", m->prettyName());
    return interpret(m, args);
}

Reg VM::callVirtual(Object* receiver, const std::string& name, const std::string& sig, std::vector<Reg> extra) {
    if (!receiver) throwNPE("calling " + name + " on null");
    Method* m = receiver->klass->findVirtual(name, sig);
    if (!m) {
        warnOnce("no method " + receiver->klass->javaName() + "." + name + sig);
        return 0;
    }
    std::vector<Reg> args;
    args.push_back(fromObj(receiver));
    args.insert(args.end(), extra.begin(), extra.end());
    return invoke(m, args.data());
}

Reg VM::callStatic(Class* c, const std::string& name, const std::string& sig, std::vector<Reg> args) {
    ensureInitialized(c);
    Method* m = c->findMethod(name, sig);
    if (!m) throwNew("Ljava/lang/NoSuchMethodError;", c->javaName() + "." + name + sig);
    return invoke(m, args.data());
}

Object* VM::construct(Class* c, const std::string& sig, std::vector<Reg> args) {
    Object* o = alloc(c);
    Method* init = c->findMethod("<init>", sig);
    if (!init) throwNew("Ljava/lang/NoSuchMethodError;", c->javaName() + ".<init>" + sig);
    args.insert(args.begin(), fromObj(o));
    invoke(init, args.data());
    return o;
}

// ---------------------------------------------------------- exceptions

void VM::throwNew(const std::string& descriptor, const std::string& message) {
    Class* c = findClass(descriptor);
    Object* ex = alloc(c);
    Reg args[2] = {fromObj(ex), fromObj(message.empty() ? nullptr : newString(message))};
    if (Method* init = c->findMethod("<init>", "(Ljava/lang/String;)V")) invoke(init, args);
    throw JavaThrow{ex};
}

std::string VM::describeException(Object* ex) {
    if (!ex) return "null";
    std::string s = ex->klass->javaName();
    if (Field* f = ex->klass->findField("detailMessage")) {
        if (Object* msg = asObj(ex->fields[f->slot])) s += ": " + utf8(msg);
    }
    return s;
}

std::vector<std::string> VM::stackTrace() const {
    std::vector<std::string> out;
    for (auto it = frames.rbegin(); it != frames.rend(); ++it)
        out.push_back((*it)->method->prettyName() + " @pc " + std::to_string((*it)->pc));
    return out;
}

// --------------------------------------------------------------- strings

std::string VM::utf8(Object* str) {
    if (!str) return "null";
    if (str->kind != Object::String) return "<" + str->klass->javaName() + ">";
    return utf16ToUtf8(static_cast<StringObject*>(str)->value);
}

std::u16string VM::string16Of(Object* o) {
    if (!o) return u"null";
    if (o->kind == Object::String) return static_cast<StringObject*>(o)->value;
    Object* s = asObj(callVirtual(o, "toString", "()Ljava/lang/String;"));
    return s && s->kind == Object::String ? static_cast<StringObject*>(s)->value : u"null";
}

std::string VM::stringOf(Object* o) { return utf16ToUtf8(string16Of(o)); }

// ------------------------------------------------------------------ GC
//
// A simple mark & sweep collector. Roots: static fields, interned strings,
// explicitly registered roots (activities, pending callbacks...) and, scanned
// conservatively, every register of every active interpreter frame.

void VM::removeRoot(Object* o) {
    auto it = roots_.find(o);
    if (it != roots_.end() && --it->second <= 0) roots_.erase(it);
}

void VM::collectGarbage() {
    std::vector<Object*> stack;
    auto push = [&](Object* o) {
        if (o && !o->marked) { o->marked = true; stack.push_back(o); }
    };
    auto pushReg = [&](Reg r) {
        Object* o = asObj(r);
        if (heapSet_.count(o)) push(o);
    };

    for (auto& [name, c] : classes_) {
        for (auto& f : c->fields)
            if (f->isStatic && f->isRef()) push(asObj(f->staticValue));
        push(c->mirror);
    }
    for (auto& [s, o] : interned_) push(o);
    for (auto& [o, n] : roots_) push(o);
    for (Frame* f : frames)
        for (uint16_t i = 0; i < f->nregs; ++i) pushReg(f->regs[i]);

    std::vector<Object*> refs;
    while (!stack.empty()) {
        Object* o = stack.back();
        stack.pop_back();
        for (uint32_t slot : o->klass->refSlots) push(asObj(o->fields[slot]));
        if (o->kind == Object::Array && o->klass->component->descriptor.size() > 1)
            for (Reg r : static_cast<ArrayObject*>(o)->data) push(asObj(r));
        if (o->peer) {
            refs.clear();
            o->peer->trace(refs);
            for (Object* r : refs) push(r);
        }
    }

    size_t kept = 0;
    for (Object* o : heap_) {
        if (o->marked) {
            o->marked = false;
            heap_[kept++] = o;
        } else {
            heapSet_.erase(o);
            delete o;
        }
    }
    heap_.resize(kept);
    allocSinceGc_ = 0;
}

}  // namespace droid
