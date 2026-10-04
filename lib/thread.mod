-[ thread.mod : 线程与并行，基于 _sys_spawn / _sys_join 原语 ]-
-[ 每个工作线程有独立的 VM，全局变量是深拷贝；闭包捕获的单元仍然共享， ]-
-[ 因此工作函数里不要修改捕获到的可变状态（可以用 [[pure]] 让分析器帮忙盯住）。 ]-

[[module: thread]]
[[version: 1.0]]

Task()=(
    handle = 0
    cached = null
    has_cached = false

    done() = _sys_task_done(handle)

    wait()(
        if !has_cached(
            cached = _sys_join(handle)
            has_cached = true
        )
        =cached
    )

    value() = wait()

    __str__()(
        if has_cached( ="Task(done, " + str(cached) + ")" )
        if done( ="Task(finished)" )
        ="Task(running)"
    )
)

Thread()=(
    [[static]]
    hardware() = _sys_info("cpus")

    [[static]]
    sleep(ms)(
        _sys_sleep(ms)
        =null
    )

    [[static]]
    spawn(fn, args = [])(
        new t = Task()
        t.handle = _sys_spawn(fn, args)
        =t
    )

    [[static]]
    run(fn, args = [])(
        -- 起线程、等结果、返回
        =Thread.spawn(fn, args).wait()
    )

    [[static]]
    parallel_map(fn, xs, workers = 0)(
        -- 把 fn 并行作用到 xs 的每个元素上，结果顺序与输入一致
        if len(xs) == 0( =[] )
        if workers <= 0( workers = Thread.hardware() )
        new out = []
        new tasks = []
        for x in xs(
            tasks.push(Thread.spawn(fn, [x]))
            -- 每攒够一批就先收，避免一次性开太多线程
            if len(tasks) >= workers(
                for t in tasks( out.push(t.wait()) )
                tasks = []
            )
        )
        for t in tasks( out.push(t.wait()) )
        =out
    )

    [[static]]
    collect(tasks)(
        new out = []
        for t in tasks( out.push(t.wait()) )
        =out
    )

    [[static]]
    parallel_each(fn, xs, workers = 0)(
        new results = Thread.parallel_map(fn, xs, workers)
        =len(results)
    )

    [[static]]
    pool(size = 0)(
        -- 简单线程池：提交任务后统一 collect
        new p = Pool()
        if size > 0( p.size = size )
        =p
    )
)

Pool()=(
    size = 0
    pending:List = []
    finished:List = []

    submit(fn, args = [])(
        new t = Task()
        t.handle = _sys_spawn(fn, args)
        pending.push(t)
        if len(pending) >= size && size > 0(
            finished.push(pending[0].wait())
            pending.pop()
        )
        =t
    )

    drain()(
        for t in pending(
            finished.push(t.wait())
        )
        pending = []
        =finished
    )
)
