// Annota - module_loader.hpp : the one file-based `use` resolver.
//
// The CLI and the analyzer must agree on module resolution, otherwise a program runs but the
// IDE reports its module globals as undefined.  Everything lives here: the search path, the
// `name` / `dir/name` / `name.mod` forms, sibling directories of loaded modules, and the
// "cannot find module" hint (searched dirs, available modules, closest name).
#pragma once
#include "parser.hpp"
#include "ffi.hpp"
#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <system_error>
#include <vector>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#else
#include <unistd.h>
#endif

namespace annota {

// tiny Levenshtein distance, used to suggest the closest module name on a typo
inline size_t moduleNameDistance(const std::string& a, const std::string& b) {
    std::vector<size_t> prev(b.size() + 1), cur(b.size() + 1);
    for (size_t j = 0; j <= b.size(); j++) prev[j] = j;
    for (size_t i = 1; i <= a.size(); i++) {
        cur[0] = i;
        for (size_t j = 1; j <= b.size(); j++) {
            size_t cost = a[i - 1] == b[j - 1] ? 0 : 1;
            cur[j] = std::min(std::min(prev[j] + 1, cur[j - 1] + 1), prev[j - 1] + cost);
        }
        prev = cur;
    }
    return prev[b.size()];
}

inline std::string moduleDirOf(const std::string& path) {
    size_t q = path.find_last_of("/\\");
    return q == std::string::npos ? std::string(".") : path.substr(0, q);
}

// directory of the running executable (used to find the standard library next to it)
inline std::string executableDir() {
    static std::string cached;
    if (!cached.empty()) return cached;
#ifdef _WIN32
    char buf[MAX_PATH];
    DWORD n = GetModuleFileNameA(nullptr, buf, MAX_PATH);
    cached = n > 0 ? moduleDirOf(std::string(buf, buf + n)) : std::string(".");
#else
    char buf[4096];
    ssize_t n = readlink("/proc/self/exe", buf, sizeof(buf) - 1);
    cached = n > 0 ? moduleDirOf(std::string(buf, buf + n)) : std::string(".");
#endif
    return cached;
}

inline void addSearchDir(std::vector<std::string>& dirs, const std::string& d) {
    if (d.empty()) return;
    for (auto& s : dirs)
        if (s == d) return;
    dirs.push_back(d);
}

struct FileModuleLoader : ModuleLoader {
    std::vector<std::string> searchDirs;

    explicit FileModuleLoader(std::string dir) {
        // the program's own directory, the repository layout around it, the executable's
        // directory (so a GUI or an installed binary finds lib/ too) and finally the cwd
        addSearchDir(searchDirs, dir);
        addSearchDir(searchDirs, dir + "/lib");
        addSearchDir(searchDirs, dir + "/../lib");
        std::string exe = executableDir();
        addSearchDir(searchDirs, exe);
        addSearchDir(searchDirs, exe + "/lib");
        addSearchDir(searchDirs, exe + "/../lib");
        addSearchDir(searchDirs, "lib");
        addSearchDir(searchDirs, ".");
    }

    static bool readable(const std::string& p) {
        std::FILE* f = std::fopen(p.c_str(), "rb");
        if (!f) return false;
        std::fclose(f);
        return true;
    }

    // keep the directory a module was loaded from, so that modules can `use` their siblings
    void remember(const std::string& file) {
        std::string d = moduleDirOf(file);
        for (auto& s : searchDirs)
            if (s == d) return;
        searchDirs.insert(searchDirs.begin(), d);
    }

    // `use <name>` may name a plugin instead of a .mod file: look for <name>.dll / lib<name>.so
    bool ensureModule(const std::string& name) override {
        if (ffiHasAny(name)) return true;
        std::vector<std::string> dirs = searchDirs;
        dirs.push_back(executableDir() + "/plugins");
        dirs.push_back("plugins");
        return ffiLoadPluginFor(name, dirs);
    }

    bool loadModule(const std::string& spec, std::vector<Token>& toks, std::string& file) override {
        std::vector<std::string> candidates;
        bool looksLikePath = spec.find('/') != std::string::npos ||
                             spec.find('\\') != std::string::npos ||
                             spec.find(".mod") != std::string::npos;
        if (looksLikePath) {
            candidates.push_back(spec);
            candidates.push_back(spec + ".mod");        // `use sub/name` finds `sub/name.mod`
            for (auto& d : searchDirs) {
                candidates.push_back(d + "/" + spec);
                candidates.push_back(d + "/" + spec + ".mod");
            }
        } else {
            for (auto& d : searchDirs) candidates.push_back(d + "/" + spec + ".mod");
            candidates.push_back(spec + ".mod");
        }
        for (auto& c : candidates) {
            if (!readable(c)) continue;
            std::ifstream in(c, std::ios::binary);
            std::stringstream ss;
            ss << in.rdbuf();
            file = c;
            toks = lex(ss.str(), c);
            remember(c);
            return true;
        }
        return false;   // not a file module - maybe a native one
    }

    std::string searchHint(const std::string& spec) override {
        std::string msg = " (searched ";
        for (size_t i = 0; i < searchDirs.size(); i++) {
            if (i) msg += ", ";
            msg += searchDirs[i].empty() ? "." : searchDirs[i];
        }
        msg += ")";
        std::vector<std::string> names;
        for (auto& d : searchDirs) {
            std::error_code ec;
            for (auto& e : std::filesystem::directory_iterator(d, ec)) {
                if (ec) break;
                if (!e.is_regular_file(ec)) continue;
                std::string p = e.path().string();
                if (p.size() <= 4 || p.compare(p.size() - 4, 4, ".mod") != 0) continue;
                std::string base = e.path().stem().string();
                if (std::find(names.begin(), names.end(), base) == names.end()) names.push_back(base);
            }
        }
        if (!names.empty()) {
            std::sort(names.begin(), names.end());
            std::string list;
            for (size_t i = 0; i < names.size(); i++) {
                if (i) list += ", ";
                list += names[i];
            }
            msg += "\n  available modules: " + list;
        }
        std::string want = spec;
        size_t cut = want.find_last_of("/\\");
        if (cut != std::string::npos) want = want.substr(cut + 1);
        if (want.size() > 4 && want.compare(want.size() - 4, 4, ".mod") == 0)
            want = want.substr(0, want.size() - 4);
        std::string best;
        size_t bestD = 3;
        for (auto& n : names) {
            size_t d = moduleNameDistance(want, n);
            if (d < bestD) { bestD = d; best = n; }
        }
        if (!best.empty()) msg += "\n  did you mean '" + best + "'?";
        return msg;
    }
};

} // namespace annota
