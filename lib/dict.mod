-[ dict.mod : 哈希表容器 Dict / Set / Counter ]-
-[ Dict 是开放寻址 + 线性探测的哈希表（FNV-1a），负载因子 0.75 时扩容、1/8 时收缩， ]
-[ 所有操作平均 O(1)；另提供哈希集合 Set 与计数表 Counter。 ]-

[[module: dict]]
[[version: 2.0]]
[[author: "annotateam"]]
[[macro_depth: 128]]

use seq

Dict(pairs = null)=(
    _cap:int = 8
    _live:int = 0
    _filled:int = 0            -- 有效项 + 墓碑
    _keys:List = []
    _vals:List = []
    _state:List = []           -- 0 空，1 占用，2 墓碑

    -- ---------------------------------------------------------- 哈希与表管理
    [[static]]
    hash_of(s)(
        -- FNV-1a，全程保持非负
        new h = 2166136261
        new i = 0
        while i < len(s)(
            h = (h * 16777619 + ord(s[i])) % 4294967296
            i = i + 1
        )
        =h
    )

    _init(n)(
        _cap = n
        _keys = []
        _vals = []
        _state = []
        for i in 0 to n - 1(
            _keys.push(null)
            _vals.push(null)
            _state.push(0)
        )
        _live = 0
        _filled = 0
    )

    -- 找到 key 所在槽位；不存在则返回第一个可写槽位（空或墓碑）
    _find(k)(
        new h = Dict.hash_of(str(k)) % _cap
        new firstFree = -1
        new i = 0
        while i < _cap(
            new slot = (h + i) % _cap
            new st = _state[slot]
            if st == 0(
                if firstFree < 0( =slot )
                =firstFree
            )
            if st == 1 && _keys[slot] == k( =slot )
            if st == 2 && firstFree < 0( firstFree = slot )
            i = i + 1
        )
        =firstFree
    )

    _insert_raw(k, v)(
        new at = _find(k)
        if _state[at] == 0( _filled = _filled + 1 )
        _keys[at] = k
        _vals[at] = v
        _state[at] = 1
        _live = _live + 1
        =at
    )

    _grow()(
        new ok = _keys
        new ov = _vals
        new os = _state
        _init(_cap * 2)
        for i in 0 to len(os) - 1(
            if os[i] == 1(
                _insert_raw(ok[i], ov[i])
            )
        )
        =_cap
    )

    _shrink()(
        new ok = _keys
        new ov = _vals
        new os = _state
        _init(_cap / 2)
        for i in 0 to len(os) - 1(
            if os[i] == 1(
                _insert_raw(ok[i], ov[i])
            )
        )
        =_cap
    )

    __init__()(
        _init(8)
        if pairs != null(
            for p in pairs(
                set(p[0], p[1])
            )
        )
    )

    -- ---------------------------------------------------------- 基本操作
    set(k, v)(
        new at = _find(k)
        if _state[at] == 1(
            _vals[at] = v
            =v
        )
        if _state[at] == 0( _filled = _filled + 1 )
        _keys[at] = k
        _vals[at] = v
        _state[at] = 1
        _live = _live + 1
        if _filled * 4 >= _cap * 3( _grow() )
        =v
    )

    get(k, fallback = null)(
        new at = _find(k)
        if _state[at] == 1 && _keys[at] == k( =_vals[at] )
        =fallback
    )

    has(k)(
        new at = _find(k)
        =_state[at] == 1 && _keys[at] == k
    )

    remove(k)(
        new at = _find(k)
        if _state[at] != 1 || _keys[at] != k( =false )
        _state[at] = 2
        _keys[at] = null
        _vals[at] = null
        _live = _live - 1
        if _cap > 8 && _live * 8 < _cap( _shrink() )
        =true
    )

    size() = _live

    is_empty() = _live == 0

    capacity() = _cap

    load_factor() = float(_filled) / float(_cap)

    clear()(
        _init(8)
        =true
    )

    -- 不存在时用 make() 生成并写入（只算一次哈希）
    get_or(k, make)(
        new at = _find(k)
        if _state[at] == 1 && _keys[at] == k( =_vals[at] )
        new v = make()
        set(k, v)
        =v
    )

    setdefault(k, v)(
        new at = _find(k)
        if _state[at] == 1 && _keys[at] == k( =_vals[at] )
        set(k, v)
        =v
    )

    -- ---------------------------------------------------------- 遍历
    keys_list()(
        new out = []
        for i in 0 to len(_state) - 1(
            if _state[i] == 1(
                out.push(_keys[i])
            )
        )
        =out
    )

    values_list()(
        new out = []
        for i in 0 to len(_state) - 1(
            if _state[i] == 1(
                out.push(_vals[i])
            )
        )
        =out
    )

    items()(
        new out = []
        for i in 0 to len(_state) - 1(
            if _state[i] == 1(
                out.push((_keys[i], _vals[i]))
            )
        )
        =out
    )

    keys_sorted() = Seq.sort(keys_list())

    items_sorted()(
        new pairs = items()
        =Seq.merge_sort(pairs, (p, q)( =p[0] < q[0] ))
    )

    values_sorted()(
        new pairs = items_sorted()
        new out = []
        for p in pairs(
            out.push(p[1])
        )
        =out
    )

    each(f)(
        for p in items()(
            f(p[0], p[1])
        )
        =_live
    )

    -- ---------------------------------------------------------- 变换
    [[static]]
    from_pairs(pairs)(
        new d = Dict()
        for p in pairs(
            d.set(p[0], p[1])
        )
        =d
    )

    to_pairs() = items()

    update(other)(
        for p in other.items()(
            set(p[0], p[1])
        )
        =_live
    )

    merge(other)(
        new d = copy()
        d.update(other)
        =d
    )

    copy()(
        new d = Dict()
        for i in 0 to len(_state) - 1(
            if _state[i] == 1(
                d.set(_keys[i], _vals[i])
            )
        )
        =d
    )

    map_values(f)(
        new d = Dict()
        for i in 0 to len(_state) - 1(
            if _state[i] == 1(
                d.set(_keys[i], f(_vals[i]))
            )
        )
        =d
    )

    filter(pred)(
        new d = Dict()
        for i in 0 to len(_state) - 1(
            if _state[i] == 1 && pred(_keys[i], _vals[i])(
                d.set(_keys[i], _vals[i])
            )
        )
        =d
    )

    invert()(
        new d = Dict()
        for i in 0 to len(_state) - 1(
            if _state[i] == 1(
                d.set(_vals[i], _keys[i])
            )
        )
        =d
    )

    count_value(v)(
        new n = 0
        for i in 0 to len(_state) - 1(
            if _state[i] == 1 && _vals[i] == v( n = n + 1 )
        )
        =n
    )

    __len__() = _live

    __get__(k) = get(k)

    __set__(k, v)(
        set(k, v)
        =v
    )

    __contains__(k) = has(k)

    __iter__() = keys_list()

    __str__()(
        new out = "{"
        new first = true
        for p in items_sorted()(
            if !first( out = out + ", " )
            first = false
            out = out + str(p[0]) + ": " + str(p[1])
        )
        =out + "}"
    )

    __repr__() = "Dict(" + str(_live) + ")"
)

