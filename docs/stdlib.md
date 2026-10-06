-[ stdlib.md : 标准库参考（模块 / 类 / 函数一览） ]-

# 标准库参考

> **分层**：`Seq.sort` / `kth` / `median` / `dedup` / `sort_by` / `lower_bound` / `upper_bound` / `bsearch`
> 都是薄封装，底下调用的是原生模块 **`seqnative`**（`native/seq_native.cpp`，通过
> [FFI](ffi.md) 注册，不在语言核心内）。这些原语以前是内置函数，现在搬到了链接进来的 C++ 层，
> 所以再往下加算法只需要加一个 `native/*.cpp`，核心代码一行都不用动。
> 脚本实现（`merge_sort`、`kth_script` 等）仍然保留，作为参考实现和自定义比较器时的入口。

语言内核只暴露 `_` 前缀的原语（时钟、文件、线程、套接字……），其余全部是 `lib/` 下用 Annota
自己写的模块。用 `use <名字>` 单独加载，或 `use std` 一次加载全部。

```annota
use seq          -- 只加载序列模块
use std          -- 加载全部（slice / seq / str / dict / mathx / test / file / time / os / thread / net）
use sub/x        -- 子目录里的模块（等价于 `use "sub/x"`，会自动补 .mod）
```

* 搜索路径：主文件所在目录、它的 `lib/`、`lib/`、当前目录；此外还会记住每个被加载模块所在目录，
  所以模块可以 `use` 自己的兄弟模块。
* 找不到模块时会直接报错，并列出搜索过的地方、可用的模块名和一个"是不是想用 xxx"的建议：

```
error: cannot find module 'seqq' (searched build, build/lib, lib, .)
  available modules: dict, geometry, graph, mathx, matrix, ...
  did you mean 'seq'?
```

* 模块顶层用 `new` / `const` 声明的东西是**程序全局**（运行时和静态分析都能看到）；
  模块内部的诊断默认不外报，用 `--modules` 可以让分析器把它们也报出来。

下面按模块列出 API。复杂度写在括号里，标注"稳定"的排序保持相等元素的原有顺序。

---

## seq —— 序列与算法

```annota
use seq
new xs = [5, 3, 9, 1]
Seq.sort(xs)                 -- [1, 3, 5, 9]（稳定归并排序，O(n log n)）
Seq.median(xs)               -- 4（快速选择，平均 O(n)）
Seq.bsearch(Seq.sort(xs), 9)  -- 3（二分，O(log n)）
Heap.from_list(xs).drain()   -- [1, 3, 5, 9]（二叉堆，O(n log n)）
```

### 遍历与变换

| 函数 | 说明 |
|---|---|
| `map(xs, f)` `filter(xs, p)` `reject(xs, p)` | 映射 / 筛选 / 反向筛选 |
| `reduce(xs, f, init)` `fold(xs, init, f)` `scan(xs, f, init)` | 归约、折叠、前缀扫描（长度 +1） |
| `prefix_sums(xs)` | 前缀和 |
| `flat_map(xs, f)` `flatten(xs)` `compact(xs)` | 展平一层 / 去掉 null |
| `any` `all` `none` `count` `count_where` | 量词与计数 |
| `find` `find_last` `index_where` `last_index_where` `index_of` `contains` | 查找 |
| `contains_all` `contains_any` | 集合包含 |
| `reverse` `copy` `repeat(v, n)` `iota(a, b)` `times(n, f)` | 基础构造 |
| `take` `drop` `take_while` `drop_while` `slice_of` `slice_step` | 截取 |
| `chunk(xs, n)` `window(xs, n)` `pairwise(xs)` `runs(xs)` | 分组与窗口 |
| `split_at` `partition` `rotate` `swap` `delete_at` `indices` | 切分与就地操作 |
| `enumerate_of` `zip_with` `zip3` `unzip` | 组合 |
| `join(xs, sep)` `equals(xs, ys)` `first` `last` | 杂项 |
| `shuffle(xs, seed)` `sample(xs, k, seed)` | 可复现的伪随机 |

### 排序与选择

