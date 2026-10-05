-[ seq.mod : 序列（List / Tuple / 数组）通用操作与算法 ]-
-[ 排序是稳定的归并排序 O(n log n)，另有堆排序；选择用快速选择 O(n) 平均； ]
-[ 查找在有序序列上走二分 O(log n)；集合运算走排序 + 线性扫描，不做 O(n^2) 暴力。 ]-

[[module: seq]]
[[version: 2.0]]
[[author: "annotateam"]]
[[macro_depth: 128]]

use math

Seq()=(
    -- ============================================================ 遍历与变换
    [[static]]
    map(xs, f)(
        new out = []
        for x in xs(
            out.push(f(x))
        )
        =out
    )

    [[static]]
    filter(xs, pred)(
        new out = []
        for x in xs(
            if pred(x)(
                out.push(x)
            )
        )
        =out
    )

    [[static]]
    reject(xs, pred) = Seq.filter(xs, (x)( =!pred(x) ))

    [[static]]
    reduce(xs, f, init)(
        new acc = init
        for x in xs(
            acc = f(acc, x)
        )
        =acc
    )

    [[static]]
    fold(xs, init, f) = Seq.reduce(xs, f, init)

    -- 前缀和：scan(xs, f, init) 返回每一步的累计结果（长度 = len(xs) + 1）
    [[static]]
    scan(xs, f, init)(
        new out = [init]
        new acc = init
        for x in xs(
            acc = f(acc, x)
            out.push(acc)
        )
        =out
    )

    [[static]]
    prefix_sums(xs)(
        new out = []
        new acc = 0
        for x in xs(
            acc = acc + x
            out.push(acc)
        )
        =out
    )

    [[static]]
    flat_map(xs, f) = Seq.flatten(Seq.map(xs, f))

    [[static]]
    compact(xs)(
        new out = []
        for x in xs(
            if x != null(
                out.push(x)
            )
        )
        =out
    )

    [[static]]
    any(xs, pred)(
        for x in xs(
            if pred(x)( =true )
        )
        =false
    )

    [[static]]
    all(xs, pred)(
        for x in xs(
            if !pred(x)( =false )
        )
        =true
    )

    [[static]]
    none(xs, pred) = !Seq.any(xs, pred)

    [[static]]
    count(xs, value)(
        new n = 0
        for x in xs(
            if x == value( n = n + 1 )
        )
        =n
    )

    [[static]]
    count_where(xs, pred)(
        new n = 0
        for x in xs(
            if pred(x)( n = n + 1 )
        )
        =n
    )

    [[static]]
    find(xs, pred, fallback = null)(
        for x in xs(
            if pred(x)( =x )
        )
        =fallback
    )

    [[static]]
    find_last(xs, pred, fallback = null)(
        new found = fallback
        for x in xs(
            if pred(x)( found = x )
        )
        =found
    )

    [[static]]
    index_where(xs, pred)(
        new i = 0
        for x in xs(
            if pred(x)( =i )
            i = i + 1
        )
        =-1
    )

    [[static]]
    last_index_where(xs, pred)(
        new at = -1
        new i = 0
        for x in xs(
            if pred(x)( at = i )
            i = i + 1
        )
        =at
    )

    [[static]]
    index_of(xs, value, fallback = -1)(
        for (i, x) in enumerate(xs)(
            if x == value( =i )
        )
        =fallback
    )

    [[static]]
    contains(xs, value) = Seq.index_of(xs, value) >= 0

    [[static]]
    contains_all(xs, ys)(
        for y in ys(
            if !Seq.contains(xs, y)( =false )
        )
        =true
    )

    [[static]]
    contains_any(xs, ys)(
        for y in ys(
            if Seq.contains(xs, y)( =true )
        )
        =false
    )

    [[static]]
    reverse(xs)(
        new out = []
        new n = len(xs)
        for i in n - 1 to 0 step -1(
            out.push(xs[i])
        )
        =out
    )

    [[static]]
    copy(xs)(
        new out = []
        for x in xs( out.push(x) )
        =out
    )

    [[static]]
    repeat(value, n)(
        new out = []
        for i in 0 to n - 1(
            out.push(value)
        )
        =out
    )

    [[static]]
    iota(from, upto)(
        new out = []
        for i in from to upto - 1(
            out.push(i)
        )
        =out
    )

    [[static]]
    times(n, f)(
        for i in 0 to n - 1(
            f(i)
        )
        =n
    )

    -- ============================================================ 排序（归并 / 堆 / 插入）
    -- 稳定归并排序：less(a, b) 为真表示 a 应排在 b 前面
    [[static]]
    merge_sort(xs, less)(
        new n = len(xs)
        if n < 2(
            =Seq.copy(xs)
        )
        new mid = math.floor(n / 2)
        new left = Seq.merge_sort(Seq.slice_of(xs, 0, mid), less)
        new right = Seq.merge_sort(Seq.slice_of(xs, mid, n), less)
        =Seq.merge_sorted(left, right, less)
    )

    [[static]]
    merge_sorted(a, b, less)(
        new out = []
        new i = 0
        new j = 0
        while i < len(a) && j < len(b)(
            if less(b[j], a[i])(
                out.push(b[j])
                j = j + 1
            ) else (
                out.push(a[i])
                i = i + 1
            )
        )
        while i < len(a)(
            out.push(a[i])
            i = i + 1
        )
        while j < len(b)(
            out.push(b[j])
            j = j + 1
        )
        =out
    )

    -- 兼容旧签名：默认升序，reverse = true 时降序（仍是稳定的归并排序）
    [[static]]
    sort(xs, reverse = false)(
        if reverse(
            =Seq.merge_sort(xs, (a, b)( =a > b ))
        )
        =Seq.merge_sort(xs, (a, b)( =a < b ))
    )

    [[static]]
    sort_by(xs, key, reverse = false)(
        new pairs = []
        for x in xs(
            pairs.push((key(x), x))
        )
        if reverse(
            pairs = Seq.merge_sort(pairs, (p, q)( =p[0] > q[0] ))
        ) else (
            pairs = Seq.merge_sort(pairs, (p, q)( =p[0] < q[0] ))
        )
        new out = []
        for p in pairs(
            out.push(p[1])
        )
        =out
    )

    -- 堆排序：原地风格的 O(n log n)，不需要递归深度
    [[static]]
    heap_sort(xs, reverse = false)(
        new a = Seq.copy(xs)
        new n = len(a)
        -- 建堆（自底向上 sift down）
        new i = math.floor(n / 2) - 1
        while i >= 0(
            Seq._sift_down(a, i, n)
            i = i - 1
        )
        new end = n - 1
        while end > 0(
            new t = a[0]
            a[0] = a[end]
            a[end] = t
            Seq._sift_down(a, 0, end)
            end = end - 1
        )
        if reverse(
            =Seq.reverse(a)
        )
        =a
    )

    [[static]]
    _sift_down(a, root, size)(
        new done = false
        while !done(
            new child = root * 2 + 1
            if child >= size( =true )
            if child + 1 < size && a[child + 1] > a[child](
                child = child + 1
            )
            if a[root] >= a[child]( =true )
            new t = a[root]
            a[root] = a[child]
            a[child] = t
            root = child
        )
        =true
    )

    [[static]]
    insertion_sort(xs, less)(
        new a = Seq.copy(xs)
        for i in 1 to len(a) - 1(
            new key = a[i]
            new j = i - 1
            while j >= 0 && less(key, a[j])(
                a[j + 1] = a[j]
                j = j - 1
            )
            a[j + 1] = key
        )
        =a
    )

    [[static]]
    is_sorted(xs, less = null)(
        if len(xs) < 2( =true )
        new i = 1
        while i < len(xs)(
            if less == null(
                if xs[i - 1] > xs[i]( =false )
            ) else (
                if less(xs[i], xs[i - 1])( =false )
            )
            i = i + 1
        )
        =true
    )

    -- 原生排序（C++ std::sort 支撑），性能最好，但不受稳定性保证
    [[static]]
    sort_native(xs) = sorted(xs)

    -- 把 x 插入已经有序的序列，保持有序（二分定位）
    [[static]]
    insert_sorted(xs, x)(
        new at = Seq.upper_bound(xs, x)
        new out = Seq.copy(xs)
        out.insert(at, x)
        =out
    )

    -- ============================================================ 有序序列上的查找（二分）
    [[static]]
    lower_bound(xs, value)(
        new lo = 0
        new hi = len(xs)
        while lo < hi(
            new mid = math.floor((lo + hi) / 2)
            if xs[mid] < value(
                lo = mid + 1
            ) else (
                hi = mid
            )
        )
        =lo
    )

    [[static]]
    upper_bound(xs, value)(
        new lo = 0
        new hi = len(xs)
        while lo < hi(
            new mid = math.floor((lo + hi) / 2)
            if xs[mid] <= value(
                lo = mid + 1
            ) else (
                hi = mid
            )
        )
        =lo
    )

    [[static]]
    bsearch(xs, value)(
        new at = Seq.lower_bound(xs, value)
        if at < len(xs) && xs[at] == value( =at )
        =-1
    )

    [[static]]
    bsearch_by(xs, value, key)(
        new lo = 0
        new hi = len(xs)
        while lo < hi(
            new mid = math.floor((lo + hi) / 2)
            if key(xs[mid]) < value(
                lo = mid + 1
            ) else (
                hi = mid
            )
        )
        if lo < len(xs) && key(xs[lo]) == value( =lo )
        =-1
    )

    -- ============================================================ 选择（快速选择 O(n) 平均）
    -- 第 k 小（k 从 0 开始），平均 O(n)；Lomuto 划分保证枢轴落在最终位置
    [[static]]
    kth(xs, k)(
        if len(xs) == 0( =null )
        new a = Seq.copy(xs)
        new lo = 0
        new hi = len(a) - 1
        new want = k
        if want < 0( want = 0 )
        if want > hi( want = hi )
        while lo < hi(
            new p = Seq._partition(a, lo, hi)
            if p == want( =a[p] )
            if want < p(
                hi = p - 1
            ) else (
                lo = p + 1
            )
        )
        =a[lo]
    )

    [[static]]
    _partition(a, lo, hi)(
        -- 三数取中，避免有序输入退化成 O(n^2)
        new mid = math.floor((lo + hi) / 2)
        if a[mid] < a[lo](
            new t = a[lo]
            a[lo] = a[mid]
            a[mid] = t
        )
        if a[hi] < a[lo](
            new t = a[lo]
            a[lo] = a[hi]
            a[hi] = t
        )
        if a[hi] < a[mid](
            new t = a[mid]
            a[mid] = a[hi]
            a[hi] = t
        )
        new pivot = a[hi]
        new i = lo
        for j in lo to hi - 1(
            if a[j] < pivot(
                new t = a[i]
                a[i] = a[j]
                a[j] = t
                i = i + 1
            )
        )
        new t = a[i]
        a[i] = a[hi]
        a[hi] = t
        =i
    )

    [[static]]
    median(xs)(
        new n = len(xs)
        if n == 0( =null )
        if n % 2 == 1(
            =Seq.kth(xs, math.floor(n / 2))
        )
        =float(Seq.kth(xs, n / 2 - 1) + Seq.kth(xs, n / 2)) / 2.0
    )

    -- 分位数：q 取 0..1，最近秩法（不需要插值）
    [[static]]
    quantile(xs, q)(
        new n = len(xs)
        if n == 0( =null )
        new pos = math.floor(q * (n - 1))
        =Seq.kth(xs, pos)
    )

    [[static]]
    median_sorted(xs)(
        new n = len(xs)
        if n == 0( =null )
        if n % 2 == 1(
            =xs[math.floor(n / 2)]
        )
        =float(xs[n / 2 - 1] + xs[n / 2]) / 2.0
    )

    -- ============================================================ 统计
    [[static]]
    sum_of(xs) = sum(xs)

    [[static]]
    product_of(xs)(
        new acc = 1
        for x in xs(
            acc = acc * x
        )
        =acc
    )

    [[static]]
    min_of(xs, fallback = null)(
        if len(xs) == 0( =fallback )
        new best = xs[0]
        for x in xs(
            if x < best( best = x )
        )
        =best
    )

    [[static]]
    max_of(xs, fallback = null)(
        if len(xs) == 0( =fallback )
        new best = xs[0]
        for x in xs(
            if x > best( best = x )
        )
        =best
    )

    [[static]]
    min_max(xs)(
        if len(xs) == 0( =null )
        new lo = xs[0]
        new hi = xs[0]
        for x in xs(
            if x < lo( lo = x )
            if x > hi( hi = x )
        )
        =(lo, hi)
    )

    [[static]]
    mean(xs)(
        if len(xs) == 0( =null )
        =float(sum(xs)) / float(len(xs))
    )

    [[static]]
    variance(xs, sample = false)(
        new n = len(xs)
        if n == 0( =null )
        new m = Seq.mean(xs)
        new acc = 0.0
        for x in xs(
            acc = acc + (x - m) * (x - m)
        )
        if sample && n > 1(
            =acc / float(n - 1)
        )
        =acc / float(n)
    )

    [[static]]
    stddev(xs, sample = false) = math.sqrt(Seq.variance(xs, sample))

    [[static]]
    range_of(xs)(
        new mm = Seq.min_max(xs)
        if mm == null( =null )
        =mm[1] - mm[0]
    )

    [[static]]
    argmin(xs)( =Seq.index_of(xs, Seq.min_of(xs)) )

    [[static]]
    argmax(xs)( =Seq.index_of(xs, Seq.max_of(xs)) )

    [[static]]
    min_by(xs, key, fallback = null)(
        if len(xs) == 0( =fallback )
        new best = xs[0]
        for x in xs(
            if key(x) < key(best)( best = x )
        )
        =best
    )

    [[static]]
    max_by(xs, key, fallback = null)(
        if len(xs) == 0( =fallback )
        new best = xs[0]
        for x in xs(
            if key(x) > key(best)( best = x )
        )
        =best
    )

    [[static]]
    sum_by(xs, key)(
        new acc = 0
        for x in xs(
            acc = acc + key(x)
        )
        =acc
    )

    -- 频次表：排序后数连续段，O(n log n)，不做嵌套暴力
    [[static]]
    frequencies(xs)(
        new out = []
        if len(xs) == 0( =out )
        new s = Seq.sort(xs)
        new cur = s[0]
        new n = 1
        for i in 1 to len(s) - 1(
            if s[i] == cur(
                n = n + 1
            ) else (
                out.push((cur, n))
                cur = s[i]
                n = 1
            )
        )
        out.push((cur, n))
        =out
    )

    [[static]]
    mode(xs)(
        new fr = Seq.frequencies(xs)
        if len(fr) == 0( =null )
        new best = fr[0]
        for p in fr(
            if p[1] > best[1]( best = p )
        )
        =best[0]
    )

    [[static]]
    all_equal(xs)(
        if len(xs) < 2( =true )
        new first = xs[0]
        for x in xs(
            if x != first( =false )
        )
        =true
    )

    [[static]]
    is_same_set(xs, ys)( =Seq.is_subset(xs, ys) && Seq.is_subset(ys, xs) )

    -- ============================================================ 集合运算
    [[static]]
    unique(xs)( =Seq.dedup(xs) )

    -- 去重：排序 (值, 首次下标) 后线性扫描，O(n log n)，并保留首次出现的顺序
    -- 需要值支持 `<` 比较；不可比较的类型请用 unique（O(n^2) 的兜底实现）
    [[static]]
    dedup(xs)(
        new pairs = []
        for (i, x) in enumerate(xs)(
            pairs.push((x, i))
        )
        pairs = Seq.merge_sort(pairs, (p, q)(
            if p[0] == q[0]( =p[1] < q[1] )
            =p[0] < q[0]
        ))
        new picked = []
        new last = null
        new have = false
        for p in pairs(
            if !have || p[0] != last(
                picked.push(p)
                last = p[0]
                have = true
            )
        )
        picked = Seq.merge_sort(picked, (p, q)( =p[1] < q[1] ))
        new out = []
        for p in picked(
            out.push(p[0])
        )
        =out
    )

    [[static]]
    dedup_sorted(s)(
        new out = []
        for x in s(
            if len(out) == 0 || out[len(out) - 1] != x(
                out.push(x)
            )
        )
        =out
    )

    [[static]]
    union(xs, ys) = Seq.dedup(Seq.concat(xs, ys))

    [[static]]
    concat(xs, ys)(
        new out = []
        for x in xs( out.push(x) )
        for y in ys( out.push(y) )
        =out
    )

    [[static]]
    intersect(xs, ys)(
        new a = Seq.dedup_sorted(Seq.sort(xs))
        new b = Seq.dedup_sorted(Seq.sort(ys))
        new out = []
        new i = 0
        new j = 0
        while i < len(a) && j < len(b)(
            if a[i] == b[j](
                out.push(a[i])
                i = i + 1
                j = j + 1
            ) elif a[i] < b[j](
                i = i + 1
            ) else (
                j = j + 1
            )
        )
        =out
    )

    [[static]]
    difference(xs, ys)(
        new a = Seq.dedup_sorted(Seq.sort(xs))
        new b = Seq.dedup_sorted(Seq.sort(ys))
        new out = []
        new i = 0
        new j = 0
        while i < len(a)(
            if j >= len(b) || a[i] < b[j](
                out.push(a[i])
                i = i + 1
            ) elif a[i] == b[j](
                i = i + 1
                j = j + 1
            ) else (
                j = j + 1
            )
        )
        =out
    )

    [[static]]
    symmetric_difference(xs, ys) = Seq.union(Seq.difference(xs, ys), Seq.difference(ys, xs))

    [[static]]
    is_subset(xs, ys)(
        for x in Seq.dedup(xs)(
            if !Seq.contains(ys, x)( =false )
        )
        =true
    )

    [[static]]
    is_superset(xs, ys) = Seq.is_subset(ys, xs)

    -- ============================================================ 切分与组合
    [[static]]
    flatten(xs)(
        new out = []
        for x in xs(
            for y in x(
                out.push(y)
            )
        )
        =out
    )

    [[static]]
    transpose(rows)(
        new out = []
        if len(rows) == 0( =out )
        new width = len(rows[0])
        for c in 0 to width - 1(
            new row = []
            for r in rows(
                if c < len(r)(
                    row.push(r[c])
                )
            )
            out.push(row)
        )
        =out
    )

    [[static]]
    take(xs, n)(
        new out = []
        for i in 0 to n - 1(
            if i >= len(xs)( break )
            out.push(xs[i])
        )
        =out
    )

    [[static]]
    drop(xs, n)(
        new out = []
        for i in 0 to len(xs) - 1(
            if i >= n(
                out.push(xs[i])
            )
        )
        =out
    )

    [[static]]
    take_while(xs, pred)(
        new out = []
        for x in xs(
            if !pred(x)( break )
            out.push(x)
        )
        =out
    )

    [[static]]
    drop_while(xs, pred)(
        new out = []
        new dropping = true
        for x in xs(
            if dropping && pred(x)(
                continue
            )
            dropping = false
            out.push(x)
        )
        =out
    )

    [[static]]
    slice_of(xs, from, upto)(
        new out = []
        for i in from to upto - 1(
            if i >= 0 && i < len(xs)(
                out.push(xs[i])
            )
        )
        =out
    )

    [[static]]
    slice_step(xs, from, upto, step)(
        new out = []
        if step == 0( =out )
        for i in from to upto - 1 step step(
            if i >= 0 && i < len(xs)(
                out.push(xs[i])
            )
        )
        =out
    )

    [[static]]
    split_at(xs, at)(
        new a = []
        new b = []
        for (i, x) in enumerate(xs)(
            if i < at(
                a.push(x)
            ) else (
                b.push(x)
            )
        )
        =(a, b)
    )

    [[static]]
    partition(xs, pred)(
        new yes = []
        new no = []
        for x in xs(
            if pred(x)(
                yes.push(x)
            ) else (
                no.push(x)
            )
        )
        =(yes, no)
    )

    [[static]]
    chunk(xs, size)(
        new out = []
        if size <= 0( =out )
        new cur = []
        for x in xs(
            cur.push(x)
            if len(cur) == size(
                out.push(cur)
                cur = []
            )
        )
        if len(cur) > 0(
            out.push(cur)
        )
        =out
    )

    [[static]]
    window(xs, size)(
        new out = []
        if size <= 0 || size > len(xs)( =out )
        for i in 0 to len(xs) - size(
            out.push(Seq.slice_of(xs, i, i + size))
        )
        =out
    )

    [[static]]
    pairwise(xs)(
        new out = []
        for i in 0 to len(xs) - 2(
            out.push((xs[i], xs[i + 1]))
        )
        =out
    )

    [[static]]
    runs(xs)(
        new out = []
        if len(xs) == 0( =out )
        new cur = [xs[0]]
        for i in 1 to len(xs) - 1(
            if xs[i] == cur[len(cur) - 1](
                cur.push(xs[i])
            ) else (
                out.push(cur)
                cur = [xs[i]]
            )
        )
        out.push(cur)
        =out
    )

    [[static]]
    rotate(xs, by)(
        new n = len(xs)
        if n == 0( =[] )
        new k = by % n
        if k < 0( k = k + n )
        new out = []
        for i in k to n - 1(
            out.push(xs[i])
        )
        for i in 0 to k - 1(
            out.push(xs[i])
        )
        =out
    )

    [[static]]
    swap(xs, i, j)(
        if i < 0 || j < 0 || i >= len(xs) || j >= len(xs)( =false )
        new t = xs[i]
        xs[i] = xs[j]
        xs[j] = t
        =true
    )

    [[static]]
    delete_at(xs, at)(
        new out = []
        for (i, x) in enumerate(xs)(
            if i != at(
                out.push(x)
            )
        )
        =out
    )

    [[static]]
    indices(xs)( =Seq.iota(0, len(xs)) )

    [[static]]
    enumerate_of(xs)(
        new out = []
        for (i, x) in enumerate(xs)(
            out.push((i, x))
        )
        =out
    )

    [[static]]
    zip_with(xs, ys, f)(
        new out = []
        for (i, x) in enumerate(xs)(
            if i >= len(ys)( break )
            out.push(f(x, ys[i]))
        )
        =out
    )

    [[static]]
    zip3(xs, ys, zs)(
        new out = []
        for (i, x) in enumerate(xs)(
            if i >= len(ys) || i >= len(zs)( break )
            out.push((x, ys[i], zs[i]))
        )
        =out
    )

    [[static]]
    unzip(pairs)(
        new a = []
        new b = []
        for p in pairs(
            a.push(p[0])
            b.push(p[1])
        )
        =(a, b)
    )

    [[static]]
    group_of(xs, size) = Seq.chunk(xs, size)

    [[static]]
    join(xs, sep = ", ") = xs.join(sep)

    [[static]]
    equals(xs, ys)(
        if len(xs) != len(ys)( =false )
        for (i, x) in enumerate(xs)(
            if x != ys[i]( =false )
        )
        =true
    )

    [[static]]
    first(xs, fallback = null)(
        if len(xs) == 0( =fallback )
        =xs[0]
    )

    [[static]]
    last(xs, fallback = null)(
        if len(xs) == 0( =fallback )
        =xs[len(xs) - 1]
    )

    -- 线性同余伪随机（可复现，便于测试）
    [[static]]
    shuffle(xs, seed = 12345)(
        new a = Seq.copy(xs)
        new rng = seed
        new n = len(a)
        for i in n - 1 to 1 step -1(
            rng = (rng * 1103515245 + 12345) % 2147483648
            new j = rng % (i + 1)
            new t = a[i]
            a[i] = a[j]
            a[j] = t
        )
        =a
    )

    [[static]]
    sample(xs, k, seed = 12345)( =Seq.take(Seq.shuffle(xs, seed), k) )
)