-[ Set : 哈希集合，成员判断平均 O(1) ]-
Set(source = null)=(
    _d = Dict()

    __init__()(
        if source != null(
            for x in source(
                add(x)
            )
        )
    )

    add(x)(
        _d.set(x, true)
        =x
    )

    has(x) = _d.has(x)

    remove(x) = _d.remove(x)

    size() = _d.size()

    is_empty() = _d.size() == 0

    items() = Seq.sort(_d.keys_list())

    clear()(
        _d.clear()
        =true
    )

    union(other)(
        new s = Set()
        for x in items()( s.add(x) )
        for x in other.items()( s.add(x) )
        =s
    )

    union_of(xs)(
        new s = Set()
        for x in items()( s.add(x) )
        for x in xs( s.add(x) )
        =s
    )

    intersect(other)(
        new s = Set()
        for x in items()(
            if other.has(x)( s.add(x) )
        )
        =s
    )

    difference(other)(
        new s = Set()
        for x in items()(
            if !other.has(x)( s.add(x) )
        )
        =s
    )

    symmetric_difference(other)(
        new s = Set()
        for x in items()(
            if !other.has(x)( s.add(x) )
        )
        for x in other.items()(
            if !has(x)( s.add(x) )
        )
        =s
    )

    is_subset(other)(
        for x in items()(
            if !other.has(x)( =false )
        )
        =true
    )

    is_superset(other) = other.is_subset(this)

    equals(other) = is_subset(other) && other.is_subset(this)

    __len__() = _d.size()

    __contains__(x) = _d.has(x)

    __iter__() = items()

    __str__()(
        new out = "{"
        new first = true
        for x in items()(
            if !first( out = out + ", " )
            first = false
            out = out + str(x)
        )
        =out + "}"
    )
)

-[ Counter : 多重集 / 计数表，按值计数 ]-
Counter(source = null)=(
    _d = Dict()

    __init__()(
        if source != null(
            for x in source(
                add(x)
            )
        )
    )

    add(x, n = 1)(
        _d.set(x, _d.get(x, 0) + n)
        =_d.get(x, 0)
    )

    count(x) = _d.get(x, 0)

    has(x) = _d.has(x)

    remove(x)(
        new c = _d.get(x, 0)
        if c <= 0( =false )
        if c == 1(
            _d.remove(x)
        ) else (
            _d.set(x, c - 1)
        )
        =true
    )

    distinct() = _d.size()

    total()(
        new n = 0
        for v in _d.values_list()(
            n = n + v
        )
        =n
    )

    items()(
        -- 次数降序，同次数按值升序
        new pairs = _d.items()
        =Seq.merge_sort(pairs, (p, q)(
            if p[1] == q[1]( =p[0] < q[0] )
            =p[1] > q[1]
        ))
    )

    most_common(k = null)(
        new pairs = items()
        if k == null || k >= len(pairs)( =pairs )
        =Seq.take(pairs, k)
    )

    keys() = _d.keys_sorted()

    subtract(other)(
        for x in other.keys()(
            new c = _d.get(x, 0) - other.count(x)
            if c <= 0(
                _d.remove(x)
            ) else (
                _d.set(x, c)
            )
        )
        =true
    )

    clear()(
        _d.clear()
        =true
    )

    __len__() = total()

    __contains__(x) = _d.has(x)

    __str__()(
        new out = "{"
        new first = true
        for p in items()(
            if !first( out = out + ", " )
            first = false
            out = out + str(p[0]) + ": " + str(p[1])
        )
        =out + "}"
    )
)
