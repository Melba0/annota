-[ file.mod : 文件 / 目录 / 路径的高级封装 ]-
-[ 底层是 _file_* / _dir_* / _path_* 原语（README 3.4）：原语直接调用操作系统、出错即抛出； ]-
-[ 这里提供更好用的接口，并统一两种错误处理风格： ]-
-[   1) 直接调用        File.read(p)           失败抛出异常（配合 except 使用） ]-
-[   2) 返回 Result     File.try_read(p)       返回 Result(ok / value / error)，不抛异常 ]-
-[   3) 带默认值        File.read_or(p, "...") 失败时返回默认值 ]-

[[module: file]]
[[version: 2.0]]
[[author: "annotateam"]]

use str
use json
use time

Result()=(
    ok:bool = false
    value = null
    error = ""

    is_ok() = ok
    is_err() = !ok

    unwrap()(
        if !ok(
            if error != ""( throw error )
            throw str(value)
        )
        =value
    )

    unwrap_or(fallback)(
        if !ok( =fallback )
        =value
    )

    __str__()(
        if ok( ="Ok(" + str(value) + ")" )
        ="Err(" + error + ")"
    )
)

FileText()=(
    -- 读写文本时反复要用到的处理
    [[static]]
    to_lines(text)(
        new raw = text.split("\n")
        new out = []
        for line in raw(
            if line.ends_with("\r")(
                line = line.substr(0, len(line) - 1)
            )
            out.push(line)
        )
        while len(out) > 0(
            if out[len(out) - 1] != ""( break )
            out.pop()
        )
        =out
    )

    [[static]]
    from_lines(lines)(
        if len(lines) == 0( ="" )
        =lines.join("\n") + "\n"
    )

    [[static]]
    count_lines(text)(
        if text == ""( =0 )
        =len(FileText.to_lines(text))
    )
)

Path()=(
    [[static]]
    join(a, b, c = null)(
        if c == null( =_path_join(a, b) )
        =_path_join(_path_join(a, b), c)
    )

    [[static]]
    join_all(parts)(
        new out = "."
        for p in parts(
            out = _path_join(out, p)
        )
        =out
    )

    [[static]]
    dirname(p) = _path_dirname(p)

    [[static]]
    basename(p) = _path_basename(p)

    [[static]]
    ext(p) = _path_ext(p)

    [[static]]
    stem(p)(
        new name = _path_basename(p)
        new e = _path_ext(p)
        if e == ""( =name )
        =name.substr(0, len(name) - len(e))
    )

    [[static]]
    with_ext(p, ext)(
        new e = Path.ext(p)
        if e == ""( =p + ext )
        =p.substr(0, len(p) - len(e)) + ext
    )

    [[static]]
    abs(p) = _path_abs(p)

    [[static]]
    normalize(p) = _path_normalize(p)

    [[static]]
    is_abs(p)(
        if p.starts_with("/")( =true )
        if len(p) >= 2 && p.substr(1, 1) == ":"( =true )
        =false
    )

    [[static]]
    sibling(p, name) = Path.join(Path.dirname(p), name)

    [[static]]
    parts(p)(
        new norm = _path_normalize(p)
        new out = []
        for piece in norm.replace("\\", "/").split("/")(
            if piece != ""( out.push(piece) )
        )
        =out
    )
)

