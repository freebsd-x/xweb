#pragma once

// What the interpreter and the built-ins share. Internal to the engine.

#include <memory>
#include <string>
#include <unordered_map>

#include "ast.h"
#include "xweb/js.h"

namespace fbx::xweb::js {

struct Binding {
    Value value;
    bool is_mutable = true;
    bool initialized = true;
};

struct Env : Cell {
    explicit Env(Env* o) : outer(o) {}
    Env* outer;
    std::unordered_map<std::u16string, Binding> vars;
    // A function's own: what `this`, `super` and `new.target` mean inside it.
    bool is_function = false;
    Value this_value;
    bool this_initialized = true;
    Object* function = nullptr;
    Object* home = nullptr;
    Object* new_target = nullptr;
    // The global scope's: its var names are the global object's properties.
    Object* global_object = nullptr;
};

struct FunctionData {
    FunctionInfo* info = nullptr;
    Env* closure = nullptr;
    Object* home = nullptr;          // a method's object, for super
    std::shared_ptr<Program> program;   // keeps the tree alive
    Object* class_fields_home = nullptr;
};

}  // namespace fbx::xweb::js
