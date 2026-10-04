-[ slice.mod : read-only array slices, implemented with the macro system ]-

[[module: slice]]
[[version: 1.0]]
[[author: "annotateam"]]
[[macro_depth: 64]]

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
    SliceView($x, $a, $b)
)
macro $x[$a:](
    SliceView($x, $a, $x.size())
)
macro $x[:$b](
    SliceView($x, 0, $b)
)
macro $x[:](
    SliceView($x, 0, $x.size())
)
