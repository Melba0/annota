-[ text.mod : 字符串算法（KMP 搜索、编辑距离、LCS、回文/变位词、折行、CSV、Base64……） ]-

[[module: text]]
[[version: 2.0]]
[[author: "annotateam"]]
[[macro_depth: 128]]

use math
use seq
use dict

Text()=(
    -- ---------------------------------------------------------- 字符判断
    [[static]]
    code(c)(
        if len(c) == 0( =-1 )
        =ord(c)
    )

    [[static]]
    is_digit(c)(
        new k = Text.code(c)
        =k >= 48 && k <= 57
    )

    [[static]]
    is_upper(c)(
        new k = Text.code(c)
        =k >= 65 && k <= 90
    )

    [[static]]
    is_lower(c)(
        new k = Text.code(c)
        =k >= 97 && k <= 122
    )

    [[static]]
    is_alpha(c)( =Text.is_upper(c) || Text.is_lower(c) )

    [[static]]
    is_alnum(c)( =Text.is_alpha(c) || Text.is_digit(c) )

    [[static]]
    is_space(c)(
        =c == " " || c == "\t" || c == "\n" || c == "\r"
    )

    [[static]]
    chars(s)(
        new out = []
        for i in 0 to len(s) - 1(
            out.push(s[i])
        )
        =out
    )

    -- ---------------------------------------------------------- 填充与重复
    [[static]]
    repeat(s, n)(
        new out = ""
        for i in 0 to n - 1(
            out = out + s
        )
        =out
    )

    [[static]]
    -- 显示宽度（终端列数）：先按 UTF-8 解码出码点，全角/宽字符算两列，其余算一列。
    -- 注意字符串是字节串，直接 for ch in s 拿到的是字节，不能用来判断宽度。
    [[static]]
    width(s)(
        new n = 0
        new i = 0
        while i < len(s)(
            new b = ord(s[i])
            if b < 0x80(
                n = n + 1
                i = i + 1
            ) else (
                new cp = 0
                new extra = 0
                if b >= 0xF0( cp = b - 0xF0, extra = 3 )
                elif b >= 0xE0( cp = b - 0xE0, extra = 2 )
                else ( cp = b - 0xC0, extra = 1 )
                i = i + 1
                new k = 0
                while k < extra && i < len(s)(
                    cp = cp * 64 + (ord(s[i]) - 0x80)
                    i = i + 1
                    k = k + 1
                )
                if (cp >= 0x1100 && cp <= 0x115F) || (cp >= 0x2E80 && cp <= 0xA4CF) ||
                   (cp >= 0xAC00 && cp <= 0xD7A3) || (cp >= 0xF900 && cp <= 0xFAFF) ||
                   (cp >= 0xFE30 && cp <= 0xFE6F) || (cp >= 0xFF00 && cp <= 0xFF60) ||
                   (cp >= 0xFFE0 && cp <= 0xFFE6) || (cp >= 0x3000 && cp <= 0x303E)(
                    n = n + 2
                ) else (
                    n = n + 1
                )
            )
        )
        =n
    )

    [[static]]
    pad_left(s, width, fill = " ")(
        new out = s
        while Text.width(out) < width(
            out = fill + out
        )
        =out
    )

    [[static]]
    [[static]]
    pad_right(s, width, fill = " ")(
        new out = s
        while Text.width(out) < width(
            out = out + fill
        )
        =out
    )

    [[static]]
    center(s, width, fill = " ")(
        new out = s
        new left = true
        while len(out) < width(
            if left(
                out = fill + out
            ) else (
                out = out + fill
            )
            left = !left
        )
        =out
    )

    [[static]]
    reverse(s)(
        new out = ""
        new i = len(s) - 1
        while i >= 0(
            out = out + s[i]
            i = i - 1
        )
        =out
    )

    [[static]]
    collapse_spaces(s)(
        new out = ""
        new inSpace = false
        for i in 0 to len(s) - 1(
            new c = s[i]
            if Text.is_space(c)(
                if !inSpace(
                    out = out + " "
                    inSpace = true
                )
            ) else (
                out = out + c
                inSpace = false
            )
        )
        =out.trim()
    )

    -- ---------------------------------------------------------- 搜索（KMP O(n+m)）
    -- KMP 的失败函数
    [[static]]
    _failure(pat)(
        new m = len(pat)
        new fail = []
        for i in 0 to m - 1(
            fail.push(0)
        )
        new k = 0
        new i = 1
        while i < m(
            while k > 0 && pat[i] != pat[k](
                k = fail[k - 1]
            )
            if pat[i] == pat[k](
                k = k + 1
            )
            fail[i] = k
            i = i + 1
        )
        =fail
    )

    -- 返回 pattern 在 s 中所有出现的起始下标（允许重叠）
    [[static]]
    find_all(s, pat)(
        new out = []
        if len(pat) == 0 || len(pat) > len(s)( =out )
        new fail = Text._failure(pat)
        new k = 0
        new i = 0
        while i < len(s)(
            while k > 0 && s[i] != pat[k](
                k = fail[k - 1]
            )
            if s[i] == pat[k](
                k = k + 1
            )
            if k == len(pat)(
                out.push(i - k + 1)
                k = fail[k - 1]
            )
            i = i + 1
        )
        =out
    )

    [[static]]
    find_kmp(s, pat)(
        new all = Text.find_all(s, pat)
        if len(all) == 0( =-1 )
        =all[0]
    )

    [[static]]
    count_sub(s, sub)(
        -- 不重叠计数
        if len(sub) == 0( =0 )
        new n = 0
        new i = 0
        while i + len(sub) <= len(s)(
            if s.substr(i, i + len(sub)) == sub(
                n = n + 1
                i = i + len(sub)
            ) else (
                i = i + 1
            )
        )
        =n
    )

    [[static]]
    replace_all(s, from, to)(
        if len(from) == 0( =s )
        new out = ""
        new i = 0
        while i < len(s)(
            if i + len(from) <= len(s) && s.substr(i, i + len(from)) == from(
                out = out + to
                i = i + len(from)
            ) else (
                out = out + s[i]
                i = i + 1
            )
        )
        =out
    )

    -- ---------------------------------------------------------- 切分与大小写
    [[static]]
    words(s)(
        new out = []
        new cur = ""
        for i in 0 to len(s) - 1(
            new c = s[i]
            if Text.is_space(c)(
                if len(cur) > 0(
                    out.push(cur)
                    cur = ""
                )
            ) else (
                cur = cur + c
            )
        )
        if len(cur) > 0( out.push(cur) )
        =out
    )

    [[static]]
    word_count(s)( =len(Text.words(s)) )

    [[static]]
    capitalize_words(s)(
        new out = []
        for w in Text.words(s)(
            out.push(w[0].upper() + w.substr(1, len(w)).lower())
        )
        =out.join(" ")
    )

    [[static]]
    title_case(s)( =Text.capitalize_words(s) )

    [[static]]
    snake_case(s)(
        new out = ""
        for i in 0 to len(s) - 1(
            new c = s[i]
            if Text.is_upper(c) && i > 0(
                out = out + "_"
            )
            if c == " " || c == "-"(
                out = out + "_"
            ) else (
                out = out + c.lower()
            )
        )
        =Text.replace_all(Text.replace_all(out, "__", "_"), "__", "_")
    )

    [[static]]
    kebab_case(s)( =Text.replace_all(Text.snake_case(s), "_", "-") )

    [[static]]
    camel_case(s)(
        new parts = Text.words(Text.replace_all(s, "_", " "))
        new out = ""
        for (i, w) in enumerate(parts)(
            if i == 0(
                out = out + w.lower()
            ) else (
                out = out + w[0].upper() + w.substr(1, len(w)).lower()
            )
        )
        =out
    )

    -- ---------------------------------------------------------- 编辑距离与公共子序列
    -- 莱文斯坦距离（滚动数组，O(n*m) 时间，O(min) 空间）
    [[static]]
    levenshtein(a, b)(
        new n = len(a)
        new m = len(b)
        if n == 0( =m )
        if m == 0( =n )
        new prev = []
        for j in 0 to m(
            prev.push(j)
        )
        for i in 1 to n(
            new cur = [i]
            for j in 1 to m(
                new cost = 1
                if a[i - 1] == b[j - 1]( cost = 0 )
                new best = prev[j] + 1
                if cur[j - 1] + 1 < best( best = cur[j - 1] + 1 )
                if prev[j - 1] + cost < best( best = prev[j - 1] + cost )
                cur.push(best)
            )
            prev = cur
        )
        =prev[m]
    )

    [[static]]
    similarity(a, b)(
        new longest = len(a)
        if len(b) > longest( longest = len(b) )
        if longest == 0( =1.0 )
        =1.0 - float(Text.levenshtein(a, b)) / float(longest)
    )

    -- 最长公共子序列长度（两行滚动）
    [[static]]
    lcs_length(a, b)(
        new m = len(b)
        new prev = []
        for j in 0 to m(
            prev.push(0)
        )
        for i in 1 to len(a)(
            new cur = [0]
            for j in 1 to m(
                if a[i - 1] == b[j - 1](
                    cur.push(prev[j - 1] + 1)
                ) else (
                    new best = prev[j]
                    if cur[j - 1] > best( best = cur[j - 1] )
                    cur.push(best)
                )
            )
            prev = cur
        )
        =prev[m]
    )

    -- 最长公共子序列（完整表，便于回溯）
    [[static]]
    lcs(a, b)(
        new n = len(a)
        new m = len(b)
        new table = []
        for i in 0 to n(
            new row = []
            for j in 0 to m(
                row.push(0)
            )
            table.push(row)
        )
        for i in 1 to n(
            for j in 1 to m(
                if a[i - 1] == b[j - 1](
                    table[i][j] = table[i - 1][j - 1] + 1
                ) elif table[i - 1][j] >= table[i][j - 1](
                    table[i][j] = table[i - 1][j]
                ) else (
                    table[i][j] = table[i][j - 1]
                )
            )
        )
        new out = ""
        new i = n
        new j = m
        while i > 0 && j > 0(
            if a[i - 1] == b[j - 1](
                out = a[i - 1] + out
                i = i - 1
                j = j - 1
            ) elif table[i - 1][j] >= table[i][j - 1](
                i = i - 1
            ) else (
                j = j - 1
            )
        )
        =out
    )

    [[static]]
    longest_common_prefix(strs)(
        if len(strs) == 0( ="" )
        new out = strs[0]
        for s in strs(
            while len(out) > 0 && s.substr(0, len(out)) != out(
                out = out.substr(0, len(out) - 1)
            )
            if len(out) == 0( ="" )
        )
        =out
    )

    -- ---------------------------------------------------------- 判断类
    [[static]]
    is_palindrome(s)(
        new left = 0
        new right = len(s) - 1
        while left < right(
            while left < right && !Text.is_alnum(s[left])(
                left = left + 1
            )
            while left < right && !Text.is_alnum(s[right])(
                right = right - 1
            )
            if s[left].lower() != s[right].lower()( =false )
            left = left + 1
            right = right - 1
        )
        =true
    )

    [[static]]
    is_anagram(a, b)(
        -- 只比较字母与数字（空格、标点不影响判断）
        new x = []
        for c in Text.chars(a.lower())(
            if Text.is_alnum(c)( x.push(c) )
        )
        new y = []
        for c in Text.chars(b.lower())(
            if Text.is_alnum(c)( y.push(c) )
        )
        =Seq.equals(Seq.sort(x), Seq.sort(y))
    )

    -- 按多字符分隔符切分（KMP 定位，O(n + m)；原生 split 只按单字符切）
    [[static]]
    split_by(s, sep)(
        new out = []
        if len(sep) == 0(
            for c in Text.chars(s)( out.push(c) )
            =out
        )
        new at = Text.find_all(s, sep)
        new start = 0
        for hit in at(
            out.push(s.substr(start, hit))
            start = hit + len(sep)
        )
        out.push(s.substr(start, len(s)))
        =out
    )

    [[static]]
    join_words(xs, sep = " ") = xs.join(sep)

    [[static]]
    char_freq(s)(
        new c = Counter(Text.chars(s))
        =c
    )

    -- ---------------------------------------------------------- 古典密码
    [[static]]
    caesar(s, shift)(
        new out = ""
        for i in 0 to len(s) - 1(
            new c = s[i]
            new k = Text.code(c)
            if Text.is_upper(c)(
                k = 65 + ((k - 65 + shift) % 26 + 26) % 26
                out = out + chr(k)
            ) elif Text.is_lower(c)(
                k = 97 + ((k - 97 + shift) % 26 + 26) % 26
                out = out + chr(k)
            ) else (
                out = out + c
            )
        )
        =out
    )

    [[static]]
    rot13(s)( =Text.caesar(s, 13) )

    -- ---------------------------------------------------------- 折行 / CSV / Base64
    [[static]]
    wrap(s, width)(
        new out = []
        new cur = ""
        for w in Text.words(s)(
            if len(cur) == 0(
                cur = w
            ) elif len(cur) + 1 + len(w) <= width(
                cur = cur + " " + w
            ) else (
                out.push(cur)
                cur = w
            )
        )
        if len(cur) > 0( out.push(cur) )
        =out
    )

    -- 极简 CSV：支持双引号包裹与 "" 转义
    [[static]]
    csv_parse(line)(
        new out = []
        new cur = ""
        new inQuote = false
        new i = 0
        while i < len(line)(
            new c = line[i]
            if inQuote(
                if c == "\""(
                    if i + 1 < len(line) && line[i + 1] == "\""(
                        cur = cur + "\""
                        i = i + 2
                        continue
                    )
                    inQuote = false
                    i = i + 1
                    continue
                )
                cur = cur + c
            ) elif c == ","(
                out.push(cur)
                cur = ""
            ) elif c == "\""(
                inQuote = true
            ) else (
                cur = cur + c
            )
            i = i + 1
        )
        out.push(cur)
        =out
    )

    [[static]]
    csv_format(cells)(
        new out = []
        for cell in cells(
            new s = str(cell)
            if s.index_of(",") >= 0 || s.index_of("\"") >= 0(
                s = "\"" + Text.replace_all(s, "\"", "\"\"") + "\""
            )
            out.push(s)
        )
        =out.join(",")
    )

    [[static]]
    base64_encode(s)(
        new alphabet = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/"
        new out = ""
        new i = 0
        while i < len(s)(
            new b0 = Text.code(s[i])
            new b1 = 0
            new b2 = 0
            new have1 = i + 1 < len(s)
            new have2 = i + 2 < len(s)
            if have1( b1 = Text.code(s[i + 1]) )
            if have2( b2 = Text.code(s[i + 2]) )
            new triple = b0 * 65536 + b1 * 256 + b2
            out = out + alphabet[math.floor(triple / 262144) % 64]
            out = out + alphabet[math.floor(triple / 4096) % 64]
            if have1(
                out = out + alphabet[math.floor(triple / 64) % 64]
            ) else (
                out = out + "="
            )
            if have2(
                out = out + alphabet[triple % 64]
            ) else (
                out = out + "="
            )
            i = i + 3
        )
        =out
    )

    [[static]]
    hex_dump(s)(
        new out = ""
        for i in 0 to len(s) - 1(
            new k = Text.code(s[i])
            new hi = math.floor(k / 16)
            new lo = k % 16
            out = out + "0123456789abcdef"[hi] + "0123456789abcdef"[lo]
            if i + 1 < len(s)( out = out + " " )
        )
        =out.trim()
    )

    [[static]]
    ngrams(s, n)(
        new out = []
        if n <= 0 || n > len(s)( =out )
        for i in 0 to len(s) - n(
            out.push(s.substr(i, i + n))
        )
        =out
    )

    -- 简易词频统计（用哈希计数表，O(n)）
    [[static]]
    word_freq(s)(
        new c = Counter(Text.words(s.lower()))
        =c
    )
)