-[ Heap : 二叉最小堆，push/pop 都是 O(log n) ]-
Heap()=(
    data:List = []

    size() = len(data)

    is_empty() = len(data) == 0

    peek()(
        if len(data) == 0( =null )
        =data[0]
    )

    push(value)(
        data.push(value)
        new i = len(data) - 1
        while i > 0(
            new parent = math.floor((i - 1) / 2)
            if data[parent] <= data[i]( break )
            new t = data[parent]
            data[parent] = data[i]
            data[i] = t
            i = parent
        )
        =value
    )

    pop()(
        if len(data) == 0( =null )
        new top = data[0]
        new last = data.pop()
        if len(data) > 0(
            data[0] = last
            new i = 0
            while true(
                new l = i * 2 + 1
                new r = i * 2 + 2
                new small = i
                if l < len(data) && data[l] < data[small]( small = l )
                if r < len(data) && data[r] < data[small]( small = r )
                if small == i( break )
                new t = data[i]
                data[i] = data[small]
                data[small] = t
                i = small
            )
        )
        =top
    )

    [[static]]
    from_list(xs)(
        new h = Heap()
        for x in xs(
            h.push(x)
        )
        =h
    )

    -- 堆排序的另一种用法：不断 pop 得到有序序列
    drain()(
        new out = []
        while !is_empty()(
            out.push(pop())
        )
        =out
    )

    __len__() = len(data)
)
