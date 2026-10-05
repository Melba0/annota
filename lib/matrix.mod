-[ matrix.mod : 矩阵（一维紧凑存储 + O(n^3) 分块友好的乘法、高斯消元求行列式、快速幂） ]-

[[module: matrix]]
[[version: 2.0]]
[[author: "annotateam"]]
[[macro_depth: 128]]

use math
use seq

Matrix(nrows, ncols, fill = 0)=(
    _r:int = 0
    _c:int = 0
    _data:List = []

    __init__()(
        _r = nrows
        _c = ncols
        _data = []
        for i in 0 to nrows * ncols - 1(
            _data.push(fill)
        )
    )

    row_count() = _r

    col_count() = _c

    size() = _r * _c

    is_square() = _r == _c

    -- 行优先的一维下标，访问是 O(1)
    at(i, j) = _data[i * _c + j]

    get(i, j, fallback = null)(
        if i < 0 || j < 0 || i >= _r || j >= _c( =fallback )
        =_data[i * _c + j]
    )

    put(i, j, v)(
        if i < 0 || j < 0 || i >= _r || j >= _c( =null )
        _data[i * _c + j] = v
        =v
    )

    fill_with(v)(
        for i in 0 to _r * _c - 1(
            _data[i] = v
        )
        =this
    )

    data() = Seq.copy(_data)

    get_row(i)(
        new out = []
        for j in 0 to _c - 1(
            out.push(at(i, j))
        )
        =out
    )

    get_col(j)(
        new out = []
        for i in 0 to _r - 1(
            out.push(at(i, j))
        )
        =out
    )

    to_lists()(
        new out = []
        for i in 0 to _r - 1(
            out.push(get_row(i))
        )
        =out
    )

    copy()(
        new m = Matrix(_r, _c, 0)
        for i in 0 to _r * _c - 1(
            m._data[i] = _data[i]
        )
        =m
    )

    [[static]]
    from_lists(rows)(
        new r = len(rows)
        new c = 0
        if r > 0( c = len(rows[0]) )
        new m = Matrix(r, c, 0)
        for i in 0 to r - 1(
            for j in 0 to c - 1(
                m.put(i, j, rows[i][j])
            )
        )
        =m
    )

    [[static]]
    identity(n)(
        new m = Matrix(n, n, 0)
        for i in 0 to n - 1(
            m.put(i, i, 1)
        )
        =m
    )

    [[static]]
    zeros(nrows, ncols) = Matrix(nrows, ncols, 0)

    [[static]]
    ones(nrows, ncols) = Matrix(nrows, ncols, 1)

    -- ---------------------------------------------------------- 运算
    add(other)(
        new m = Matrix(_r, _c, 0)
        for i in 0 to _r - 1(
            for j in 0 to _c - 1(
                m.put(i, j, at(i, j) + other.at(i, j))
            )
        )
        =m
    )

    sub(other)(
        new m = Matrix(_r, _c, 0)
        for i in 0 to _r - 1(
            for j in 0 to _c - 1(
                m.put(i, j, at(i, j) - other.at(i, j))
            )
        )
        =m
    )

    scale(k)(
        new m = Matrix(_r, _c, 0)
        for i in 0 to _r - 1(
            for j in 0 to _c - 1(
                m.put(i, j, at(i, j) * k)
            )
        )
        =m
    )

    map(f)(
        new m = Matrix(_r, _c, 0)
        for i in 0 to _r - 1(
            for j in 0 to _c - 1(
                m.put(i, j, f(at(i, j)))
            )
        )
        =m
    )

    transpose()(
        new m = Matrix(_c, _r, 0)
        for i in 0 to _r - 1(
            for j in 0 to _c - 1(
                m.put(j, i, at(i, j))
            )
        )
        =m
    )

    -- i-k-j 循环顺序，减少缓存跳动
    mul(other)(
        new m = Matrix(_r, other.col_count(), 0)
        for i in 0 to _r - 1(
            for k in 0 to _c - 1(
                new a = at(i, k)
                if a != 0(
                    for j in 0 to other.col_count() - 1(
                        m.put(i, j, m.at(i, j) + a * other.at(k, j))
                    )
                )
            )
        )
        =m
    )

    -- 矩阵 × 列向量
    mul_list(xs)(
        new out = []
        for i in 0 to _r - 1(
            new acc = 0
            for j in 0 to _c - 1(
                acc = acc + at(i, j) * xs[j]
            )
            out.push(acc)
        )
        =out
    )

    trace()(
        new total = 0
        new n = _r
        if _c < n( n = _c )
        for i in 0 to n - 1(
            total = total + at(i, i)
        )
        =total
    )

    -- 快速幂（仅方阵）：O(log e) 次矩阵乘法
    pow(exp)(
        new result = Matrix.identity(_r)
        new base = copy()
        new e = exp
        while e > 0(
            if e % 2 == 1(
                result = result.mul(base)
            )
            base = base.mul(base)
            e = math.floor(e / 2)
        )
        =result
    )

    -- 高斯消元（列主元）求行列式：O(n^3)，返回浮点
    det()(
        if !is_square()( =null )
        new n = _r
        new a = []
        for i in 0 to n - 1(
            new r = get_row(i)
            new fr = []
            for v in r(
                fr.push(float(v))
            )
            a.push(fr)
        )
        new sign = 1.0
        new d = 1.0
        for i in 0 to n - 1(
            new pivot = i
            for r in i + 1 to n - 1(
                new av = math.abs(a[r][i])
                if av > math.abs(a[pivot][i])( pivot = r )
            )
            if math.abs(a[pivot][i]) < 0.000000001( =0.0 )
            if pivot != i(
                new t = a[i]
                a[i] = a[pivot]
                a[pivot] = t
                sign = 0.0 - sign
            )
            d = d * a[i][i]
            for r in i + 1 to n - 1(
                new factor = a[r][i] / a[i][i]
                for c in i to n - 1(
                    a[r][c] = a[r][c] - factor * a[i][c]
                )
            )
        )
        =sign * d
    )

    equals(other)(
        if _r != other.row_count() || _c != other.col_count()( =false )
        for i in 0 to _r - 1(
            for j in 0 to _c - 1(
                if at(i, j) != other.at(i, j)( =false )
            )
        )
        =true
    )

    __len__() = _r

    __str__()(
        new out = ""
        for i in 0 to _r - 1(
            if i > 0( out = out + "\n" )
            out = out + "["
            for j in 0 to _c - 1(
                if j > 0( out = out + ", " )
                out = out + str(at(i, j))
            )
            out = out + "]"
        )
        =out
    )
)
