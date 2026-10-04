-[ str.mod : 字符串工具 ]-

[[module: str]]
[[version: 1.0]]
[[author: "annotateam"]]
[[macro_depth: 64]]

Str()=(
    [[static]]
    repeat(s, n)(
        new out = ""
        for i in 1 to n(
            out = out + s
        )
        =out
    )

    [[static]]
    pad_left(s, width, ch = " ")(
        new out = s
        while out.size() < width(
            out = ch + out
        )
        =out
    )

    [[static]]
    pad_right(s, width, ch = " ")(
        new out = s
        while out.size() < width(
            out = out + ch
        )
        =out
    )

    [[static]]
    center(s, width, ch = " ")(
        new out = s
        new left = true
        while out.size() < width(
            if left(
                out = ch + out
            ) else (
                out = out + ch
            )
            left = !left
        )
        =out
    )

    [[static]]
    reverse(s)(
        new out = ""
        for i in s.size() - 1 to 0 step -1(
            out = out + s[i]
        )
        =out
    )

    [[static]]
    capitalize(s)(
        if s.size() == 0( =s )
        =s.substr(0, 1).upper() + s.substr(1).lower()
    )

    [[static]]
    title(s)(
        new out = ""
        for word in s.split(" ")(
            if out.size() > 0( out = out + " " )
            out = out + Str.capitalize(word)
        )
        =out
    )

    [[static]]
    count(s, sub)(
        if sub.size() == 0( =0 )
        new n = 0
        new i = 0
        while i <= s.size() - sub.size()(
            if s.substr(i, i + sub.size()) == sub(
                n = n + 1
                i = i + sub.size()
            ) else (
                i = i + 1
            )
        )
        =n
    )

    [[static]]
    blank(s) = s.trim().size() == 0

    [[static]]
    is_digit(s)(
        if s.size() == 0( =false )
        for c in s(
            if c < "0" || c > "9"( =false )
        )
        =true
    )

    [[static]]
    lines(s)(
        new raw = s.split("\n")
        new out = []
        for line in raw(
            out.push(line.trim())
        )
        =out
    )

    [[static]]
    join(xs, sep = ", ") = xs.join(sep)

    [[static]]
    hex(n)(
        if n < 0( ="-" + Str.hex(-n) )
        if n == 0( ="0" )
        new digits = "0123456789abcdef"
        new out = ""
        new value = n
        while value > 0(
            new d = value % 16
            out = digits[d] + out
            value = math.floor(value / 16)
        )
        =out
    )

    -- 极简模板：`{}` 依序替换为参数，`{{` / `}}` 为字面量
    [[static]]
    format(tpl, ...args)(
        new out = ""
        new ai = 0
        new i = 0
        while i < tpl.size()(
            new c = tpl[i]
            if c == "{" && i + 1 < tpl.size() && tpl[i + 1] == "{"(
                out = out + "{"
                i = i + 2
            ) else (
                if c == "}" && i + 1 < tpl.size() && tpl[i + 1] == "}"(
                    out = out + "}"
                    i = i + 2
                ) else (
                    if c == "{" && i + 1 < tpl.size() && tpl[i + 1] == "}"(
                        if ai < args.size()(
                            out = out + args[ai]
                            ai = ai + 1
                        )
                        i = i + 2
                    ) else (
                        out = out + c
                        i = i + 1
                    )
                )
            )
        )
        =out
    )
)
