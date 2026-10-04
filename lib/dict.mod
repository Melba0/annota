-[ dict.mod : 键值容器（语言没有映射字面量，这里用类实现） ]-

[[module: dict]]
[[version: 1.0]]
[[author: "annotateam"]]
[[macro_depth: 64]]

use seq

Dict()=(
    keys:List = []
    vals:List = []

    size() = len(keys)

    is_empty() = len(keys) == 0

    has(k) = keys.contains(k)

    get(k, fallback = null)(
        new i = keys.index_of(k)
        if i < 0( =fallback )
        =vals[i]
    )

    set(k, v)(
        new i = keys.index_of(k)
        if i < 0(
            keys.push(k)
            vals.push(v)
        ) else (
            vals[i] = v
        )
    )

    remove(k)(
        new i = keys.index_of(k)
        if i < 0( =false )
        keys.remove(i)
        vals.remove(i)
        =true
    )

    key_list()(
        new out = []
        for k in keys( out.push(k) )
        =out
    )

    value_list()(
        new out = []
        for v in vals( out.push(v) )
        =out
    )

    items()(
        new out = []
        for i in 0 to len(keys) - 1(
            out.push((keys[i], vals[i]))
        )
        =out
    )

    clear()(
        keys = []
        vals = []
    )

    merge(other)(
        for (k, v) in other.items()(
            set(k, v)
        )
    )

    copy()(
        new out = Dict()
        out.merge(this)
        =out
    )

    keys_sorted() = Seq.sort(keys)

    __len__() = len(keys)

    __str__()(
        new out = "{"
        for i in 0 to len(keys) - 1(
            if i > 0( out = out + ", " )
            out = out + keys[i] + ": " + vals[i]
        )
        =out + "}"
    )
)
