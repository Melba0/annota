-[ numeric.mod : 数论与整数算法（筛法、快速幂、扩展欧几里得、快速倍增斐波那契……） ]-
-[ 复杂度都写在注释里；不做 sqrt(n) 以上的暴力枚举。 ]-

[[module: numeric]]
[[version: 2.0]]
[[author: "annotateam"]]
[[macro_depth: 128]]

use math
use seq

Num()=(
    -- ---------------------------------------------------------- 素数
    -- 埃拉托斯特尼筛：O(n log log n)
    [[static]]
    sieve(limit)(
        new flags = []
        for i in 0 to limit(
            flags.push(true)
        )
        if limit >= 0( flags[0] = false )
        if limit >= 1( flags[1] = false )
        new p = 2
        while p * p <= limit(
            if flags[p](
                new m = p * p
                while m <= limit(
                    flags[m] = false
                    m = m + p
                )
            )
            p = p + 1
        )
        new out = []
        for i in 2 to limit(
            if flags[i](
                out.push(i)
            )
        )
        =out
    )

    -- 试除到 sqrt(n)，跳过 2/3 的倍数：O(sqrt n)
    [[static]]
    is_prime(n)(
        if n < 2( =false )
        if n < 4( =true )
        if n % 2 == 0( =false )
        if n % 3 == 0( =false )
        new i = 5
        while i * i <= n(
            if n % i == 0 || n % (i + 2) == 0( =false )
            i = i + 6
        )
        =true
    )

    [[static]]
    nth_prime(n)(
        if n <= 0( =null )
        new count = 0
        new candidate = 1
        while count < n(
            candidate = candidate + 1
            if Num.is_prime(candidate)(
                count = count + 1
            )
        )
        =candidate
    )

    -- 质因数分解（含重数）：O(sqrt n)
    [[static]]
    prime_factors(n)(
        new out = []
        new v = n
        if v < 0( v = 0 - v )
        new d = 2
        while d * d <= v(
            while v % d == 0(
                out.push(d)
                v = v / d
            )
            if d == 2(
                d = 3
            ) else (
                d = d + 2
            )
        )
        if v > 1(
            out.push(v)
        )
        =out
    )

    [[static]]
    distinct_prime_factors(n)(
        new out = []
        new last = 0
        for p in Num.prime_factors(n)(
            if p != last(
                out.push(p)
                last = p
            )
        )
        =out
    )

    -- 约数：成对枚举到 sqrt(n)：O(sqrt n)
    [[static]]
    divisors(n)(
        new v = math.abs(n)
        new small = []
        new large = []
        new d = 1
        while d * d <= v(
            if v % d == 0(
                small.push(d)
                if d != v / d(
                    large.push(v / d)
                )
            )
            d = d + 1
        )
        =Seq.concat(small, Seq.reverse(large))
    )

    [[static]]
    divisor_count(n)(
        new total = 1
        new v = math.abs(n)
        if v == 0( =0 )
        new count = 0
        while v % 2 == 0(
            count = count + 1
            v = v / 2
        )
        total = total * (count + 1)
        new d = 3
        while d * d <= v(
            count = 0
            while v % d == 0(
                count = count + 1
                v = v / d
            )
            total = total * (count + 1)
            d = d + 2
        )
        if v > 1( total = total * 2 )
        =total
    )

    [[static]]
    divisor_sum(n)(
        new total = 0
        for d in Num.divisors(n)(
            total = total + d
        )
        =total
    )

    [[static]]
    is_perfect(n)(
        if n < 2( =false )
        =Num.divisor_sum(n) - n == n
    )

    -- 欧拉 φ：用质因数分解：O(sqrt n)
    [[static]]
    totient(n)(
        if n <= 0( =0 )
        new result = n
        for p in Num.distinct_prime_factors(n)(
            result = result - result / p
        )
        =result
    )

    -- 哥德巴赫拆分：返回 (a, b)，a + b = n 且都是素数
    [[static]]
    goldbach(n)(
        if n <= 2 || n % 2 != 0( =null )
        new a = 2
        while a <= n / 2(
            if Num.is_prime(a) && Num.is_prime(n - a)(
                =(a, n - a)
            )
            if a == 2(
                a = 3
            ) else (
                a = a + 2
            )
        )
        =null
    )

    -- ---------------------------------------------------------- 模算术
    -- 快速幂：O(log e)
    [[static]]
    mod_pow(base, exp, mod)(
        if mod == 0( =null )
        new result = 1
        new b = base % mod
        new e = exp
        while e > 0(
            if e % 2 == 1(
                result = (result * b) % mod
            )
            b = (b * b) % mod
            e = math.floor(e / 2)
        )
        =result
    )

    [[static]]
    pow_int(base, exp)(
        new result = 1
        new e = exp
        new b = base
        while e > 0(
            if e % 2 == 1(
                result = result * b
            )
            b = b * b
            e = math.floor(e / 2)
        )
        =result
    )

    -- 扩展欧几里得：返回 (g, x, y) 使得 a*x + b*y = g
    [[static]]
    gcd_ext(a, b)(
        if b == 0( =(a, 1, 0) )
        new r = Num.gcd_ext(b, a % b)
        =(r[0], r[2], r[1] - (a / b) * r[2])
    )

    [[static]]
    gcd(a, b)(
        new x = math.abs(a)
        new y = math.abs(b)
        while y != 0(
            new t = y
            y = x % y
            x = t
        )
        =x
    )

    [[static]]
    lcm(a, b)(
        if a == 0 || b == 0( =0 )
        =math.abs(a / Num.gcd(a, b) * b)
    )

    [[static]]
    gcd_of_list(xs)(
        new g = 0
        for x in xs(
            g = Num.gcd(g, x)
        )
        =g
    )

    [[static]]
    lcm_of_list(xs)(
        new l = 1
        for x in xs(
            l = Num.lcm(l, x)
        )
        =l
    )

    [[static]]
    mod_inverse(a, mod)(
        new r = Num.gcd_ext(a, mod)
        if r[0] != 1( =null )
        =((r[1] % mod) + mod) % mod
    )

    -- ---------------------------------------------------------- 斐波那契（快速倍增 O(log n)）
    [[static]]
    fib_pair(n)(
        -- 返回 (F(n), F(n+1))
        if n == 0( =(0, 1) )
        new p = Num.fib_pair(math.floor(n / 2))
        new a = p[0]
        new b = p[1]
        new c = a * (2 * b - a)
        new d = a * a + b * b
        if n % 2 == 0( =(c, d) )
        =(d, c + d)
    )

    [[static]]
    fib(n)( =Num.fib_pair(n)[0] )

    -- ---------------------------------------------------------- 组合数学
    [[static]]
    binomial(n, k)(
        if k < 0 || k > n( =0 )
        new kk = k
        if kk > n - k( kk = n - k )
        new result = 1
        for i in 1 to kk(
            result = result * (n - kk + i)
            result = result / i
        )
        =result
    )

    [[static]]
    pascal_row(n)(
        new out = []
        for k in 0 to n(
            out.push(Num.binomial(n, k))
        )
        =out
    )

    [[static]]
    pascal_triangle(rows)(
        new out = []
        for n in 0 to rows - 1(
            out.push(Num.pascal_row(n))
        )
        =out
    )

    [[static]]
    catalan(n)( =Num.binomial(2 * n, n) / (n + 1) )

    [[static]]
    triangular(n)( =n * (n + 1) / 2 )

    [[static]]
    is_triangular(n)(
        if n < 0( =false )
        new d = 8 * n + 1
        new s = Num.integer_sqrt(d)
        =s * s == d
    )

    -- ---------------------------------------------------------- 整数工具
    -- 牛顿迭代求整数平方根：O(log n)
    [[static]]
    integer_sqrt(n)(
        if n < 0( =null )
        if n < 2( =n )
        new x = n
        new y = math.floor((x + 1) / 2)
        while y < x(
            x = y
            y = math.floor((x + n / x) / 2)
        )
        =x
    )

    [[static]]
    is_perfect_square(n)(
        if n < 0( =false )
        new s = Num.integer_sqrt(n)
        =s * s == n
    )

    -- 数位（高位在前）
    [[static]]
    digits(n)(
        new v = math.abs(n)
        if v == 0( =[0] )
        new out = []
        while v > 0(
            out.push(v % 10)
            v = math.floor(v / 10)
        )
        =Seq.reverse(out)
    )

    [[static]]
    from_digits(ds)(
        new v = 0
        for d in ds(
            v = v * 10 + d
        )
        =v
    )

    [[static]]
    digit_sum(n)(
        new total = 0
        for d in Num.digits(n)(
            total = total + d
        )
        =total
    )

    [[static]]
    digital_root(n)(
        new v = math.abs(n)
        if v == 0( =0 )
        =1 + (v - 1) % 9
    )

    [[static]]
    reverse_number(n)(
        =Num.from_digits(Seq.reverse(Num.digits(n)))
    )

    [[static]]
    is_palindrome_number(n)(
        new v = math.abs(n)
        =v == Num.reverse_number(v)
    )

    [[static]]
    is_armstrong(n)(
        new ds = Num.digits(n)
        new k = len(ds)
        new total = 0
        for d in ds(
            total = total + Num.pow_int(d, k)
        )
        =total == math.abs(n)
    )

    [[static]]
    collatz_length(n)(
        new v = n
        new steps = 0
        while v != 1(
            if v % 2 == 0(
                v = v / 2
            ) else (
                v = 3 * v + 1
            )
            steps = steps + 1
        )
        =steps
    )

    [[static]]
    collatz_path(n)(
        new v = n
        new out = [v]
        while v != 1(
            if v % 2 == 0(
                v = v / 2
            ) else (
                v = 3 * v + 1
            )
            out.push(v)
        )
        =out
    )

    [[static]]
    longest_collatz(limit)(
        new best = 1
        new best_len = 0
        for n in 1 to limit(
            new l = Num.collatz_length(n)
            if l > best_len(
                best_len = l
                best = n
            )
        )
        =(best, best_len)
    )

    -- 进制转换
    [[static]]
    base_str(n, base)(
        if base < 2 || base > 36( =null )
        new alphabet = "0123456789abcdefghijklmnopqrstuvwxyz"
        new v = math.abs(n)
        if v == 0( ="0" )
        new out = ""
        while v > 0(
            new d = v % base
            out = alphabet[d] + out
            v = math.floor(v / base)
        )
        if n < 0( ="-" + out )
        =out
    )

    [[static]]
    parse_base(s, base)(
        new alphabet = "0123456789abcdefghijklmnopqrstuvwxyz"
        new v = 0
        new sign = 1
        new i = 0
        if len(s) > 0 && s[0] == "-"(
            sign = -1
            i = 1
        )
        while i < len(s)(
            new d = alphabet.index_of(s[i].lower())
            if d < 0 || d >= base( =null )
            v = v * base + d
            i = i + 1
        )
        =sign * v
    )

    -- 通用整数二分：返回 [lo, hi] 中第一个满足 pred 的值，没有则返回 null
    [[static]]
    binary_search_monotone(lo, hi, pred)(
        new left = lo
        new right = hi
        new answer = null
        while left <= right(
            new mid = math.floor((left + right) / 2)
            if pred(mid)(
                answer = mid
                right = mid - 1
            ) else (
                left = mid + 1
            )
        )
        =answer
    )

    -- 整数立方根等：牛顿迭代求 floor(n^(1/k))
    [[static]]
    integer_root(n, k)(
        if n < 0( =null )
        if n < 2( =n )
        new x = n
        while true(
            new y = math.floor(((k - 1) * x + n / Num.pow_int(x, k - 1)) / k)
            if y >= x( =x )
            x = y
        )
        =x
    )

    -- 汉诺塔：把移动序列写进 out，返回总步数（2^n - 1）
    [[static]]
    hanoi_into(n, from, to, via, out)(
        if n <= 0( =0 )
        Num.hanoi_into(n - 1, from, via, to, out)
        out.push((from, to))
        Num.hanoi_into(n - 1, via, to, from, out)
        =len(out)
    )

    [[static]]
    hanoi(n)(
        new out = []
        Num.hanoi_into(n, 1, 3, 2, out)
        =out
    )

    -- 枚举区间内所有素数之和（用筛法，O(n log log n)）
    [[static]]
    prime_sum(limit)(
        new total = 0
        for p in Num.sieve(limit)(
            total = total + p
        )
        =total
    )
)
