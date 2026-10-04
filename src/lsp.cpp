// Annota - lsp.cpp : a small Language Server Protocol front end over stdio.
//
//   didOpen / didChange   -> layer 1 diagnostics immediately  (keystroke budget)
//   didSave               -> layer 2 diagnostics, then layer 3 in a cancellable background job
//   hover / definition / rename / completion -> served from the same analysis layer
#include "commands.hpp"
#include "analyzer.hpp"
#include "json.hpp"
#include "common.hpp"
#include <atomic>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iostream>
#include <map>
#include <memory>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#if defined(_WIN32)
#include <fcntl.h>
#include <io.h>
#endif

namespace annota {
namespace {

std::mutex g_outMutex;

void sendMessage(const Json& msg) {
    std::string body = jsonDump(msg);
    std::lock_guard<std::mutex> lock(g_outMutex);
    std::fwrite("Content-Length: ", 1, 16, stdout);
    std::string len = formatInt((long long)body.size());
    std::fwrite(len.data(), 1, len.size(), stdout);
    std::fwrite("\r\n\r\n", 1, 4, stdout);
    std::fwrite(body.data(), 1, body.size(), stdout);
    std::fflush(stdout);
}

void sendResult(const Json& id, Json result) {
    Json m = Json::object();
    m.set("jsonrpc", Json::string("2.0"));
    m.set("id", id);
    m.set("result", std::move(result));
    sendMessage(m);
}

void sendError(const Json& id, int code, const std::string& message) {
    Json err = Json::object();
    err.set("code", Json::integer(code));
    err.set("message", Json::string(message));
    Json m = Json::object();
    m.set("jsonrpc", Json::string("2.0"));
    m.set("id", id);
    m.set("error", err);
    sendMessage(m);
}

std::string uriToPath(const std::string& uri) {
    std::string p = uri;
    if (p.rfind("file:///", 0) == 0) p = p.substr(8);
    else if (p.rfind("file://", 0) == 0) p = p.substr(7);
    std::string out;
    for (size_t i = 0; i < p.size(); i++) {
        if (p[i] == '%' && i + 2 < p.size()) {
            auto hex = [](char c) { return c >= '0' && c <= '9' ? c - '0' : (c | 32) - 'a' + 10; };
            out += (char)(hex(p[i + 1]) * 16 + hex(p[i + 2]));
            i += 2;
        } else out += p[i];
    }
    return out;
}

std::string pathToUri(const std::string& path) {
    std::string p = path;
    for (auto& c : p) if (c == '\\') c = '/';
    if (p.size() > 1 && p[1] == ':') return "file:///" + p;
    return "file://" + p;
}

Json makeRange(int line, int col, int endCol) {
    Json r = Json::object();
    Json s = Json::object();
    s.set("line", Json::integer(line - 1 < 0 ? 0 : line - 1));
    s.set("character", Json::integer(col - 1 < 0 ? 0 : col - 1));
    Json e = Json::object();
    e.set("line", Json::integer(line - 1 < 0 ? 0 : line - 1));
    e.set("character", Json::integer(endCol - 1 < 0 ? 0 : endCol - 1));
    r.set("start", s);
    r.set("end", e);
    return r;
}

int lspSeverity(Severity s) {
    switch (s) {
        case Severity::Error: return 1;
        case Severity::Warning: return 2;
        default: return 3;
    }
}

struct Document {
    std::string uri;
    std::string path;
    std::string text;
    int version = 0;
    std::atomic<int> generation{0};
    std::shared_ptr<std::atomic<bool>> cancel = std::make_shared<std::atomic<bool>>(false);
    std::thread worker;
};

class Server {
public:
    Server() { docs_ = &docsStatic(); }
    int run();

private:
    std::map<std::string, Document>* docs_;
    bool shutdown_ = false;
    bool exitRequested_ = false;

    static std::map<std::string, Document>& docsStatic() {
        static std::map<std::string, Document> d;
        return d;
    }

