// Annota - sys_api.cpp : the primitive ABI (see sys_api.hpp for the design rule).
//
// Everything in this file answers "what can the operating system do", never "how should that
// look to a program".  Formatting, retries, HTTP, thread pools and friends are in lib/*.mod.
#include "sys_api.hpp"
#include "builtins.hpp"
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <map>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#if defined(_WIN32)
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#else
// Declared at global scope on purpose: inside `namespace annota` this would resolve to
// `annota::environ`, which is a different (undefined) symbol at link time.
extern "C" char** environ;
#include <arpa/inet.h>
#include <errno.h>
#include <netdb.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>
#endif

namespace annota {
namespace {

// ---------------------------------------------------------------- argument helpers
Value argAt(std::vector<Value>& a, size_t i) { return i < a.size() ? a[i] : Value::null(); }

std::string asStr(VM& vm, const Value& v, const char* what) {
    if (v.t != VT::Str) vm.throwError(std::string(what) + " expects a string");
    return v.o->str;
}

int64_t asInt(VM& vm, const Value& v, const char* what, int64_t def = 0) {
    if (v.isNull()) return def;
    if (v.t == VT::Int) return v.i;
    if (v.t == VT::Float) return (int64_t)v.f;
    if (v.t == VT::Bool) return v.b ? 1 : 0;
    vm.throwError(std::string(what) + " expects a number");
}

std::string platformName() {
#if defined(_WIN32)
    return "windows";
#elif defined(__APPLE__)
    return "macos";
#elif defined(__linux__)
    return "linux";
#else
    return "unknown";
#endif
}

std::string archName() {
#if defined(_M_X64) || defined(__x86_64__)
    return "x64";
#elif defined(_M_ARM64) || defined(__aarch64__)
    return "arm64";
#elif defined(_M_IX86) || defined(__i386__)
    return "x86";
#else
    return "unknown";
#endif
}

int64_t wallMillis() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::system_clock::now().time_since_epoch()).count();
}
int64_t steadyMillis() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::steady_clock::now().time_since_epoch()).count();
}

std::tm localTm(int64_t ms) {
    std::time_t t = (std::time_t)(ms / 1000);
    std::tm tm{};
#if defined(_WIN32)
    localtime_s(&tm, &t);
#else
    localtime_r(&t, &tm);
#endif
    return tm;
}

// ---------------------------------------------------------------- tasks (threads)
// A worker gets its own VM whose globals are a deep copy of the caller's, so data does not leak
// between threads; code, classes and natives are shared because they are immutable.  Cells
// captured by a closure stay shared - that is the language's documented closure semantics, and
// it is the caller's job to keep them read-only inside a worker.
struct Task {
    std::thread worker;
    Value result;
    std::string error;
    std::atomic<bool> done{false};
    // a task the program never joined must not abort the process at exit
    ~Task() {
        if (worker.joinable()) worker.detach();
    }
};

std::mutex& taskMutex() {
    static std::mutex m;
    return m;
}
std::map<int64_t, std::shared_ptr<Task>>& tasks() {
    static std::map<int64_t, std::shared_ptr<Task>> t;
    return t;
}
std::atomic<int64_t>& nextTask() {
    static std::atomic<int64_t> id{1};
    return id;
}

std::shared_ptr<VM> workerVm(VM& parent) {
    auto vm = std::make_shared<VM>();
    registerBuiltins(*vm);
    vm->contracts = parent.contracts;
    vm->capture = true;
    vm->inputProvider = parent.inputProvider;
    for (auto& kv : parent.globals)
        if (kv.second) vm->globals[kv.first] = std::make_shared<Value>(deepCopy(*kv.second));
    return vm;
}

Value runOnWorker(VM& parent, const Value& fn, std::vector<Value> args, std::string& error) {
    auto vm = workerVm(parent);
    try {
        return vm->callSync(fn, std::move(args));
    } catch (VMError& e) {
        error = e.message;
    } catch (CompileError& e) {
        error = e.message;
    } catch (std::exception& e) {
        error = e.what();
    }
    return Value::null();
}

