// 与 examples/perf.ant 完全相同的负载，用 C++ 跑一遍，-O2，作为同机基准
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <functional>
#include <queue>
#include <string>
#include <unordered_map>
#include <cstdlib>
#include <vector>

static volatile long long sink = 0;

static double now_ms() {
    using namespace std::chrono;
    return duration<double, std::milli>(steady_clock::now().time_since_epoch()).count();
}

static void row(const char* name, long long ops, double ms) {
    double ns = ops > 0 ? ms * 1e6 / (double)ops : 0.0;
    std::printf("%-26s %10lld %12.3f %14.2f\n", name, ops, ms, ns);
}

static long long count_up(long long n) {          // 与 perf.ant 的 count_up 相同
    long long i = 0, acc = 0;
    while (i < n) { acc += i; i += 1; }
    return acc;
}
static long long add_one(long long x) { return x + 1; }

static std::vector<int> make_data(int n) {        // 同一个 LCG
    std::vector<int> xs;
    unsigned rng = 7;
    for (int i = 0; i < n; i++) {
        rng = rng * 1103515245u + 12345u;
        xs.push_back((int)(rng % 2147483648u % 100000u));
    }
    return xs;
}

static std::string haystack(int n) {
    std::string s;
    for (int i = 0; i < n; i++) s += (char)(97 + i % 26);
    return s;
}

static int kmp_count(const std::string& s, const std::string& pat);

static int levenshtein(const std::string& a, const std::string& b) {
    int n = (int)a.size(), m = (int)b.size();
    std::vector<int> prev(m + 1), cur(m + 1);
    for (int j = 0; j <= m; j++) prev[j] = j;
    for (int i = 1; i <= n; i++) {
        cur[0] = i;
        for (int j = 1; j <= m; j++) {
            int cost = a[i - 1] == b[j - 1] ? 0 : 1;
            cur[j] = std::min(std::min(prev[j] + 1, cur[j - 1] + 1), prev[j - 1] + cost);
        }
        prev = cur;
    }
    return prev[m];
}

