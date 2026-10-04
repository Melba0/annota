-[ seq.mod : 序列（List / Tuple / String）通用操作 ]-

[[module: seq]]
[[version: 1.0]]
[[author: "annotateam"]]
[[macro_depth: 64]]

Seq()=(
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
    count(xs, value)(
        new n = 0
        for x in xs(
            if x == value( n = n + 1 )
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
    index_where(xs, pred)(
        new i = 0
        for x in xs(
            if pred(x)( =i )
            i = i + 1
        )
        =-1
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
    unique(xs)(
        new out = []
        for x in xs(
            if !out.contains(x)(
                out.push(x)
            )
        )
        =out
    )

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

    -- 插入排序：语言只有 `<` / `>`，因此这里演示如何用脚本自身实现排序
    [[static]]
    sort(xs, reverse = false)(
        new out = []
        for x in xs( out.push(x) )
        for i in 1 to len(out) - 1(
            new key = out[i]
            new j = i - 1
            while j >= 0 && out[j] > key(
                out[j + 1] = out[j]
                j = j - 1
            )
            out[j + 1] = key
        )
        if reverse(
            =Seq.reverse(out)
        )
        =out
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
    first(xs, fallback = null)(
        if len(xs) == 0( =fallback )
        =xs[0]
    )

    [[static]]
    last(xs, fallback = null)(
        if len(xs) == 0( =fallback )
        =xs[len(xs) - 1]
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
    join(xs, sep = ", ") = xs.join(sep)

    [[static]]
    sum_of(xs) = sum(xs)
)
