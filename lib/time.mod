-[ time.mod : 时间与日期，全部由 _sys_* 原语实现 ]-
-[ 时钟原语：_sys_clock() 单调毫秒，_sys_time() 墙上毫秒 ]-
-[ 也可以直接用原生 time 模块：time.now() / time.millis() / time.clock() / time.sleep() ]-

[[module: time]]
[[version: 1.0]]

Time()=(
    -- ---------- 时钟（毫秒）
    [[static]]
    millis() = _sys_time()

    [[static]]
    seconds() = _sys_time() / 1000

    [[static]]
    clock() = _sys_clock()

    [[static]]
    sleep(ms)(
        _sys_sleep(ms)
        =Time.clock()
    )

    -- ---------- 拆解与构造
    [[static]]
    parts(ms = null)(
        if ms == null( =_sys_local_time() )
        =_sys_local_time(ms)
    )

    [[static]]
    year(ms = null) = Time.parts(ms)[0]
    [[static]]
    month(ms = null) = Time.parts(ms)[1]
    [[static]]
    day(ms = null) = Time.parts(ms)[2]
    [[static]]
    hour(ms = null) = Time.parts(ms)[3]
    [[static]]
    minute(ms = null) = Time.parts(ms)[4]
    [[static]]
    second(ms = null) = Time.parts(ms)[5]
    [[static]]
    weekday(ms = null) = Time.parts(ms)[6]

    [[static]]
    make(y, mo, d, h = 0, mi = 0, s = 0) = _sys_make_time(y, mo, d, h, mi, s)

    [[static]]
    today()(
        new p = Time.parts()
        =Time.make(p[0], p[1], p[2])
    )

    -- ---------- 格式化（strftime 风格由本模块自己实现，见 format）
    [[static]]
    pad(n, width = 2)(
        new s = str(n)
        while len(s) < width( s = "0" + s )
        =s
    )

    [[static]]
    format(fmt, ms = null, ampm = false)(
        new p = Time.parts(ms)
        new y = p[0]
        new mo = p[1]
        new d = p[2]
        new h = p[3]
        new mi = p[4]
        new s = p[5]
        new h12 = h % 12
        if h12 == 0( h12 = 12 )
        new out = ""
        new i = 0
        while i < len(fmt)(
            new c = fmt[i]
            if c == "%" && i + 1 < len(fmt)(
                new k = fmt[i + 1]
                i = i + 2
                if k == "Y"( out = out + str(y) ) else if k == "m"( out = out + Time.pad(mo) ) else if k == "d"( out = out + Time.pad(d) ) else if k == "H"( out = out + Time.pad(h) ) else if k == "M"( out = out + Time.pad(mi) ) else if k == "S"( out = out + Time.pad(s) ) else if k == "I"( out = out + Time.pad(h12) ) else if k == "p"( if h < 12( out = out + "AM" ) else ( out = out + "PM" ) ) else if k == "y"( out = out + Time.pad(y % 100) ) else if k == "j"(
                    new doy = Time.day_of_year(ms)
                    out = out + Time.pad(doy, 3)
                ) else if k == "%"( out = out + "%" ) else( out = out + k )
                continue
            )
            out = out + c
            i = i + 1
        )
        =out
    )

    [[static]]
    iso(ms = null) = Time.format("%Y-%m-%dT%H:%M:%S", ms)
    [[static]]
    date(ms = null) = Time.format("%Y-%m-%d", ms)
    [[static]]
    clock_text(ms = null) = Time.format("%H:%M:%S", ms)
    [[static]]
    stamp_text(ms = null) = Time.format("%Y-%m-%d %H:%M:%S", ms)

    [[static]]
    day_of_year(ms = null)(
        new p = Time.parts(ms)
        new y = p[0]
        new total = p[2]
        new m = 1
        while m < p[1](
            total = total + Time.days_in_month(y, m)
            m = m + 1
        )
        =total
    )

    [[static]]
    leap(y)(
        if y % 400 == 0( =true )
        if y % 100 == 0( =false )
        =y % 4 == 0
    )

    [[static]]
    days_in_month(y, m)(
        if m == 2(
            if Time.leap(y)( =29 )
            =28
        )
        if m == 4 || m == 6 || m == 9 || m == 11( =30 )
        =31
    )

    -- ---------- 差值
    [[static]]
    diff_ms(a, b) = a - b
    [[static]]
    diff_seconds(a, b) = (a - b) / 1000

    [[static]]
    describe_ms(ms)(
        -- 把毫秒数写成 12 ms / 1.234 s 这种可读形式
        new sign = ""
        new v = ms
        if v < 0(
            sign = "-"
            v = 0 - v
        )
        if v < 1000( =sign + str(v) + " ms" )
        new whole = int(v / 1000)
        new frac = v % 1000
        =sign + str(whole) + "." + Time.pad(frac, 3) + " s"
    )
)

-[ 计时器：measure() 之间累计耗时 ]-
Stopwatch()=(
    started = _sys_clock()
    laps:List = []

    elapsed() = _sys_clock() - started

    lap()(
        new now = _sys_clock()
        new delta = now - started
        laps.push(delta)
        started = now
        =delta
    )

    reset()(
        started = _sys_clock()
        laps = []
        =true
    )

    __str__() = "Stopwatch(" + Time.describe_ms(elapsed()) + ", " + str(len(laps)) + " laps)"
)
