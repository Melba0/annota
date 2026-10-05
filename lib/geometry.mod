-[ geometry.mod : 平面几何（叉积定向、鞋带面积、Andrew 凸包、最近点对分治） ]-
-[ 凸包 O(n log n)；最近点对 O(n log n) 分治 + 带宽合并；点与多边形 O(n) 射线法。 ]-

[[module: geometry]]
[[version: 2.0]]
[[author: "annotateam"]]
[[macro_depth: 128]]

use math
use seq

Geo()=(
    -- ---------------------------------------------------------- 基础量
    [[static]]
    dist2(p, q)(
        new dx = p[0] - q[0]
        new dy = p[1] - q[1]
        =dx * dx + dy * dy
    )

    [[static]]
    dist(p, q)( =math.sqrt(float(Geo.dist2(p, q))) )

    [[static]]
    manhattan(p, q)( =math.abs(p[0] - q[0]) + math.abs(p[1] - q[1]) )

    [[static]]
    dot(u, v)( =u[0] * v[0] + u[1] * v[1] )

    -- 叉积 (a - o) × (b - o)：> 0 逆时针，< 0 顺时针，= 0 共线
    [[static]]
    cross(o, a, b)(
        =(a[0] - o[0]) * (b[1] - o[1]) - (a[1] - o[1]) * (b[0] - o[0])
    )

    [[static]]
    orientation(o, a, b)(
        new c = Geo.cross(o, a, b)
        if c > 0( =1 )
        if c < 0( =-1 )
        =0
    )

    [[static]]
    on_segment(p, q, r)(
        -- 已知三点共线时，r 是否落在线段 pq 上
        =r[0] <= math.max(p[0], q[0]) && r[0] >= math.min(p[0], q[0]) &&
         r[1] <= math.max(p[1], q[1]) && r[1] >= math.min(p[1], q[1])
    )

    [[static]]
    segments_intersect(p1, p2, p3, p4)(
        new d1 = Geo.orientation(p3, p4, p1)
        new d2 = Geo.orientation(p3, p4, p2)
        new d3 = Geo.orientation(p1, p2, p3)
        new d4 = Geo.orientation(p1, p2, p4)
        if d1 != d2 && d3 != d4( =true )
        if d1 == 0 && Geo.on_segment(p3, p4, p1)( =true )
        if d2 == 0 && Geo.on_segment(p3, p4, p2)( =true )
        if d3 == 0 && Geo.on_segment(p1, p2, p3)( =true )
        if d4 == 0 && Geo.on_segment(p1, p2, p4)( =true )
        =false
    )

    -- ---------------------------------------------------------- 多边形
    -- 鞋带公式：O(n) 求有向面积
    [[static]]
    polygon_area2(pts)(
        new n = len(pts)
        if n < 3( =0 )
        new acc = 0
        for i in 0 to n - 1(
            new j = (i + 1) % n
            acc = acc + pts[i][0] * pts[j][1] - pts[j][0] * pts[i][1]
        )
        =acc
    )

    [[static]]
    polygon_area(pts)( =float(math.abs(Geo.polygon_area2(pts))) / 2.0 )

    [[static]]
    polygon_perimeter(pts)(
        new n = len(pts)
        if n < 2( =0.0 )
        new total = 0.0
        for i in 0 to n - 1(
            total = total + Geo.dist(pts[i], pts[(i + 1) % n])
        )
        =total
    )

    [[static]]
    centroid(pts)(
        new n = len(pts)
        if n == 0( =null )
        new sx = 0
        new sy = 0
        for p in pts(
            sx = sx + p[0]
            sy = sy + p[1]
        )
        =(float(sx) / float(n), float(sy) / float(n))
    )

    [[static]]
    bounding_box(pts)(
        if len(pts) == 0( =null )
        new minx = pts[0][0]
        new maxx = pts[0][0]
        new miny = pts[0][1]
        new maxy = pts[0][1]
        for p in pts(
            if p[0] < minx( minx = p[0] )
            if p[0] > maxx( maxx = p[0] )
            if p[1] < miny( miny = p[1] )
            if p[1] > maxy( maxy = p[1] )
        )
        =(minx, miny, maxx, maxy)
    )

    -- 射线法：O(n)
    [[static]]
    point_in_polygon(pts, p)(
        new n = len(pts)
        if n < 3( =false )
        new inside = false
        new j = n - 1
        for i in 0 to n - 1(
            new a = pts[i]
            new b = pts[j]
            if (a[1] > p[1]) != (b[1] > p[1])(
                new x = float(a[0]) + float(p[1] - a[1]) / float(b[1] - a[1]) * float(b[0] - a[0])
                if float(p[0]) < x( inside = !inside )
            )
            j = i
        )
        =inside
    )

    -- ---------------------------------------------------------- 凸包（Andrew 单调链）
    [[static]]
    convex_hull(pts)(
        new ordered = Seq.merge_sort(pts, (p, q)(
            if p[0] == q[0]( =p[1] < q[1] )
            =p[0] < q[0]
        ))
        new sorted_pts = []
        for p in ordered(
            if len(sorted_pts) == 0 || sorted_pts[len(sorted_pts) - 1] != p(
                sorted_pts.push(p)
            )
        )
        new n = len(sorted_pts)
        if n < 3( =sorted_pts )
        new lower = []
        for p in sorted_pts(
            while len(lower) >= 2 && Geo.cross(lower[len(lower) - 2], lower[len(lower) - 1], p) <= 0(
                lower.pop()
            )
            lower.push(p)
        )
        new upper = []
        new i = n - 1
        while i >= 0(
            new p = sorted_pts[i]
            while len(upper) >= 2 && Geo.cross(upper[len(upper) - 2], upper[len(upper) - 1], p) <= 0(
                upper.pop()
            )
            upper.push(p)
            i = i - 1
        )
        lower.pop()
        upper.pop()
        =Seq.concat(lower, upper)
    )

    -- ---------------------------------------------------------- 最近点对（分治 O(n log n)）
    [[static]]
    closest_pair(pts)(
        new n = len(pts)
        if n < 2( =null )
        new byx = Seq.merge_sort(pts, (p, q)(
            if p[0] == q[0]( =p[1] < q[1] )
            =p[0] < q[0]
        ))
        new byy = Seq.merge_sort(pts, (p, q)(
            if p[1] == q[1]( =p[0] < q[0] )
            =p[1] < q[1]
        ))
        new best = Geo._closest(byx, byy)
        =math.sqrt(float(best))
    )

    [[static]]
    _closest(byx, byy)(
        new n = len(byx)
        if n <= 3(
            -- 小规模直接比较（常数大小，不影响总体复杂度）
            new best = -1
            for i in 0 to n - 1(
                for j in i + 1 to n - 1(
                    new d = Geo.dist2(byx[i], byx[j])
                    if best < 0 || d < best( best = d )
                )
            )
            =best
        )
        new mid = math.floor(n / 2)
        new midPoint = byx[mid]
        new leftX = Seq.slice_of(byx, 0, mid)
        new rightX = Seq.slice_of(byx, mid, n)
        new leftY = []
        new rightY = []
        for p in byy(
            if p[0] < midPoint[0] || (p[0] == midPoint[0] && p[1] <= midPoint[1])(
                leftY.push(p)
            ) else (
                rightY.push(p)
            )
        )
        new dl = Geo._closest(leftX, leftY)
        new dr = Geo._closest(rightX, rightY)
        new best = dl
        if dr < best( best = dr )
        new strip = []
        for p in byy(
            if float(Geo.dist2(p, midPoint)) < float(best)(
                strip.push(p)
            )
        )
        for i in 0 to len(strip) - 1(
            new j = i + 1
            while j < len(strip) && j <= i + 7(
                new d = Geo.dist2(strip[i], strip[j])
                if d < best( best = d )
                j = j + 1
            )
        )
        =best
    )

    -- 凸包周长与直径（旋转卡壳的简化版：凸包上两两比较，O(h^2)）
    [[static]]
    hull_perimeter(hull)( =Geo.polygon_perimeter(hull) )

    [[static]]
    hull_diameter(hull)(
        new h = len(hull)
        if h < 2( =0.0 )
        new best = 0.0
        for i in 0 to h - 1(
            for j in i + 1 to h - 1(
                new d = Geo.dist(hull[i], hull[j])
                if d > best( best = d )
            )
        )
        =best
    )
)