// ---------------------------------------------------------------- sockets
struct Socket {
#if defined(_WIN32)
    SOCKET fd = INVALID_SOCKET;
#else
    int fd = -1;
#endif
    bool open() const {
#if defined(_WIN32)
        return fd != INVALID_SOCKET;
#else
        return fd >= 0;
#endif
    }
    void close() {
        if (!open()) return;
#if defined(_WIN32)
        ::closesocket(fd);
        fd = INVALID_SOCKET;
#else
        ::close(fd);
        fd = -1;
#endif
    }
};

std::once_flag& winsockOnce() {
    static std::once_flag f;
    return f;
}
void ensureSockets() {
#if defined(_WIN32)
    std::call_once(winsockOnce(), [] {
        WSADATA wsa;
        WSAStartup(MAKEWORD(2, 2), &wsa);
    });
#endif
}

std::mutex& socketMutex() {
    static std::mutex m;
    return m;
}
std::map<int64_t, std::shared_ptr<Socket>>& sockets() {
    static std::map<int64_t, std::shared_ptr<Socket>> s;
    return s;
}
std::atomic<int64_t>& nextSocket() {
    static std::atomic<int64_t> id{1};
    return id;
}

int64_t storeSocket(std::shared_ptr<Socket> s) {
    int64_t id = nextSocket()++;
    std::lock_guard<std::mutex> lock(socketMutex());
    sockets()[id] = std::move(s);
    return id;
}

std::shared_ptr<Socket> socketById(VM& vm, int64_t id, const char* what) {
    std::lock_guard<std::mutex> lock(socketMutex());
    auto it = sockets().find(id);
    if (it == sockets().end() || !it->second->open())
        vm.throwError(std::string(what) + ": handle " + formatInt(id) + " is not an open socket");
    return it->second;
}

std::string socketError() {
#if defined(_WIN32)
    return "winsock error " + formatInt((long long)WSAGetLastError());
#else
    return std::strerror(errno);
#endif
}

// connect or bind+listen; shared by the two ops
addrinfo* resolveList(VM& vm, const std::string& host, int port, bool passive, const char* what) {
    ensureSockets();
    addrinfo hints{};
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    if (passive) hints.ai_flags = AI_PASSIVE;
    addrinfo* res = nullptr;
    std::string portStr = formatInt(port);
    const char* node = host.empty() ? nullptr : host.c_str();
    if (getaddrinfo(node, portStr.c_str(), &hints, &res) != 0 || !res)
        vm.throwError(std::string(what) + ": cannot resolve '" + host + "'");
    return res;
}

std::shared_ptr<Socket> makeSocket(addrinfo* it) {
    auto s = std::make_shared<Socket>();
#if defined(_WIN32)
    s->fd = ::socket(it->ai_family, it->ai_socktype, it->ai_protocol);
#else
    s->fd = ::socket(it->ai_family, it->ai_socktype, it->ai_protocol);
#endif
    return s;
}

