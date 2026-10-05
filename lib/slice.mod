-[ slice.mod : read-only array slices, implemented with the macro system ]-
-[ 列表/元组 -> SliceView；字符串 -> 直接用 substr 取子串（substr 的第二个参数是结束下标） ]-

[[module: slice]]
[[version: 1.1]]
[[author: "annotateam"]]
[[macro_depth: 64]]

Slice()=(
    [[static]]
    length(x)(
        if typeof(x) == "String"( =len(x) )
        =x.size()
    )

    [[static]]
    make(src, start, stop)(
        -- 字符串切片要返回字符串，而不是字符视图
        if typeof(src) == "String"(
            new n = len(src)
            new a = start
            new b = stop
            if a < 0( a = 0 )
            if b > n( b = n )
            if b < a( b = a )
            =src.substr(a, b)
        )
        =SliceView(src, start, stop)
    )
)

SliceView(src, start, stop)=(
    _src:List
    _start:int
    _stop:int

    size() = _stop - _start

    get(i)(
        new k = _start + i
        if k < _start || k >= _stop(
            throw "slice index out of range: " + i
        )
        =_src.get(k)
    )

    to_list()(
        new out = []
        for i in 0 to size() - 1(
            out.push(get(i))
        )
        =out
    )

    __str__()(
        new s = "SliceView("
        for i in 0 to size() - 1(
            if i > 0( s = s + ", " )
            s = s + get(i)
        )
        =s + ")"
    )

    __len__() = size()

    __iter__() = to_list()
)

macro $x[$a:$b](
    Slice.make($x, $a, $b)
)
macro $x[$a:](
    Slice.make($x, $a, Slice.length($x))
)
macro $x[:$b](
    Slice.make($x, 0, $b)
)
macro $x[:](
    Slice.make($x, 0, Slice.length($x))
)