File()=(
    -- 统一把「可能抛异常的调用」变成 Result
    [[static]]
    attempt(f)(
        new r = Result()
        (
            r.value = f()
            r.ok = true
            except e(
                r.error = e
            )
        )
        =r
    )

    -- ---------- 读
    [[static]]
    read(path) = _file_read(path)

    [[static]]
    try_read(path) = File.attempt(()( =_file_read(path) ))

    [[static]]
    read_or(path, fallback) = File.try_read(path).unwrap_or(fallback)

    [[static]]
    bytes(path) = _file_read_bytes(path)

    [[static]]
    try_bytes(path) = File.attempt(()( =_file_read_bytes(path) ))

    [[static]]
    lines(path) = FileText.to_lines(File.read(path))

    [[static]]
    try_lines(path) = File.attempt(()( =FileText.to_lines(File.read(path)) ))

    [[static]]
    line(path, n)(
        new all = File.lines(path)
        if n < 0 || n >= len(all)( ="" )
        =all[n]
    )

    [[static]]
    json(path) = json.parse(File.read(path))

    [[static]]
    try_json(path) = File.attempt(()( =json.parse(File.read(path)) ))

    [[static]]
    csv(path, sep = ",")(
        -- 简易 CSV：按分隔符切分，去掉两侧空白与包裹的引号
        new rows = []
        for line in File.lines(path)(
            if line.trim() == ""( continue )
            new cells = []
            for cell in line.split(sep)(
                new c = cell.trim()
                if c.starts_with("\"") && c.ends_with("\"") && len(c) >= 2(
                    c = c.substr(1, len(c) - 1)
                )
                cells.push(c)
            )
            rows.push(cells)
        )
        =rows
    )

    -- ---------- 写
    [[static]]
    write(path, text)(
        File.ensure_parent(path)
        =_file_write(path, text)
    )

    [[static]]
    try_write(path, text) = File.attempt(()( =File.write(path, text) ))

    [[static]]
    append(path, text)( =_file_append(path, text) )

    [[static]]
    append_line(path, text) = _file_append(path, text + "\n")

    [[static]]
    write_lines(path, lines) = File.write(path, FileText.from_lines(lines))

    [[static]]
    write_json(path, value) = File.write(path, json.stringify(value))

    [[static]]
    touch(path)(
        if File.exists(path)( =false )
        _file_append(path, "")
        =true
    )

    -- ---------- 查询
    [[static]]
    exists(path) = _file_exists(path)

    [[static]]
    is_file(path)(
        if !_file_exists(path)( =false )
        =!_file_is_dir(path)
    )

    [[static]]
    is_dir(path) = _file_is_dir(path)

    [[static]]
    is_empty(path) = _file_size(path) == 0

    [[static]]
    size(path) = _file_size(path)

    [[static]]
    mtime(path) = _file_mtime(path)

    [[static]]
    describe(path)(
        if !File.exists(path)( ="missing   " + path )
        if File.is_dir(path)( ="dir       " + path )
        ="file      " + path + "  " + str(File.size(path)) + " bytes"
    )

    -- ---------- 组织
    [[static]]
    copy(from, to)(
        File.ensure_parent(to)
        =_file_copy(from, to, true)
    )

    [[static]]
    move(from, to)(
        File.ensure_parent(to)
        =_file_rename(from, to)
    )

    [[static]]
    remove(path) = _file_remove(path)

    [[static]]
    ensure_parent(path)(
        new dir = Path.dirname(path)
        if dir == "" || dir == "."( =false )
        if _file_is_dir(dir)( =false )
        _dir_make(dir)
        =true
    )

    [[static]]
    unique(prefix = "annota") = prefix + "-" + str(int(time.now()))
)

Dir()=(
    [[static]]
    list(path) = _dir_list(path)

    [[static]]
    try_list(path) = File.attempt(()( =_dir_list(path) ))

    [[static]]
    files(path)(
        new out = []
        for name in _dir_list(path)(
            if !_file_is_dir(_path_join(path, name))(
                out.push(name)
            )
        )
        =out
    )

    [[static]]
    dirs(path)(
        new out = []
        for name in _dir_list(path)(
            if _file_is_dir(_path_join(path, name))(
                out.push(name)
            )
        )
        =out
    )

    [[static]]
    make(path) = _dir_make(path)

    [[static]]
    remove(path, recursive = false) = _dir_remove(path, recursive)

    [[static]]
    walk(path) = _dir_walk(path)

    [[static]]
    find(path, suffix)(
        new out = []
        for rel in _dir_walk(path)(
            if rel.ends_with(suffix)(
                out.push(rel)
            )
        )
        =out
    )

    [[static]]
    find_in(path, suffix)(
        new out = []
        for rel in _dir_walk(path)(
            if rel.ends_with(suffix)(
                out.push(_path_join(path, rel))
            )
        )
        =out
    )

    [[static]]
    clear(path)(
        new removed = 0
        for name in _dir_list(path)(
            new full = _path_join(path, name)
            if _file_is_dir(full)(
                _dir_remove(full, true)
            ) else (
                _file_remove(full)
            )
            removed = removed + 1
        )
        =removed
    )

    [[static]]
    count(path) = len(_dir_list(path))

    [[static]]
    size_of(path)(
        -- 目录下所有文件的字节数（递归）
        new total = 0
        for rel in _dir_walk(path)(
            new full = _path_join(path, rel)
            if !_file_is_dir(full)(
                total = total + _file_size(full)
            )
        )
        =total
    )
)
