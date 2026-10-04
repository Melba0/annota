// Annota - gui.cpp : component tree utilities + the no-Qt stubs.
#include "gui.hpp"
#include <cstdio>

namespace annota {

static std::string reprValue(const Value& v) {
    switch (v.t) {
        case VT::Null: return "null";
        case VT::Bool: return v.b ? "true" : "false";
        case VT::Int: return formatInt(v.i);
        case VT::Float: return formatDouble(v.f);
        case VT::Str: return "\"" + v.o->str + "\"";
        case VT::Color: {
            char buf[16];
            std::snprintf(buf, sizeof(buf), "#%06x", (unsigned)(v.o->color & 0xFFFFFF));
            return buf;
        }
        case VT::List: {
            std::string s = "[";
            for (size_t i = 0; i < v.o->items.size(); i++) { if (i) s += ", "; s += reprValue(v.o->items[i]); }
            return s + "]";
        }
        case VT::Tuple: {
            std::string s = "(";
            for (size_t i = 0; i < v.o->items.size(); i++) { if (i) s += ", "; s += reprValue(v.o->items[i]); }
            return s + ")";
        }
        case VT::Function: return "<fn " + v.o->str + ">";
        case VT::Native: return "<native " + v.o->str + ">";
        case VT::Class: return "<class " + v.o->klass->name + ">";
        default: return v.typeName();
    }
}

static void treeNode(const Value& node, std::string& out, int indent) {
    if (node.t == VT::List || node.t == VT::Tuple) {
        for (auto& c : node.o->items) treeNode(c, out, indent);
        return;
    }
    if (node.t != VT::UiNode) return;
    std::string pad((size_t)indent * 2, ' ');
    out += pad + node.o->str;
    for (auto& kv : node.o->map) {
        if (kv.first.rfind("arg", 0) == 0) continue;
        out += " " + kv.first + "=" + reprValue(kv.second);
    }
    out += "\n";
    for (auto& c : node.o->items) treeNode(c, out, indent + 1);
}

std::string renderTree(const Value& root) {
    std::string out;
    treeNode(root, out, 0);
    return out;
}

#ifndef ANNOTA_QT
// One place that explains what to do about a build without Qt, so the message is the same
// everywhere and nobody has to guess.
std::string noQtAdvice() {
    return std::string(
        "\n"
        "  这个二进制在构建时没有找到 Qt 6，因此没有窗口能力。三种选择：\n"
        "    1) 装好 Qt 6 后重新构建（推荐）：\n"
        "         powershell -ExecutionPolicy Bypass -File build.ps1 -QtRoot D:\\Qt\\6.10.1\\mingw_64\n"
        "    2) 只想要文本输出：用 --gui-tree（组件树）或 --gui-shot（不适用于无 Qt 构建）\n"
        "    3) 确认当前用的是新构建的 exe：build\\annota.exe --features  应包含 gui\n");
}

bool guiAvailable() { return false; }

int guiInit(int, char**) { return 0; }
int guiRunView(VM&, const std::string& viewName) {
    std::fprintf(stderr, "annota: 无法打开窗口：当前构建没有 Qt 支持（view '%s'）。%s",
                 viewName.c_str(), noQtAdvice().c_str());
    return 2;
}
void guiInstallInput(VM&) {}     // console builds read stdin directly

int guiRun(VM&, const std::string& viewName, int, char**) {
    std::fprintf(stderr, "annota: 无法打开窗口：当前构建没有 Qt 支持（view '%s'）。%s",
                 viewName.c_str(), noQtAdvice().c_str());
    return 2;
}

int guiShowView(VM&, const std::string& viewName) {
    std::fprintf(stderr, "annota: 无法预览界面：当前构建没有 Qt 支持（view '%s'）。%s",
                 viewName.c_str(), noQtAdvice().c_str());
    return 2;
}

int guiRenderPng(VM&, const std::string&, const std::string&,
                 const std::vector<std::pair<int, int>>&, const std::vector<std::string>&) {
    std::fprintf(stderr, "annota: --gui-shot 不可用：当前构建没有 Qt 支持。%s", noQtAdvice().c_str());
    return 2;
}

void guiViewSize(VM&, const std::string&, int& w, int& h, std::string& title) {
    w = h = 0;
    title.clear();
}
#else
std::string noQtAdvice() { return std::string(); }
#endif

} // namespace annota
