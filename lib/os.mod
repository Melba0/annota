-[ os.mod : 平台、环境变量与外部命令（封装 _sys_info / _sys_env* / _sys_exec） ]-

[[module: os]]
[[version: 1.0]]

Os()=(
    [[static]]
    platform() = _sys_info("platform")

    [[static]]
    arch() = _sys_info("arch")

    [[static]]
    cpus() = _sys_info("cpus")

    [[static]]
    pid() = _sys_info("pid")

    [[static]]
    home() = _sys_info("home")

    [[static]]
    temp() = _sys_info("temp")

    [[static]]
    cwd() = _sys_info("cwd")

    [[static]]
    is_windows() = Os.platform() == "windows"
    [[static]]
    is_linux() = Os.platform() == "linux"
    [[static]]
    is_macos() = Os.platform() == "macos"

    [[static]]
    env(name, fallback = null)(
        new v = _sys_env(name)
        if v == null( =fallback )
        =v
    )

    [[static]]
    set_env(name, value) = _sys_env_set(name, value)

    [[static]]
    env_all()(
        -- 转成 [[名字, 值], ...]，并按名字排序，方便查看与断言
        new all = _sys_env_all()
        new sorted = []
        for pair in all( sorted.push(pair) )
        new i = 1
        while i < len(sorted)(
            new j = i
            while j > 0 && sorted[j - 1][0] > sorted[j][0](
                new tmp = sorted[j - 1]
                sorted[j - 1] = sorted[j]
                sorted[j] = tmp
                j = j - 1
            )
            i = i + 1
        )
        =sorted
    )

    [[static]]
    env_names()(
        new out = []
        for pair in Os.env_all( out.push(pair[0]) )
        =out
    )

    [[static]]
    exec(cmd)(
        -- 返回 [退出码, 输出]，输出包含标准输出与标准错误
        =_sys_exec(cmd)
    )

    [[static]]
    run(cmd)(
        -- 只关心输出
        new r = Os.exec(cmd)
        =r[1]
    )

    [[static]]
    ok(cmd)(
        new r = Os.exec(cmd)
        =r[0] == 0
    )

    [[static]]
    which(name)(
        new cmd = "where " + name
        if !Os.is_windows( cmd = "command -v " + name )
        new r = Os.exec(cmd)
        if r[0] != 0( ="" )
        new text = r[1].trim()
        new nl = text.find("\n")
        if nl >= 0( =text.substr(0, nl).trim() )
        =text
    )

    [[static]]
    describe()(
        =Os.platform() + "/" + Os.arch() + "  " + str(Os.cpus()) + " cpus  pid " + str(Os.pid())
    )
)