// ---------------------------------------------------------------- the socket dispatcher
Value socketOp(VM& vm, std::vector<Value>& a) {
    std::string op = asStr(vm, argAt(a, 0), "_sys_socket");
    auto bad = [&](const std::string& why) -> Value {
        vm.throwError("_sys_socket('" + op + "'): " + why +
                      "\n  known ops: resolve, connect, listen, accept, send, recv, close, shutdown, timeout");
    };

    if (op == "resolve") {
        std::string host = asStr(vm, argAt(a, 1), "_sys_socket");
        addrinfo* res = resolveList(vm, host, 0, false, "_sys_socket('resolve')");
        std::vector<Value> out;
        char buf[INET6_ADDRSTRLEN];
        for (addrinfo* it = res; it; it = it->ai_next) {
            const char* s = nullptr;
            if (it->ai_family == AF_INET)
                s = inet_ntop(AF_INET, &((sockaddr_in*)it->ai_addr)->sin_addr, buf, sizeof(buf));
            else if (it->ai_family == AF_INET6)
                s = inet_ntop(AF_INET6, &((sockaddr_in6*)it->ai_addr)->sin6_addr, buf, sizeof(buf));
            if (s) out.push_back(Value::str(s));
        }
        freeaddrinfo(res);
        return Value::list(out);
    }
    if (op == "connect") {
        std::string host = asStr(vm, argAt(a, 1), "_sys_socket");
        int port = (int)asInt(vm, argAt(a, 2), "_sys_socket");
        addrinfo* res = resolveList(vm, host, port, false, "_sys_socket('connect')");
        for (addrinfo* it = res; it; it = it->ai_next) {
            auto s = makeSocket(it);
            if (!s->open()) continue;
#if defined(_WIN32)
            bool ok = ::connect(s->fd, it->ai_addr, (int)it->ai_addrlen) == 0;
#else
            bool ok = ::connect(s->fd, it->ai_addr, it->ai_addrlen) == 0;
#endif
            if (ok) {
                freeaddrinfo(res);
                return Value::integer(storeSocket(s));
            }
            s->close();
        }
        freeaddrinfo(res);
        return bad("cannot connect to " + host + ":" + formatInt(port) + " (" + socketError() + ")");
    }
    if (op == "listen") {
        std::string host = a.size() > 1 && argAt(a, 1).t == VT::Str ? argAt(a, 1).o->str : std::string();
        int port = (int)asInt(vm, argAt(a, a.size() > 1 && argAt(a, 1).t == VT::Str ? 2 : 1), "_sys_socket");
        addrinfo* res = resolveList(vm, host, port, true, "_sys_socket('listen')");
        for (addrinfo* it = res; it; it = it->ai_next) {
            auto s = makeSocket(it);
            if (!s->open()) continue;
            int one = 1;
            ::setsockopt(s->fd, SOL_SOCKET, SO_REUSEADDR, (const char*)&one, sizeof(one));
            bool ok = ::bind(s->fd, it->ai_addr, (int)it->ai_addrlen) == 0 && ::listen(s->fd, 8) == 0;
            if (ok) {
                freeaddrinfo(res);
                return Value::integer(storeSocket(s));
            }
            s->close();
        }
        freeaddrinfo(res);
        return bad("cannot listen on port " + formatInt(port) + " (" + socketError() + ")");
    }
    if (op == "accept") {
        auto server = socketById(vm, asInt(vm, argAt(a, 1), "_sys_socket"), "_sys_socket('accept')");
        auto client = std::make_shared<Socket>();
#if defined(_WIN32)
        client->fd = ::accept(server->fd, nullptr, nullptr);
#else
        client->fd = ::accept(server->fd, nullptr, nullptr);
#endif
        if (!client->open()) return bad("accept failed (" + socketError() + ")");
        return Value::integer(storeSocket(client));
    }
    if (op == "send") {
        auto s = socketById(vm, asInt(vm, argAt(a, 1), "_sys_socket"), "_sys_socket('send')");
        std::string data = vm.toStr(argAt(a, 2));
#if defined(_WIN32)
        int n = ::send(s->fd, data.data(), (int)data.size(), 0);
#else
        ssize_t n = ::send(s->fd, data.data(), data.size(), 0);
#endif
        if (n < 0) return bad("send failed (" + socketError() + ")");
        return Value::integer((int64_t)n);
    }
    if (op == "recv") {
        auto s = socketById(vm, asInt(vm, argAt(a, 1), "_sys_socket"), "_sys_socket('recv')");
        size_t want = (size_t)asInt(vm, argAt(a, 2), "_sys_socket", 4096);
        std::vector<char> buf(std::max<size_t>(1, std::min<size_t>(want, 1u << 20)));
#if defined(_WIN32)
        int n = ::recv(s->fd, buf.data(), (int)buf.size(), 0);
#else
        ssize_t n = ::recv(s->fd, buf.data(), buf.size(), 0);
#endif
        if (n <= 0) return Value::str("");          // closed, reset or timeout
        return Value::str(std::string(buf.data(), (size_t)n));
    }
    if (op == "close") {
        int64_t id = asInt(vm, argAt(a, 1), "_sys_socket");
        std::lock_guard<std::mutex> lock(socketMutex());
        auto it = sockets().find(id);
        if (it != sockets().end()) { it->second->close(); sockets().erase(it); }
        return Value::boolean(true);
    }
    if (op == "shutdown") {
        auto s = socketById(vm, asInt(vm, argAt(a, 1), "_sys_socket"), "_sys_socket('shutdown')");
#if defined(_WIN32)
        ::shutdown(s->fd, SD_BOTH);
#else
        ::shutdown(s->fd, SHUT_RDWR);
#endif
        return Value::boolean(true);
    }
    if (op == "timeout") {
        auto s = socketById(vm, asInt(vm, argAt(a, 1), "_sys_socket"), "_sys_socket('timeout')");
        int64_t ms = asInt(vm, argAt(a, 2), "_sys_socket");
#if defined(_WIN32)
        DWORD tv = (DWORD)ms;
        ::setsockopt(s->fd, SOL_SOCKET, SO_RCVTIMEO, (const char*)&tv, sizeof(tv));
#else
        timeval tv;
        tv.tv_sec = (time_t)(ms / 1000);
        tv.tv_usec = (suseconds_t)((ms % 1000) * 1000);
        ::setsockopt(s->fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
#endif
        return Value::boolean(true);
    }
    return bad("unknown op");
}

} // namespace