| 函数 | 复杂度 | 说明 |
|---|---|---|
| `merge_sort(xs, less)` | O(n log n) | 稳定，用比较函数 |
| `sort(xs, reverse)` | O(n log n) | 稳定，默认升序 |
| `sort_by(xs, key, reverse)` | O(n log n) | 稳定，按 key（多关键字可自己写 less） |
| `heap_sort(xs, reverse)` | O(n log n) | 原地风格，无递归 |
| `insertion_sort(xs, less)` | O(n²) | 小数组更快 |
| `sort_native(xs)` | C++ `std::sort` | 最快，但不保证稳定 |
| `is_sorted(xs, less)` `insert_sorted(xs, x)` `merge_sorted(a, b, less)` | | 有序性、插入、归并 |
| `kth(xs, k)` | 平均 O(n) | 第 k 小（三数取中，Lomuto 划分） |
| `median(xs)` `median_sorted(xs)` `quantile(xs, q)` | 平均 O(n) | 中位数与分位数 |

### 有序序列查找

| 函数 | 说明 |
|---|---|
| `lower_bound(xs, v)` / `upper_bound(xs, v)` | 第一个 ≥ v / 第一个 > v 的下标 |
| `bsearch(xs, v)` / `bsearch_by(xs, v, key)` | 二分查找，未命中返回 -1 |

### 统计（不含矩阵/相关性，见 stats）

`sum_of` `product_of` `min_of` `max_of` `min_max` `mean` `variance(xs, sample)` `stddev`
`range_of` `argmin` `argmax` `min_by` `max_by` `sum_by` `frequencies` `mode` `all_equal`

### 集合运算（排序 + 线性扫描，O(n log n)）

| 函数 | 说明 |
|---|---|
| `dedup(xs)` / `unique(xs)` | 去重并保留首次出现顺序（要求元素可比较） |
| `dedup_sorted(s)` | 已排序序列上的 O(n) 去重 |
| `union` `intersect` `difference` `symmetric_difference` | 交并差 |
| `is_subset` `is_superset` `is_same_set` `concat` `transpose` | 关系与拼接 |

### Heap（最小堆）

`push` `pop` `peek` `size` `is_empty` `drain` `Heap.from_list(xs)` —— push/pop 都是 O(log n)。

---

## dict —— 哈希容器

开放寻址 + 线性探测，FNV-1a 哈希；负载因子 0.75 扩容、1/8 收缩。查找、插入、删除**平均 O(1)**
（旧版本是 O(n) 线性扫描）。下标语法 `d[k]` / `d[k] = v` 可用。

| 类 | 说明 |
|---|---|
| `Dict(pairs = null)` | 哈希表，键可以是任意可转字符串的值 |
| `Set(source = null)` | 哈希集合 |
| `Counter(source = null)` | 多重集 / 计数表 |

### Dict

`set` `get(k, fallback)` `has` `remove` `size` `is_empty` `clear` `capacity` `load_factor`
`get_or(k, make)` `setdefault(k, v)` `keys_list` `values_list` `items` `keys_sorted` `items_sorted`
`values_sorted` `each(f)` `update` `merge` `copy` `map_values` `filter` `invert` `count_value`
`Dict.from_pairs(pairs)` `to_pairs`；`len(d)`、`str(d)`（按键排序输出）、`d[k]`、`k in d` 也都支持。

### Set

`add` `has` `remove` `size` `is_empty` `items`（已排序） `union` `union_of` `intersect`
`difference` `symmetric_difference` `is_subset` `is_superset` `equals` `clear`。

### Counter

`add(x, n)` `count(x)` `has` `remove` `distinct` `total` `items`（次数降序、同次数按值升序）
`most_common(k)` `keys` `subtract(other)` `clear`。

```annota
new freq = Counter("abracadabra")
freq.most_common(2)        -- [("a", 5), ("b", 2)]
new index = Dict()
index.set("ann", 1)
index.get_or("bob", ()( =0 ))   -- 0，并且写回
```

---

## text —— 字符串算法

