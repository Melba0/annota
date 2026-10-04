-[ test.mod : 极简断言 / 测试框架（用 Test.* 静态成员保存统计） ]-

[[module: test]]
[[version: 1.0]]
[[author: "annotateam"]]
[[macro_depth: 64]]

Test()=(
    [[static]]
    total = 0

    [[static]]
    failed = 0

    [[static]]
    failures = []

    [[static]]
    subject = ""

    [[static]]
    grand_total = 0

    [[static]]
    grand_failed = 0

    [[static]]
    suite(name)(
        Test.subject = name
        Test.total = 0
        Test.failed = 0
        Test.failures = []
        print "== " + name + " =="
    )

    [[static]]
    ok(cond, msg = "condition")(
        Test.total = Test.total + 1
        Test.grand_total = Test.grand_total + 1
        if !cond(
            Test.failed = Test.failed + 1
            Test.grand_failed = Test.grand_failed + 1
            Test.failures.push(msg)
            print "  [FAIL] " + msg
        )
        =cond
    )

    [[static]]
    eq(actual, expected, msg = "")(
        new label = msg
        if label.size() == 0(
            label = "expected " + expected + ", got " + actual
        )
        =Test.ok(actual == expected, label)
    )

    [[static]]
    ne(a, b, msg = "values must differ") = Test.ok(a != b, msg)

    [[static]]
    near(actual, expected, tolerance = 0.000001, msg = "")(
        new label = msg
        if label.size() == 0(
            label = "expected " + expected + " +- " + tolerance + ", got " + actual
        )
        new diff = actual - expected
        if diff < 0( diff = -diff )
        =Test.ok(diff <= tolerance, label)
    )

    [[static]]
    raises(f, msg = "call must throw")(
        Test.total = Test.total + 1
        Test.grand_total = Test.grand_total + 1
        new threw = false
        (
            f()
            except e(
                threw = true
            )
        )
        if !threw(
            Test.failed = Test.failed + 1
            Test.grand_failed = Test.grand_failed + 1
            Test.failures.push(msg)
            print "  [FAIL] " + msg
        )
        =threw
    )

    [[static]]
    report()(
        new line = "-- " + Test.total + " checks in '" + Test.subject + "', " + Test.failed + " failures"
        if Test.grand_total != Test.total(
            line = line + " (all suites: " + Test.grand_total + " checks, " + Test.grand_failed + " failures)"
        )
        print line + " --"
        =Test.failed == 0
    )

    [[static]]
    check()(
        Test.report()
        if Test.failed > 0(
            for f in Test.failures(
                print "   * " + f
            )
        )
    )
)