int main() {
    std::printf("%-26s %10s %12s %14s\n", "C++ 负载", "次数", "总耗时 ms", "每次 ns");
    double t0;
    // 运行期上界：避免编译器把循环整个常量折叠掉
    long long loop_n = std::atoll(std::getenv("LOOP_N") ? std::getenv("LOOP_N") : "400000");
    long long call_n = loop_n / 10;

    t0 = now_ms(); sink += count_up(loop_n); row("int 循环 (while)", loop_n, now_ms() - t0);

    t0 = now_ms();
    { long long (*volatile fn)(long long) = &add_one;   // volatile 指针：强制真实调用
      long long acc = 0; for (long long i = 0; i < call_n; i++) acc = fn(acc); sink += acc; }
    row("函数调用", call_n, now_ms() - t0);

    t0 = now_ms();
    {
        std::vector<long long> a(60000);
        for (int i = 0; i < 60000; i++) a[i] = i * 2;
        long long s = 0;
        for (int i = 0; i < 60000; i++) s += a[i];
        sink += s;
    }
    row("定长数组 int[n]", 60000, now_ms() - t0);

    t0 = now_ms();
    {
        std::vector<long long> xs;
        for (int i = 0; i < 60000; i++) xs.push_back(i * 2);
        long long s = 0;
        for (auto x : xs) s += x;
        sink += s;
    }
    row("动态列表 push", 60000, now_ms() - t0);

    auto data = make_data(2000);
    t0 = now_ms();
    { auto d = data; std::sort(d.begin(), d.end()); }
    row("std::sort (原生)", 2000, now_ms() - t0);

    t0 = now_ms();
    { auto d = data; std::stable_sort(d.begin(), d.end()); }
    row("std::stable_sort (归并)", 2000, now_ms() - t0);

    auto sorted_data = make_data(4000);
    std::sort(sorted_data.begin(), sorted_data.end());
    t0 = now_ms();
    {
        long long acc = 0;
        for (int i = 0; i < 4000; i++)
            acc += (long long)(std::lower_bound(sorted_data.begin(), sorted_data.end(), sorted_data[i]) - sorted_data.begin());
        sink += acc;
    }
    row("二分查找 (4000 元素)", 4000, now_ms() - t0);

    std::unordered_map<std::string, int> table;
    t0 = now_ms();
    for (int i = 0; i < 4000; i++) table["key-" + std::to_string(i)] = i;
    row("哈希插入 (4000 键)", 4000, now_ms() - t0);

    t0 = now_ms();
    {
        long long hits = 0;
        for (int i = 0; i < 4000; i++)
            if (table.count("key-" + std::to_string(sorted_data[i])) > 0) hits++;
        sink += hits;
    }
    row("哈希查找 (4000 键)", 4000, now_ms() - t0);

    t0 = now_ms();
    { auto d = data; for (int t = 0; t < 6; t++) std::sort(d.begin(), d.end()); }
    row("中位数 (排序法 x6)", 2000 * 6, now_ms() - t0);

    t0 = now_ms();
    {
        for (int t = 0; t < 6; t++) {
            auto d = data;
            std::nth_element(d.begin(), d.begin() + d.size() / 2, d.end());
            sink += d[d.size() / 2];
        }
    }
    row("中位数 (nth_element x6)", 2000 * 6, now_ms() - t0);

    t0 = now_ms();
    {
        std::vector<bool> flags(30001, true);
        flags[0] = flags[1] = false;
        for (int p = 2; (long long)p * p <= 30000; p++)
            if (flags[p])
                for (int m = p * p; m <= 30000; m += p) flags[m] = false;
        int c = 0;
        for (int i = 2; i <= 30000; i++) if (flags[i]) c++;
        sink += c;
    }
    row("筛法求素数", 30000, now_ms() - t0);

    auto hay = haystack(2000);
    t0 = now_ms();
    for (int i = 0; i < 200; i++) sink += kmp_count(hay, "abcabc");
    row("KMP 搜索 (2000 字符)", 200, now_ms() - t0);

    auto w1 = haystack(80), w2 = haystack(80);
    t0 = now_ms();
    for (int i = 0; i < 40; i++) sink += levenshtein(w1, w2);
    row("编辑距离 (80x80)", 80 * 80, now_ms() - t0);

    t0 = now_ms();
    {
        const int N = 30;
        std::vector<double> m(N * N, 0), k(N * N, 0), r(N * N, 0);
        for (int i = 0; i < N; i++) m[i * N + i] = 1, k[i * N + i] = 1;
        for (int t = 0; t < 3; t++) {
            std::fill(r.begin(), r.end(), 0.0);
            for (int i = 0; i < N; i++)
                for (int p = 0; p < N; p++) {
                    double a = m[i * N + p];
                    if (a == 0) continue;
                    for (int j = 0; j < N; j++) r[i * N + j] += a * k[p * N + j];
                }
            m = r;
        }
        for (int i = 0; i < N; i++) sink += (long long)m[i * N + i];
    }
    row("矩阵乘 (30x30)", 30 * 30 * 30 * 3, now_ms() - t0);

    // 网格 Dijkstra（14x14，与 perf.ant 同规模）
    t0 = now_ms();
    {
        const int S = 14, V = S * S, INF = 1 << 30;
        std::vector<std::vector<std::pair<int, int>>> adj(V);
        auto id = [&](int r, int c) { return r * S + c; };
        for (int r = 0; r < S; r++)
            for (int c = 0; c < S; c++) {
                if (r + 1 < S) { int w = 1 + (r * 7 + c) % 5; adj[id(r, c)].push_back({id(r + 1, c), w}); }
                if (c + 1 < S) { int w = 1 + (r * 3 + c) % 5; adj[id(r, c)].push_back({id(r, c + 1), w}); }
            }
        std::vector<int> dist(V, INF);
        std::priority_queue<std::pair<int, int>, std::vector<std::pair<int, int>>, std::greater<>> pq;
        dist[0] = 0;
        pq.push({0, 0});
        while (!pq.empty()) {
            auto [d, v] = pq.top();
            pq.pop();
            if (d != dist[v]) continue;
            for (auto [u, w] : adj[v])
                if (d + w < dist[u]) { dist[u] = d + w; pq.push({dist[u], u}); }
        }
        sink += dist[V - 1];
    }
    row("Dijkstra (14x14 网格)", 196, now_ms() - t0);
    std::printf("(checksum %lld)\n", (long long)sink);
    return 0;
}

static int kmp_count(const std::string& s, const std::string& pat) {
    int m = (int)pat.size();
    if (m == 0) return 0;
    std::vector<int> fail(m, 0);
    for (int i = 1, k = 0; i < m;) {
        while (k > 0 && pat[i] != pat[k]) k = fail[k - 1];
        if (pat[i] == pat[k]) k++;
        fail[i] = k;
        i++;
    }
    int cnt = 0;
    for (int i = 0, k = 0; i < (int)s.size(); i++) {
        while (k > 0 && s[i] != pat[k]) k = fail[k - 1];
        if (s[i] == pat[k]) k++;
        if (k == m) { cnt++; k = fail[k - 1]; }
    }
    return cnt;
}