    void publish(Document& doc, const AnalysisResult& res, int generation);
    void analyzeAndPublish(Document& doc, int level, bool background);
    Json handle(const Json& msg);
    Json hover(const Document& doc, int line, int col);
    Json definition(const Document& doc, int line, int col);
    Json rename(const Document& doc, int line, int col, const std::string& newName);
    Json completion(const Document& doc, int line, int col);
};

void Server::publish(Document& doc, const AnalysisResult& res, int generation) {
    if (generation != doc.generation.load()) return;         // superseded by a newer edit
    Json params = Json::object();
    params.set("uri", Json::string(doc.uri));
    params.set("version", Json::integer(doc.version));
    Json arr = Json::array();
    for (auto& d : res.diagnostics) {
        if (d.suppressed) continue;
        Json j = Json::object();
        j.set("range", makeRange(d.line, d.col, d.endCol > d.col ? d.endCol : d.col + 1));
        j.set("severity", Json::integer(lspSeverity(d.severity)));
        j.set("code", Json::string(d.code));
        j.set("source", Json::string("annota"));
        std::string msg = d.message;
        if (!d.detail.empty()) msg += "\n" + d.detail;
        if (!d.fix.empty()) msg += "\n修复: " + d.fix;
        j.set("message", Json::string(msg));
        arr.push(j);
    }
    params.set("diagnostics", arr);
    Json note = Json::object();
    note.set("jsonrpc", Json::string("2.0"));
    note.set("method", Json::string("textDocument/publishDiagnostics"));
    note.set("params", params);
    sendMessage(note);
}

void Server::analyzeAndPublish(Document& doc, int level, bool background) {
    if (background) {
        // layer 3 runs on its own thread; a newer edit cancels it cooperatively
        if (doc.worker.joinable()) {
            doc.cancel->store(true);
            doc.worker.join();
        }
        doc.cancel = std::make_shared<std::atomic<bool>>(false);
        int gen = doc.generation.load();
        std::string text = doc.text;
        std::string path = doc.path;
        std::string uri = doc.uri;
        int version = doc.version;
        auto cancelFlag = doc.cancel;
        Server* self = this;
        Document* target = &doc;
        doc.worker = std::thread([self, target, text, path, uri, version, gen, cancelFlag]() {
            AnalysisOptions opts;
            opts.level = 3;
            opts.budgetMs = 5000;            // the background layer never runs longer than this
            opts.isCancelled = [cancelFlag]() { return cancelFlag->load(); };
            AnalysisResult res = analyzeSource(text, path, opts);
            if (cancelFlag->load()) return;
            if (target->generation.load() != gen) return;
            target->version = version;
            self->publish(*target, res, gen);
        });
        return;
    }
    AnalysisOptions opts;
    opts.level = level;
    AnalysisResult res = analyzeSource(doc.text, doc.path, opts);
    publish(doc, res, doc.generation.load());
}

Json Server::hover(const Document& doc, int line, int col) {
    AnalysisOptions opts;
    opts.level = 2;
    HoverInfo h = hoverAt(doc.text, doc.path, line, col, opts);
    if (!h.valid) return Json::null();
    Json contents = Json::object();
    contents.set("kind", Json::string("markdown"));
    contents.set("value", Json::string("**" + h.title + "**\n\n" + h.body));
    Json r = Json::object();
    r.set("contents", contents);
    r.set("range", makeRange(line, col, col + 1));
    return r;
}

Json Server::definition(const Document& doc, int line, int col) {
    AnalysisOptions opts;
    opts.level = 2;
    Location l = definitionAt(doc.text, doc.path, line, col, opts);
    if (!l.valid) return Json::null();
    Json r = Json::object();
    r.set("uri", Json::string(l.file.rfind("annota://", 0) == 0 ? l.file : pathToUri(l.file)));
    r.set("range", makeRange(l.line, l.col, l.endCol));
    return r;
}

Json Server::rename(const Document& doc, int line, int col, const std::string& newName) {
    AnalysisOptions opts;
    opts.level = 1;
    RenameResult r = renameAt(doc.text, doc.path, line, col, newName, opts);
    if (!r.ok) return Json::null();
    Json edits = Json::array();
    for (auto& e : r.edits) {
        Json je = Json::object();
        je.set("range", makeRange(e.line, e.col, e.endCol));
        je.set("newText", Json::string(e.newText));
        edits.push(je);
    }
    Json changes = Json::object();
    changes.set(pathToUri(doc.path), edits);
    Json out = Json::object();
    out.set("changes", changes);
    return out;
}

Json Server::completion(const Document& doc, int line, int col) {
    // look at the text just before the cursor to decide what to offer
    std::istringstream in(doc.text);
    std::string l;
    int n = 1;
    std::string current;
    while (std::getline(in, l) && n <= line) { current = l; n++; }
    std::string prefix;
    if (col - 1 <= (int)current.size()) prefix = current.substr(0, (size_t)std::max(0, col - 1));
    bool afterBracket = prefix.size() >= 2 && prefix.compare(prefix.size() - 2, 2, "[[") == 0;
    std::string word;
    for (size_t i = prefix.size(); i-- > 0;) {
        char c = prefix[i];
        if (isalnum((unsigned char)c) || c == '_' || c == '[') word = std::string(1, c) + word;
        else break;
    }
    if (!afterBracket && !word.empty() && word[0] == '[') afterBracket = true;
    Json items = Json::array();
    for (auto& c : completionsFor(afterBracket ? "" : word, afterBracket)) {
        Json j = Json::object();
        j.set("label", Json::string(c.label));
        j.set("kind", Json::integer(c.kind == "annotation" ? 6 : 14));
        j.set("detail", Json::string(c.detail));
        j.set("insertText", Json::string(c.insertText));
        items.push(j);
    }
    Json out = Json::object();
    out.set("isIncomplete", Json::boolean(false));
    out.set("items", items);
    return out;
}

Json Server::handle(const Json& msg) {
    std::string method = msg.s("method");
    const Json* id = msg.find("id");
    const Json* params = msg.find("params");
    Json idv = id ? *id : Json::null();

    if (method == "initialize") {
        Json caps = Json::object();
        Json sync = Json::object();
        sync.set("openClose", Json::boolean(true));
        sync.set("change", Json::integer(1));            // full text sync
        Json save = Json::object();
        save.set("includeText", Json::boolean(false));
        sync.set("save", save);
        caps.set("textDocumentSync", sync);
        caps.set("hoverProvider", Json::boolean(true));
        caps.set("definitionProvider", Json::boolean(true));
        caps.set("renameProvider", Json::boolean(true));
        Json comp = Json::object();
        Json triggers = Json::array();
        triggers.push(Json::string("["));
        triggers.push(Json::string(":"));
        comp.set("triggerCharacters", triggers);
        comp.set("resolveProvider", Json::boolean(false));
        caps.set("completionProvider", comp);
        Json serverInfo = Json::object();
        serverInfo.set("name", Json::string("annota-lsp"));
        serverInfo.set("version", Json::string("1.0"));
        Json result = Json::object();
        result.set("capabilities", caps);
        result.set("serverInfo", serverInfo);
        sendResult(idv, result);
        return Json::null();
    }
    if (method == "initialized" || method == "$/setTrace") return Json::null();
    if (method == "shutdown") {
        shutdown_ = true;
        sendResult(idv, Json::null());
        return Json::null();
    }
    if (method == "exit") {
        shutdown_ = true;
        exitRequested_ = true;
        return Json::null();
    }
    if (method == "textDocument/didOpen" && params) {
        const Json* td = params->find("textDocument");
        if (!td) return Json::null();
        std::string uri = td->s("uri");
        Document& doc = (*docs_)[uri];
        doc.uri = uri;
        doc.path = uriToPath(uri);
        doc.text = td->s("text");
        doc.version = (int)td->i("version", 1);
        doc.generation++;
        AnalysisOptions opts;
        opts.level = 1;
        AnalysisResult res = analyzeSource(doc.text, doc.path, opts);
        publish(doc, res, doc.generation.load());
        return Json::null();
    }
    if (method == "textDocument/didChange" && params) {
        const Json* td = params->find("textDocument");
        if (!td) return Json::null();
        std::string uri = td->s("uri");
        auto it = docs_->find(uri);
        if (it == docs_->end()) return Json::null();
        Document& doc = it->second;
        doc.version = (int)td->i("version", doc.version + 1);
        const Json* changes = params->find("contentChanges");
        if (changes && !changes->arr.empty()) {
            const Json& last = changes->arr.back();
            doc.text = last.s("text");
        }
        doc.generation++;
        doc.cancel->store(true);                        // stop any background job early
        analyzeAndPublish(doc, 1, false);               // keystroke layer: syntax only
        return Json::null();
    }
    if (method == "textDocument/didSave" && params) {
        const Json* td = params->find("textDocument");
        if (!td) return Json::null();
        std::string uri = td->s("uri");
        auto it = docs_->find(uri);
        if (it == docs_->end()) return Json::null();
        Document& doc = it->second;
        doc.generation++;
        analyzeAndPublish(doc, 2, false);               // save layer
        analyzeAndPublish(doc, 3, true);                // background layer
        return Json::null();
    }
    if (method == "textDocument/didClose" && params) {
        const Json* td = params->find("textDocument");
        if (!td) return Json::null();
        std::string uri = td->s("uri");
        auto it = docs_->find(uri);
        if (it != docs_->end()) {
            if (it->second.worker.joinable()) {
                it->second.cancel->store(true);
                it->second.worker.join();
            }
            docs_->erase(it);
        }
        Json empty = Json::array();
        Json p = Json::object();
        p.set("uri", Json::string(uri));
        p.set("diagnostics", empty);
        Json note = Json::object();
        note.set("jsonrpc", Json::string("2.0"));
        note.set("method", Json::string("textDocument/publishDiagnostics"));
        note.set("params", p);
        sendMessage(note);
        return Json::null();
    }
    if (!params) {
        if (id) sendError(idv, -32601, "unknown method " + method);
        return Json::null();
    }
    const Json* td = params->find("textDocument");
    const Json* pos = params->find("position");
    if (!td || !pos) {
        if (id) sendResult(idv, Json::null());
        return Json::null();
    }
    std::string uri = td->s("uri");
    auto it = docs_->find(uri);
    if (it == docs_->end()) {
        if (id) sendResult(idv, Json::null());
        return Json::null();
    }
    Document& doc = it->second;
    int line = (int)pos->i("line", 0) + 1;
    int col = (int)pos->i("character", 0) + 1;

    if (method == "textDocument/hover") { sendResult(idv, hover(doc, line, col)); return Json::null(); }
    if (method == "textDocument/definition") { sendResult(idv, definition(doc, line, col)); return Json::null(); }
    if (method == "textDocument/completion") { sendResult(idv, completion(doc, line, col)); return Json::null(); }
    if (method == "textDocument/rename") {
        std::string newName;
        const Json* np = params->find("newName");
        if (np) newName = np->asString();
        sendResult(idv, rename(doc, line, col, newName));
        return Json::null();
    }
    if (id) sendError(idv, -32601, "unknown method " + method);
    return Json::null();
}

int Server::run() {
#if defined(_WIN32)
    _setmode(_fileno(stdin), _O_BINARY);
    _setmode(_fileno(stdout), _O_BINARY);
#endif
    std::string buffer;
    char buf[8192];
    while (!shutdown_) {
        // read headers
        size_t contentLength = 0;
        bool haveLength = false;
        std::string header;
        while (true) {
            size_t nl = buffer.find("\r\n");
            if (nl == std::string::npos) {
                size_t n = std::fread(buf, 1, sizeof(buf), stdin);
                if (n == 0) return 0;
                buffer.append(buf, n);
                continue;
            }
            header = buffer.substr(0, nl);
            buffer.erase(0, nl + 2);
            if (header.empty()) break;
            std::string lower = header;
            for (auto& c : lower) c = (char)tolower((unsigned char)c);
            size_t p = lower.find("content-length:");
            if (p != std::string::npos) {
                contentLength = (size_t)std::atoll(header.c_str() + p + 15);
                haveLength = true;
            }
        }
        if (!haveLength) continue;
        while (buffer.size() < contentLength) {
            size_t n = std::fread(buf, 1, sizeof(buf), stdin);
            if (n == 0) return 0;
            buffer.append(buf, n);
        }
        std::string body = buffer.substr(0, contentLength);
        buffer.erase(0, contentLength);
        Json msg;
        std::string err;
        if (!jsonParse(body, msg, &err)) {
            std::fprintf(stderr, "annota-lsp: bad json: %s\n", err.c_str());
            continue;
        }
        handle(msg);
        if (exitRequested_) break;
    }
    // let a running background analysis finish (it is bounded by its own budget) so that the
    // client still receives the last publishDiagnostics
    for (auto& kv : *docs_) {
        if (kv.second.worker.joinable()) kv.second.worker.join();
    }
    return 0;
}

} // namespace

int cmdLsp(int, char**) {
    Server s;
    return s.run();
}

} // namespace annota