// ================================================================ registration
void registerSysPrimitives(VM& vm) {
    auto reg = [&](const std::string& name, std::function<Value(VM&, std::vector<Value>&)> fn) {
        vm.globals[name] = std::make_shared<Value>(vm.makeNative(name, std::move(fn)));
    };

    // ------------------------------------------------------------ clock
    reg("_sys_clock", [](VM&, std::vector<Value>&) { return Value::integer(steadyMillis()); });
    reg("_sys_time", [](VM&, std::vector<Value>&) { return Value::integer(wallMillis()); });
    reg("_sys_sleep", [](VM& v, std::vector<Value>& a) {
        int64_t ms = asInt(v, argAt(a, 0), "_sys_sleep");
        if (ms > 0) std::this_thread::sleep_for(std::chrono::milliseconds(ms));
        return Value::null();
    });
    reg("_sys_local_time", [](VM& v, std::vector<Value>& a) {
        std::tm tm = localTm(a.empty() ? wallMillis() : asInt(v, argAt(a, 0), "_sys_local_time"));
        std::vector<Value> out = {Value::integer(tm.tm_year + 1900), Value::integer(tm.tm_mon + 1),
                                  Value::integer(tm.tm_mday), Value::integer(tm.tm_hour),
                                  Value::integer(tm.tm_min), Value::integer(tm.tm_sec),
                                  Value::integer(tm.tm_wday)};
        return Value::list(out);
    });
    reg("_sys_make_time", [](VM& v, std::vector<Value>& a) {
        std::tm tm{};
        tm.tm_year = (int)asInt(v, argAt(a, 0), "_sys_make_time") - 1900;
        tm.tm_mon = (int)asInt(v, argAt(a, 1), "_sys_make_time") - 1;
        tm.tm_mday = (int)asInt(v, argAt(a, 2), "_sys_make_time");
        tm.tm_hour = (int)asInt(v, argAt(a, 3), "_sys_make_time", 0);
        tm.tm_min = (int)asInt(v, argAt(a, 4), "_sys_make_time", 0);
        tm.tm_sec = (int)asInt(v, argAt(a, 5), "_sys_make_time", 0);
        std::time_t t = std::mktime(&tm);
        return Value::integer((int64_t)t * 1000);
    });

    // ------------------------------------------------------------ process and environment
    reg("_sys_info", [](VM& v, std::vector<Value>& a) {
        std::string key = asStr(v, argAt(a, 0), "_sys_info");
        if (key == "platform") return Value::str(platformName());
        if (key == "arch") return Value::str(archName());
        if (key == "cpus") {
            unsigned n = std::thread::hardware_concurrency();
            return Value::integer(n ? (int64_t)n : 1);
        }
        if (key == "pid") {
#if defined(_WIN32)
            return Value::integer((int64_t)GetCurrentProcessId());
#else
            return Value::integer((int64_t)getpid());
#endif
        }
        if (key == "home") {
            const char* h = std::getenv(platformName() == "windows" ? "USERPROFILE" : "HOME");
            return h ? Value::str(h) : Value::null();
        }
        if (key == "temp") {
#if defined(_WIN32)
            char buf[MAX_PATH];
            DWORD n = GetTempPathA(MAX_PATH, buf);
            return Value::str(n ? std::string(buf, n) : std::string(".\\"));
#else
            const char* t = std::getenv("TMPDIR");
            return Value::str(t ? t : "/tmp");
#endif
        }
        if (key == "cwd") {
            std::error_code ec;
            std::string p = std::filesystem::current_path(ec).string();
            return Value::str(ec ? std::string(".") : p);
        }
        v.throwError("_sys_info: unknown key '" + key +
                     "' (platform, arch, cpus, pid, home, temp, cwd)");
    });
    reg("_sys_env", [](VM& v, std::vector<Value>& a) {
        std::string name = asStr(v, argAt(a, 0), "_sys_env");
        const char* val = std::getenv(name.c_str());
        return val ? Value::str(val) : Value::null();
    });
    reg("_sys_env_set", [](VM& v, std::vector<Value>& a) {
        std::string name = asStr(v, argAt(a, 0), "_sys_env_set");
        std::string value = asStr(v, argAt(a, 1), "_sys_env_set");
#if defined(_WIN32)
        _putenv_s(name.c_str(), value.c_str());
#else
        setenv(name.c_str(), value.c_str(), 1);
#endif
        return Value::boolean(true);
    });
    reg("_sys_env_all", [](VM&, std::vector<Value>&) {
        std::vector<Value> out;
#if defined(_WIN32)
        LPCH block = GetEnvironmentStringsA();
        if (block) {
            for (LPCH p = block; *p;) {
                std::string entry = p;
                p += entry.size() + 1;
                size_t eq = entry.find('=');
                if (eq == std::string::npos || eq == 0) continue;
                out.push_back(Value::list({Value::str(entry.substr(0, eq)), Value::str(entry.substr(eq + 1))}));
            }
            FreeEnvironmentStringsA(block);
        }
#else
        for (char** p = ::environ; p && *p; p++) {
            std::string entry = *p;
            size_t eq = entry.find('=');
            if (eq == std::string::npos) continue;
            out.push_back(Value::list({Value::str(entry.substr(0, eq)), Value::str(entry.substr(eq + 1))}));
        }
#endif
        return Value::list(out);
    });
    reg("_sys_exec", [](VM& v, std::vector<Value>& a) {
        // run through the shell, capturing stdout+stderr: the C `system()` analogue
        std::string cmd = asStr(v, argAt(a, 0), "_sys_exec");
        static std::atomic<int> counter{0};
        std::string tmp = "_sys_exec_" + formatInt((long long)std::rand()) + "_" +
                          formatInt(counter++) + ".tmp";
        std::string full = cmd + " > \"" + tmp + "\" 2>&1";
        int code = std::system(full.c_str());
        std::string out;
        {
            std::ifstream in(tmp, std::ios::binary);
            std::stringstream ss;
            ss << in.rdbuf();
            out = ss.str();
        }
        std::remove(tmp.c_str());
        return Value::list({Value::integer(code), Value::str(out)});
    });

    // ------------------------------------------------------------ threads
    reg("_sys_spawn", [](VM& v, std::vector<Value>& a) {
        Value fn = argAt(a, 0);
        if (!fn.isCallable()) v.throwError("_sys_spawn expects a function");
        std::vector<Value> args;
        Value list = argAt(a, 1);
        if (list.t == VT::List || list.t == VT::Tuple) args = list.o->items;
        else if (!list.isNull()) args.push_back(list);

        auto task = std::make_shared<Task>();
        VM* parent = &v;
        task->worker = std::thread([parent, fn, args, task]() mutable {
            task->result = runOnWorker(*parent, fn, args, task->error);
            task->done = true;
        });
        int64_t id = nextTask()++;
        {
            std::lock_guard<std::mutex> lock(taskMutex());
            tasks()[id] = task;
        }
        return Value::integer(id);
    });
    reg("_sys_task_done", [](VM& v, std::vector<Value>& a) {
        int64_t id = asInt(v, argAt(a, 0), "_sys_task_done");
        std::lock_guard<std::mutex> lock(taskMutex());
        auto it = tasks().find(id);
        return Value::boolean(it != tasks().end() && it->second->done.load());
    });
    reg("_sys_join", [](VM& v, std::vector<Value>& a) {
        int64_t id = asInt(v, argAt(a, 0), "_sys_join");
        std::shared_ptr<Task> task;
        {
            std::lock_guard<std::mutex> lock(taskMutex());
            auto it = tasks().find(id);
            if (it == tasks().end()) v.throwError("_sys_join: unknown task " + formatInt(id));
            task = it->second;
        }
        if (task->worker.joinable()) task->worker.join();
        if (!task->error.empty()) v.throwError("worker: " + task->error);
        return task->result;
    });

    // ------------------------------------------------------------ sockets
    reg("_sys_socket", socketOp);

    // ------------------------------------------------------------ the small `time` module
    // Clock access stays available directly, without `use time` (millis is asked for often).
    Value time = Value::module("time");
    time.o->map["now"] = vm.makeNative("time.now", [](VM&, std::vector<Value>&) {
        return Value::integer(wallMillis() / 1000);
    });
    time.o->map["millis"] = vm.makeNative("time.millis", [](VM&, std::vector<Value>&) {
        return Value::integer(wallMillis());
    });
    time.o->map["clock"] = vm.makeNative("time.clock", [](VM&, std::vector<Value>&) {
        return Value::integer(steadyMillis());
    });
    time.o->map["ticks"] = time.o->map["clock"];
    time.o->map["sleep"] = vm.makeNative("time.sleep", [](VM& v, std::vector<Value>& a) {
        int64_t ms = asInt(v, argAt(a, 0), "time.sleep");
        if (ms > 0) std::this_thread::sleep_for(std::chrono::milliseconds(ms));
        return Value::null();
    });
    vm.globals["time"] = std::make_shared<Value>(time);

    // ------------------------------------------------------------ the `system` facade
    // Same primitives, reachable by name, so new library code can dispatch dynamically.
    Value system = Value::module("system");
    struct Bind {
        const char* name;
        const char* prim;
        const char* summary;
    };
    static const Bind binds[] = {
        {"clock", "_sys_clock", "单调毫秒，用来测耗时"},
        {"time", "_sys_time", "墙上毫秒时间戳"},
        {"sleep", "_sys_sleep", "休眠若干毫秒"},
        {"local_time", "_sys_local_time", "[年,月,日,时,分,秒,星期]"},
        {"make_time", "_sys_make_time", "本地时间转毫秒时间戳"},
        {"info", "_sys_info", "platform / arch / cpus / pid / home / temp / cwd"},
        {"env", "_sys_env", "读环境变量"},
        {"set_env", "_sys_env_set", "写环境变量"},
        {"env_all", "_sys_env_all", "全部环境变量"},
        {"exec", "_sys_exec", "[退出码, 输出]，经 shell 执行"},
        {"spawn", "_sys_spawn", "在工作线程上运行函数，返回任务号"},
        {"join", "_sys_join", "等待任务并取回结果"},
        {"task_done", "_sys_task_done", "任务是否结束"},
        {"socket", "_sys_socket", "resolve/connect/listen/accept/send/recv/close/shutdown/timeout"},
    };
    for (auto& b : binds)
        if (auto it = vm.globals.find(b.prim); it != vm.globals.end() && it->second)
            system.o->map[b.name] = *it->second;
    system.o->map["primitives"] = vm.makeNative("system.primitives", [](VM&, std::vector<Value>&) {
        std::vector<Value> out;
        for (auto& b : binds) out.push_back(Value::str(b.prim));
        return Value::list(out);
    });
    system.o->map["help"] = vm.makeNative("system.help", [](VM& v, std::vector<Value>& a) {
        std::string query = a.empty() ? std::string() : asStr(v, argAt(a, 0), "system.help");
        std::string out = "底层原语（上层封装见 lib/*.mod：time / os / thread / net）\n";
        for (auto& b : binds) {
            if (!query.empty() && query != b.name && query != b.prim) continue;
            out += "  system." + std::string(b.name) + "  /  " + b.prim + "  -  " + b.summary + "\n";
        }
        return Value::str(out);
    });
    system.o->map["call"] = vm.makeNative("system.call", [](VM& v, std::vector<Value>& a) {
        std::string name = asStr(v, argAt(a, 0), "system.call");
        for (auto& b : binds) {
            if (name != b.name && name != b.prim) continue;
            Value fn;
            if (!v.getGlobal(b.prim, fn) || !fn.isCallable())
                v.throwError("system.call: primitive " + std::string(b.prim) + " is missing");
            std::vector<Value> args(a.begin() + 1, a.end());
            return v.callSync(fn, std::move(args));
        }
        v.throwError("system.call: unknown interface '" + name + "' (see system.primitives())");
    });
    system.o->map["has"] = vm.makeNative("system.has", [](VM& v, std::vector<Value>& a) {
        std::string name = asStr(v, argAt(a, 0), "system.has");
        for (auto& b : binds)
            if (name == b.name || name == b.prim) return Value::boolean(true);
        return Value::boolean(false);
    });
    vm.globals["system"] = std::make_shared<Value>(system);
}