| 分组 | 函数 |
|---|---|
| 字符判断 | `code` `is_digit` `is_upper` `is_lower` `is_alpha` `is_alnum` `is_space` `chars` |
| 填充与变形 | `repeat` `pad_left` `pad_right` `center` `reverse` `collapse_spaces` |
| 搜索（KMP O(n+m)） | `find_all(s, pat)`（可重叠） `find_kmp` `count_sub` `replace_all` `split_by(s, sep)` |
| 切分与命名 | `words` `word_count` `join_words` `capitalize_words` `title_case` `snake_case` `kebab_case` `camel_case` |
| 距离与对齐 | `levenshtein` `similarity` `lcs_length` `lcs` `longest_common_prefix` |
| 判断 | `is_palindrome`（忽略非字母数字） `is_anagram` |
| 统计 | `char_freq`（Counter） `word_freq` `ngrams` |
| 编码与格式 | `caesar` `rot13` `csv_parse`（单行，支持引号与 `""` 转义） `csv_format` `base64_encode` `hex_dump` |
| 折行 | `wrap(s, width)` |

---

## numeric —— 数论

| 分组 | 函数 |
|---|---|
| 素数 | `sieve(limit)`（埃氏筛 O(n log log n)） `is_prime`（6k±1 试除） `nth_prime` `prime_sum` |
| 因数 | `prime_factors` `distinct_prime_factors` `divisors` `divisor_count` `divisor_sum` `is_perfect` |
| 欧拉与组合 | `totient` `goldbach` `binomial` `pascal_row` `pascal_triangle` `catalan` `triangular` `is_triangular` |
| 模运算 | `mod_pow`（O(log e)） `mod_inverse` `gcd_ext` `gcd` `lcm` `gcd_of_list` `lcm_of_list` |
| 序列 | `fib` / `fib_pair`（快速倍增 O(log n)） `collatz_length` `collatz_path` `longest_collatz` `hanoi` `hanoi_into` |
| 整数工具 | `integer_sqrt` `integer_root` `is_perfect_square` `pow_int` `digits` `from_digits` `digit_sum` `digital_root` `reverse_number` `is_palindrome_number` `is_armstrong` |
| 进制 | `base_str(n, base)` `parse_base(s, base)`（2..36） |
| 通用二分 | `binary_search_monotone(lo, hi, pred)` —— 返回满足 pred 的最小值 |

---

## matrix —— 矩阵

一维紧凑存储（行优先），访问 O(1)；乘法按 i-k-j 顺序遍历。

| 函数 | 说明 |
|---|---|
| `Matrix(nrows, ncols, fill)` `Matrix.from_lists` `identity` `zeros` `ones` | 构造 |
| `at` `get` `put` `fill_with` `data` `get_row` `get_col` `to_lists` `copy` | 元素访问 |
| `add` `sub` `scale` `map` `mul`（O(n³)） `mul_list` `transpose` `trace` | 运算 |
| `pow(exp)` | 快速幂：O(log e) 次乘法 |
| `det()` | 高斯消元（列主元）O(n³)，返回浮点 |
| `row_count` `col_count` `size` `is_square` `equals` | 形状与比较 |

---

## stats —— 描述统计

`count` `sum_of` `mean` `weighted_mean` `median`（快速选择） `mode` `variance(xs, sample)`
`stddev(xs, sample)` `quantile(xs, q)` `percentile(xs, p)` `iqr` `min_max` `range_of`
`skewness` `kurtosis` `covariance` `correlation`（皮尔逊） `zscores` `normalize`（min-max）
`rank`（平均秩） `histogram(xs, buckets)` `frequencies` `counter` `moving_average(xs, w)`
`summary(xs)` → `Dict`（count / sum / mean / median / min / max / stddev / q1 / q3）。

---

## graph —— 图与并查集

```annota
new g = Graph()            -- 无向图；Graph(true) 为有向图
g.add_edge("a", "b", 4)    -- 第三个参数是权重（默认 1）
g.shortest_path("a", "b")  -- (距离, 路径)，Dijkstra + 二叉堆 O((V+E) log V)
```

