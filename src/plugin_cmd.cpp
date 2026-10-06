// Annota - plugin_cmd.cpp : `annota plugin build <src.cpp>`.
//
// Turns one C++ file into a loadable plugin, so extending the standard library with native code
// needs no rebuild of the interpreter:
//
//     annota plugin build mycode.cpp          -> build/plugins/<name>.{dll,so}
//     annota run program.ant                  -> `use <name>` finds it and loads it
//
// The plugin registers modules/functions through src/ffi.hpp; see docs/ffi.md.
#include "ffi.hpp"
#include "module_loader.hpp"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace annota {
namespace {

bool fileExists(const std::string& p) {
    std::FILE* f = std::fopen(p.c_str(), "rb");
    if (!f) return false;
    std::fclose(f);
    return true;
}

std::string stemOf(const std::string& p) {
    size_t q = p.find_last_of("/\\");
    std::string b = q == std::string::npos ? p : p.substr(q + 1);
    size_t d = b.find_last_of('.');
    return d == std::string::npos ? b : b.substr(0, d);
}

std::string shq(const std::string& s) { return "\"" + s + "\""; }   // shell quoting

bool haveCompiler(const std::string& cxx) {
#ifdef _WIN32
    std::string cmd = "where " + shq(cxx) + " > NUL 2>&1";     // silent when absent
#else
    std::string cmd = "command -v " + shq(cxx) + " > /dev/null 2>&1";
#endif
    if (std::system(cmd.c_str()) == 0) return true;
    std::FILE* f = std::fopen(cxx.c_str(), "rb");               // a recorded full path counts
    if (f) {
        std::fclose(f);
        return true;
    }
    return false;
}

std::string pickCompiler() {
    if (const char* env = std::getenv("CXX")) {
        if (*env && haveCompiler(env)) return env;
    }
    // the compiler that built this interpreter is recorded in build-info.txt next to it
    std::ifstream info(executableDir() + "/build-info.txt");
    std::string line;
    while (std::getline(info, line)) {
        if (line.rfind("cxx=", 0) == 0) {
            std::string c = line.substr(4);
            while (!c.empty() && (c.back() == '\r' || c.back() == '\n' || c.back() == ' ')) c.pop_back();
            if (!c.empty() && haveCompiler(c)) return c;
        }
    }
    for (const char* cand : {"g++", "clang++", "c++"}) {
        if (haveCompiler(cand)) return cand;
    }
    return std::string();
}

// the checkout that holds src/ffi.hpp, looked up next to the executable and in the cwd
std::string includeDir() {
    std::vector<std::string> cands = {executableDir() + "/../src", executableDir() + "/src",
                                      "src", "../src", "../../src"};
    for (auto& c : cands)
        if (fileExists(c + "/ffi.hpp")) return c;
    return std::string();
}

void usage() {
    std::printf(
        "usage: annota plugin build <file.cpp> [-o out.dll] [-n name]\n"
        "  -o   where to write the plugin (default: <exe dir>/plugins/<name>.dll|so)\n"
        "  -n   module name (default: the file's base name)\n"
        "\n"
        "A plugin registers modules through src/ffi.hpp; `use <name>` then finds it.\n"
        "See docs/ffi.md.\n");
}

} // namespace

int cmdPlugin(int argc, char** argv) {
    if (argc < 2 || std::strcmp(argv[1], "build") != 0) {
        usage();
        return argc < 2 ? 2 : 0;
    }
    if (argc < 3) {
        usage();
        return 2;
    }
    std::string src = argv[2];
    std::string name = stemOf(src);
    std::string out;
    for (int i = 3; i < argc; i++) {
        if (std::strcmp(argv[i], "-o") == 0 && i + 1 < argc) out = argv[++i];
        else if (std::strcmp(argv[i], "-n") == 0 && i + 1 < argc) name = argv[++i];
        else {
            std::fprintf(stderr, "annota plugin: unknown option '%s'\n", argv[i]);
            return 2;
        }
    }
    if (!fileExists(src)) {
        std::fprintf(stderr, "annota plugin: cannot read '%s'\n", src.c_str());
        return 2;
    }
#ifdef _WIN32
    const char* suffix = ".dll";
    const char* prefix = "";           // hello.dll
#else
    const char* suffix = ".so";
    const char* prefix = "lib";        // libhello.so (the conventional spelling)
#endif
    if (out.empty()) {
        std::string dir = executableDir() + "/plugins";
        std::error_code ec;
        std::filesystem::create_directories(dir, ec);
        out = dir + "/" + prefix + name + suffix;
    } else {
        std::error_code ec;
        std::filesystem::create_directories(std::filesystem::path(out).parent_path(), ec);
    }
    std::string inc = includeDir();
    if (inc.empty()) {
        std::fprintf(stderr,
                     "annota plugin: cannot find src/ffi.hpp - run this from the Annota checkout, or\n"
                     "               put the plugin source next to an `src/` directory.\n");
        return 2;
    }
    std::string cxx = pickCompiler();
    if (cxx.empty()) {
        std::fprintf(stderr, "annota plugin: no C++ compiler found (set CXX, or install g++/clang++)\n");
        return 2;
    }
    std::string cmd = shq(cxx) + " -std=c++17 -O2 -shared -I" + shq(inc) + " " +
                      shq(src) + " -o " + shq(out);
#ifndef _WIN32
    cmd += " -fPIC";
#else
    // link the C++ runtime in, so the plugin does not depend on the loader's runtime
    cmd += " -static-libgcc -static-libstdc++";
    std::string implib = executableDir() + "/libannota.dll.a";
    if (fileExists(implib)) cmd += " " + shq(implib);          // import library, by path
    cmd += " 2>&1";                                            // keep diagnostics visible
#endif
    std::printf("annota plugin: %s\n", cmd.c_str());
    int rc = std::system(cmd.c_str());
    if (rc != 0) {
        std::fprintf(stderr, "annota plugin: build failed (exit %d)\n", rc);
        return 1;
    }
    if (!fileExists(out)) {
        std::fprintf(stderr, "annota plugin: no output at '%s'\n", out.c_str());
        return 1;
    }
    std::printf("annota plugin: wrote %s\n", out.c_str());
    std::printf("  `use %s` now loads it (the plugin directory is searched automatically)\n",
                name.c_str());
    return 0;
}

} // namespace annota