// ================================================================ documentation
std::string sysPrimitivesMarkdown() {
    std::string out;
    out += "## 4. Primitive ABI\n\n";
    out += "C++ exposes only thunks over the operating system; every convenience API lives in\n";
    out += "`lib/*.mod` and is written in Annota, so new features usually mean a new module rather\n";
    out += "than a rebuild. The same primitives are reachable through the `system` facade\n";
    out += "(`system.clock()`, `system.call(\"clock\")`, `system.primitives()`).\n\n";
    out += "| Primitive | Returns | Meaning |\n|---|---|---|\n";
    out += "| `_sys_clock()` | int | monotonic milliseconds (measure elapsed time) |\n";
    out += "| `_sys_time()` | int | wall clock milliseconds since the epoch |\n";
    out += "| `_sys_sleep(ms)` | null | sleep |\n";
    out += "| `_sys_local_time([ms])` | List | `[year, month, day, hour, minute, second, weekday]` |\n";
    out += "| `_sys_make_time(y, mo, d[, h, mi, s])` | int | local time to a timestamp (ms) |\n";
    out += "| `_sys_info(key)` | String/int | `platform`, `arch`, `cpus`, `pid`, `home`, `temp`, `cwd` |\n";
    out += "| `_sys_env(name)` | String/null | environment variable |\n";
    out += "| `_sys_env_set(name, value)` | bool | set for this process and its children |\n";
    out += "| `_sys_env_all()` | List | `[[name, value], ...]` |\n";
    out += "| `_sys_exec(cmd)` | List | `[exitCode, output]`, run through the shell |\n";
    out += "| `_sys_spawn(fn[, args])` | int | run `fn(...)` on a worker thread, returns a task handle |\n";
    out += "| `_sys_task_done(handle)` | bool | has the worker finished? |\n";
    out += "| `_sys_join(handle)` | value | wait and return the result (rethrows worker errors) |\n";
    out += "| `_sys_socket(op, ...)` | varies | `resolve`, `connect`, `listen`, `accept`, `send`, `recv`, `close`, `shutdown`, `timeout` |\n\n";
    out += "Built on top of them: `lib/time.mod`, `lib/os.mod`, `lib/thread.mod`, `lib/net.mod`.\n";
    out += "The clock is also available directly as `time.now()` (seconds), `time.millis()` (wall\n";
    out += "milliseconds), `time.clock()` (monotonic milliseconds) and `time.sleep(ms)`.\n\n";
    return out;
}

} // namespace annota
