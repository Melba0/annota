-[ mathx.mod : 常用数学工具（原生 math 模块的补充） ]-

[[module: mathx]]
[[version: 1.0]]
[[author: "annotateam"]]
[[macro_depth: 64]]

use math

Mathx()=(
    [[static]]
    gcd(a, b)(
        new x = a
        new y = b
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
        =a / Mathx.gcd(a, b) * b
    )

    [[static]]
    factorial(n)(
        if n <= 1( =1 )
        =n * Mathx.factorial(n - 1)
    )

    [[static]]
    fib(n)(
        if n < 2( =n )
        =Mathx.fib(n - 1) + Mathx.fib(n - 2)
    )

    [[static]]
    fib_iter(n)(
        new a = 0
        new b = 1
        for i in 1 to n(
            new t = a + b
            a = b
            b = t
        )
        =a
    )

    [[static]]
    is_prime(n)(
        if n < 2( =false )
        if n < 4( =true )
        if n % 2 == 0( =false )
        new i = 3
        while i * i <= n(
            if n % i == 0( =false )
            i = i + 2
        )
        =true
    )

    [[static]]
    primes(limit)(
        new out = []
        for n in 2 to limit - 1(
            if Mathx.is_prime(n)(
                out.push(n)
            )
        )
        =out
    )

    [[static]]
    clamp(x, lo, hi)(
        if x < lo( =lo )
        if x > hi( =hi )
        =x
    )

    [[static]]
    lerp(a, b, t) = a + (b - a) * t

    [[static]]
    round_to(x, digits)(
        new f = 10 ** digits
        =float(math.round(x * f)) / float(f)      -- 显式浮点除法（int/int 现在向零截断）
    )

    [[static]]
    mean(xs)(
        if len(xs) == 0( =null )
        =float(sum(xs)) / float(len(xs))
    )

    [[static]]
    median(xs)(
        new s = sorted(xs)
        new n = len(s)
        if n == 0( =null )
        if n % 2 == 1(
            =s[math.floor((n - 1) / 2)]
        )
        =float(s[n / 2 - 1] + s[n / 2]) / 2.0
    )

    [[static]]
    variance(xs)(
        new m = Mathx.mean(xs)
        new acc = 0.0
        for x in xs(
            acc = acc + (x - m) * (x - m)
        )
        =float(acc) / float(len(xs))
    )

    [[static]]
    stddev(xs) = math.sqrt(Mathx.variance(xs))

    [[static]]
    is_even(n) = n % 2 == 0

    [[static]]
    is_odd(n) = n % 2 != 0

    [[static]]
    digit_sum(n)(
        new v = math.abs(n)
        new s = 0
        while v > 0(
            s = s + v % 10
            v = math.floor(v / 10)
        )
        =s
    )
)