| 分组 | 函数 |
|---|---|
| 构建 | `add_vertex` `add_edge(u, v, w)` `vertices` `edges` `neighbors` `weighted_neighbors` `degree` `has_edge` `weight_of` `copy` `Graph.from_edges` |
| 遍历 | `bfs(start)`（层序） `dfs(start)`（显式栈前序） `has_path(a, b)` |
| 最短路 | `distances(start)`（返回 Dict） `shortest_path(a, b)`（带回溯） `all_pairs()`（Floyd–Warshall O(V³)） |
| 结构 | `connected_components` `topological_sort`（Kahn，有环返回 null） `has_cycle`（无向用并查集、有向用三色标记） `is_bipartite` |
| 生成树 | `Graph.kruskal(n, edges)` → `(总权重, 边表)`，按权重排序 + 并查集 |
| 统计 | `vertex_count` `edge_count` `len(g)` `str(g)` |

`DSU(n)`：`find(x)`（路径压缩） `union(a, b)`（按秩合并） `connected(a, b)` `count()` `groups()`。

---

## geometry —— 平面几何

| 分组 | 函数 |
|---|---|
| 基本量 | `dist` `dist2` `manhattan` `dot` `cross(o, a, b)` `orientation` |
| 线段 | `on_segment` `segments_intersect` |
| 多边形 | `polygon_area`（鞋带公式 O(n)） `polygon_perimeter` `polygon_area2` `centroid` `bounding_box` `point_in_polygon`（射线法 O(n)） |
| 凸包 | `convex_hull(pts)`（Andrew 单调链 O(n log n)） `hull_perimeter` `hull_diameter` |
| 最近点对 | `closest_pair(pts)`（分治 O(n log n)，带宽合并） |

---

## 其它模块

| 模块 | 对象 | 内容 |
|---|---|---|
| `slice` | `Slice` `SliceView` | `arr[1:4]` `arr[2:]` `arr[:3]` `arr[:]`（字符串返回字符串） |
| `str` | `Str` | `repeat` `pad_left` `pad_right` `center` `reverse` `capitalize` `title` `count` `blank` `is_digit` `lines` `join` `hex` `format` |
| `mathx` | `Mathx` | `gcd` `lcm` `factorial` `fib` `fib_iter` `is_prime` `primes` `clamp` `lerp` `round_to` `mean` `median` `variance` `stddev` `is_even` `is_odd` `digit_sum` |
| `file` | `File` `Dir` `Path` `FileText` `Result` | 读写、复制、遍历、路径拼接（见 `examples/files.ant`） |
| `time` | `Time` `Stopwatch` | `millis` `clock` `parts` `make` `format`（strftime 风格，脚本实现） `iso` `date` `leap` `days_in_month` `diff_ms` `describe_ms`，计时器 `Stopwatch` |
| `os` | `Os` | `platform` `arch` `cpus` `pid` `home` `temp` `cwd` `env` `set_env` `env_all` `exec` `run` `ok` `which` |
| `thread` | `Thread` `Task` `Pool` | `hardware` `spawn` `run` `parallel_map` `parallel_each` `pool` |
| `net` | `Net` `Tcp` `Url` `Response` | `tcp` `server` `get` `post` `request` `parse_http` `resolve`，`Url.encode/decode/parse` |
| `test` | `Test` | `suite` `ok` `eq` `ne` `near` `raises` `report` `check` |

---

## 示例索引

| 文件 | 内容 |
|---|---|
| `examples/stdlib.ant` | 标准库导览（69 条断言） |
| `examples/algorithms.ant` | 排序 / 选择 / 动态规划 / 图 / 数论 / 字符串 / 几何 / 矩阵（82 条断言） |
| `examples/perf.ant` | 性能基准：12 个工作负载 + 计时表 |
| `examples/collections.ant` | 哈希容器三件套 + 哈希 vs 线性扫描对比 |
| `examples/strings.ant` | 文本算法（KMP / 编辑距离 / CSV / Base64 / 相似度排序） |
| `examples/graphs.ant` | 社交网络 / 最短路 / 拓扑排序 / 最小生成树 / 网格寻路 |
| `examples/numerics.ant` | 数论练习册（素数 / 模运算 / 玩具 RSA / 考拉兹） |
| `examples/geometry.ant` | 凸包 / 面积 / 点包含 / 最近点对 / 线段相交 |
| `examples/analysis/*.ant` | 静态分析的 15 个用例（断言诊断代码） |
