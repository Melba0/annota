// Annota - builtins.hpp
#pragma once
#include "vm.hpp"

namespace annota {

void registerBuiltins(VM& vm);
// pseudo methods of built-in values (List, Tuple, String, Bytes) - null when absent
Value builtinMethod(VM& vm, const Value& obj, const std::string& name);
// `builtin` names such as int/float/bool/str/len/Bytes/List/Tuple
Value builtinFunction(VM& vm, const std::string& name);
// builtin GUI components: Text, Button, Column, ...
Value builtinComponent(VM& vm, const std::string& name);
bool isBuiltinComponent(const std::string& name);
Value makeUiNode(const std::string& type, std::unordered_map<std::string, Value> attrs);
// standard library modules: math, io, json, time, net
Value builtinModule(const std::string& name);
bool isBuiltinModule(const std::string& name);

} // namespace annota
