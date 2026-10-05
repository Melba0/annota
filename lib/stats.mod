-[ stats.mod : 描述统计与相关分析（中位数走快速选择，直方图/频次走哈希表） ]-

[[module: stats]]
[[version: 2.0]]
[[author: "annotateam"]]
[[macro_depth: 128]]

use math
use seq
use dict

Stats()=(
    [[static]]
    count(xs) = len(xs)

    [[static]]
    sum_of(xs) = sum(xs)

    [[static]]
    mean(xs)(
        if len(xs) == 0( =null )
        =float(sum(xs)) / float(len(xs))
    )

    [[static]]
    weighted_mean(xs, weights)(
        if len(xs) == 0 || len(xs) != len(weights)( =null )
        new num = 0.0
        new den = 0.0
        for (i, x) in enumerate(xs)(
            num = num + float(x) * float(weights[i])
            den = den + float(weights[i])
        )
        if den == 0.0( =null )
        =num / den
    )

    -- 中位数：快速选择 O(n) 平均，不需要整体排序
    [[static]]
    median(xs) = Seq.median(xs)

    [[static]]
    mode(xs) = Seq.mode(xs)

    [[static]]
    variance(xs, sample = false) = Seq.variance(xs, sample)

    [[static]]
    stddev(xs, sample = false) = Seq.stddev(xs, sample)

    [[static]]
    quantile(xs, q)( =Seq.quantile(xs, q) )

    [[static]]
    percentile(xs, p)(
        if p < 0( p = 0 )
        if p > 100( p = 100 )
        =Seq.quantile(xs, float(p) / 100.0)
    )

    [[static]]
    iqr(xs)(
        new n = len(xs)
        if n == 0( =null )
        =Seq.quantile(xs, 0.75) - Seq.quantile(xs, 0.25)
    )

    [[static]]
    min_max(xs) = Seq.min_max(xs)

    [[static]]
    range_of(xs) = Seq.range_of(xs)

    [[static]]
    skewness(xs)(
        -- 三阶标准矩
        new n = len(xs)
        if n < 3( =null )
        new m = Stats.mean(xs)
        new s = Stats.stddev(xs)
        if s == 0.0( =0.0 )
        new acc = 0.0
        for x in xs(
            acc = acc + ((float(x) - m) / s) ** 3
        )
        =acc / float(n)
    )

    [[static]]
    kurtosis(xs)(
        -- 超额峰度
        new n = len(xs)
        if n < 4( =null )
        new m = Stats.mean(xs)
        new s = Stats.stddev(xs)
        if s == 0.0( =0.0 )
        new acc = 0.0
        for x in xs(
            acc = acc + ((float(x) - m) / s) ** 4
        )
        =acc / float(n) - 3.0
    )

    [[static]]
    covariance(xs, ys, sample = false)(
        new n = len(xs)
        if n == 0 || n != len(ys)( =null )
        new mx = Stats.mean(xs)
        new my = Stats.mean(ys)
        new acc = 0.0
        for (i, x) in enumerate(xs)(
            acc = acc + (float(x) - mx) * (float(ys[i]) - my)
        )
        if sample && n > 1( =acc / float(n - 1) )
        =acc / float(n)
    )

    [[static]]
    correlation(xs, ys)(
        -- 皮尔逊相关系数
        new cov = Stats.covariance(xs, ys)
        new sx = Stats.stddev(xs)
        new sy = Stats.stddev(ys)
        if cov == null || sx == 0.0 || sy == 0.0( =null )
        =cov / (sx * sy)
    )

    [[static]]
    zscores(xs)(
        new m = Stats.mean(xs)
        new s = Stats.stddev(xs)
        new out = []
        if s == 0.0(
            for x in xs( out.push(0.0) )
            =out
        )
        for x in xs(
            out.push((float(x) - m) / s)
        )
        =out
    )

    [[static]]
    normalize(xs)(
        -- 最小-最大归一化到 [0, 1]
        new mm = Seq.min_max(xs)
        new out = []
        if mm == null( =out )
        new span = float(mm[1] - mm[0])
        if span == 0.0(
            for x in xs( out.push(0.0) )
            =out
        )
        for x in xs(
            out.push((float(x) - float(mm[0])) / span)
        )
        =out
    )

    [[static]]
    rank(xs)(
        -- 平均秩（并列取平均），O(n log n)
        new pairs = []
        for (i, x) in enumerate(xs)(
            pairs.push((x, i))
        )
        pairs = Seq.merge_sort(pairs, (p, q)( =p[0] < q[0] ))
        new out = []
        for i in 0 to len(xs) - 1(
            out.push(0.0)
        )
        new i = 0
        while i < len(pairs)(
            new j = i
            while j + 1 < len(pairs) && pairs[j + 1][0] == pairs[i][0](
                j = j + 1
            )
            new avg = (float(i) + float(j)) / 2.0 + 1.0
            for k in i to j(
                out[pairs[k][1]] = avg
            )
            i = j + 1
        )
        =out
    )

    [[static]]
    histogram(xs, buckets)(
        -- 等宽直方图：返回 (最小, 最大, 每桶计数)
        if len(xs) == 0 || buckets <= 0( =null )
        new mm = Seq.min_max(xs)
        new lo = mm[0]
        new hi = mm[1]
        new counts = []
        for i in 0 to buckets - 1(
            counts.push(0)
        )
        if hi == lo(
            counts[0] = len(xs)
            =(lo, hi, counts)
        )
        new width = float(hi - lo) / float(buckets)
        for x in xs(
            new idx = math.floor((float(x) - float(lo)) / width)
            if idx >= buckets( idx = buckets - 1 )
            if idx < 0( idx = 0 )
            counts[idx] = counts[idx] + 1
        )
        =(lo, hi, counts)
    )

    [[static]]
    frequencies(xs) = Seq.frequencies(xs)

    [[static]]
    counter(xs)(
        new c = Counter(xs)
        =c
    )

    [[static]]
    moving_average(xs, window)(
        new out = []
        if window <= 0 || window > len(xs)( =out )
        new acc = 0.0
        for i in 0 to len(xs) - 1(
            acc = acc + float(xs[i])
            if i >= window(
                acc = acc - float(xs[i - window])
            )
            if i >= window - 1(
                out.push(acc / float(window))
            )
        )
        =out
    )

    [[static]]
    summary(xs)(
        if len(xs) == 0( =null )
        new d = Dict()
        d.set("count", len(xs))
        d.set("sum", sum(xs))
        d.set("mean", Stats.mean(xs))
        d.set("median", Stats.median(xs))
        d.set("min", Seq.min_of(xs))
        d.set("max", Seq.max_of(xs))
        d.set("stddev", Stats.stddev(xs))
        d.set("q1", Seq.quantile(xs, 0.25))
        d.set("q3", Seq.quantile(xs, 0.75))
        =d
    )
)
