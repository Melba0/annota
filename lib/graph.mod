-[ graph.mod : 图（邻接表的 BFS/DFS、Dijkstra、拓扑排序、并查集、Kruskal、Floyd-Warshall） ]-
-[ 队列用数组 + 头指针，优先队列用二叉堆（键是整数距离），不做 O(V^2) 的暴力松弛。 ]-

[[module: graph]]
[[version: 2.0]]
[[author: "annotateam"]]
[[macro_depth: 128]]

use math
use seq
use dict

-- 最小堆（按 (距离, 顶点) 对的第一个分量比较）。
-- 堆数组放在实例字段里：Annota 的实参按值传递，把数组当参数改是改不动调用者的，
-- 早先那版 _push(heap, ...) / _pop(heap) 就是靠深拷贝失效才"能跑"，这里改成实例方法。
_Heap()=(
    data:List = []

    size() = len(data)

    is_empty() = len(data) == 0

    push(item)(
        data.push(item)
        new i = len(data) - 1
        while i > 0(
            new parent = math.floor((i - 1) / 2)
            if data[parent][0] <= data[i][0]( break )
            new t = data[parent]
            data[parent] = data[i]
            data[i] = t
            i = parent
        )
        =item
    )

    pop()(
        new top = data[0]
        new last = data.pop()
        if len(data) > 0(
            data[0] = last
            new i = 0
            while true(
                new l = i * 2 + 1
                new r = i * 2 + 2
                new small = i
                if l < len(data) && data[l][0] < data[small][0]( small = l )
                if r < len(data) && data[r][0] < data[small][0]( small = r )
                if small == i( break )
                new t = data[i]
                data[i] = data[small]
                data[small] = t
                i = small
            )
        )
        =top
    )
)

Graph(directed = false)=(
    _adj = Dict()                 -- 顶点 -> [(邻居, 权重), ...]
    _directed:bool = false
    _edge_list:List = []          -- [(u, v, w), ...]

    __init__()(
        _directed = directed
    )

    -- ---------------------------------------------------------- 构建
    add_vertex(v)(
        if !_adj.has(v)(
            _adj.set(v, [])
        )
        =v
    )

    add_edge(u, v, w = 1)(
        add_vertex(u)
        add_vertex(v)
        -- _adj.get 返回的是副本（Annota 的值语义：赋值/返回都会深拷贝），
        -- 所以就地改完必须写回，否则边不会进入邻接表
        new au = _adj.get(u)
        au.push((v, w))
        _adj.set(u, au)
        if !_directed(
            new av = _adj.get(v)
            av.push((u, w))
            _adj.set(v, av)
        )
        _edge_list.push((u, v, w))
        =w
    )

    vertices() = _adj.keys_sorted()

    vertex_count() = _adj.size()

    edge_count() = len(_edge_list)

    edges() = _edge_list

    neighbors(v)(
        if !_adj.has(v)( =[] )
        new out = []
        for p in _adj.get(v)(
            out.push(p[0])
        )
        =out
    )

    weighted_neighbors(v)(
        if !_adj.has(v)( =[] )
        =_adj.get(v)
    )

    degree(v)( =len(neighbors(v)) )

    has_edge(u, v)( =Seq.contains(neighbors(u), v) )

    weight_of(u, v)(
        for p in weighted_neighbors(u)(
            if p[0] == v( =p[1] )
        )
        =null
    )

    copy()(
        new g = Graph(_directed)
        for e in _edge_list(
            g.add_edge(e[0], e[1], e[2])
        )
        =g
    )

    [[static]]
    from_edges(edge_list, directed = false)(
        new g = Graph(directed)
        for e in edge_list(
            if len(e) >= 3(
                g.add_edge(e[0], e[1], e[2])
            ) else (
                g.add_edge(e[0], e[1])
            )
        )
        =g
    )

    -- ---------------------------------------------------------- 遍历
    bfs(start)(
        new order = []
        if !_adj.has(start)( =order )
        new seen = Set([start])
        new queue = [start]
        new head = 0
        while head < len(queue)(
            new v = queue[head]
            head = head + 1
            order.push(v)
            for u in neighbors(v)(
                if !seen.has(u)(
                    seen.add(u)
                    queue.push(u)
                )
            )
        )
        =order
    )

    -- 深度优先（显式栈，前序）
    dfs(start)(
        new order = []
        if !_adj.has(start)( =order )
        new seen = Set()
        new stack = [start]
        while len(stack) > 0(
            new v = stack.pop()
            if seen.has(v)( continue )
            seen.add(v)
            order.push(v)
            new ns = neighbors(v)
            new i = len(ns) - 1
            while i >= 0(
                if !seen.has(ns[i])(
                    stack.push(ns[i])
                )
                i = i - 1
            )
        )
        =order
    )

    has_path(a, b)( =Seq.contains(bfs(a), b) )

    -- ---------------------------------------------------------- 最短路
    -- Dijkstra（非负权）：二叉堆 + 索引编码，O((V + E) log V)
    distances(start)(
        new dist = Dict()
        for v in vertices()(
            dist.set(v, -1)
        )
        if !_adj.has(start)( =dist )
        dist.set(start, 0)
        new names = vertices()
        new index_of = Dict()
        for (i, v) in enumerate(names)(
            index_of.set(v, i)
        )
        new n = len(names)
        new heap = _Heap()                  -- [(距离, 下标)]，最小堆
        heap.push((0, index_of.get(start)))
        new done = Set()
        while !heap.is_empty()(
            new top = heap.pop()
            new d = top[0]
            new v = names[top[1]]
            if done.has(v)( continue )
            done.add(v)
            for p in weighted_neighbors(v)(
                new u = p[0]
                new w = p[1]
                new nd = d + w
                new cur = dist.get(u, -1)
                if cur < 0 || nd < cur(
                    dist.set(u, nd)
                    heap.push((nd, index_of.get(u)))
                )
            )
        )
        =dist
    )

    -- 单源最短路 + 路径回溯
    shortest_path(a, b)(
        new dist = Dict()
        new prev = Dict()
        for v in vertices()(
            dist.set(v, -1)
        )
        if !_adj.has(a)( =null )
        dist.set(a, 0)
        new heap = _Heap()
        heap.push((0, a))
        new done = Set()
        while !heap.is_empty()(
            new top = heap.pop()
            new d = top[0]
            new v = top[1]
            if done.has(v)( continue )
            done.add(v)
            for p in weighted_neighbors(v)(
                new u = p[0]
                new w = p[1]
                new nd = d + w
                new cur = dist.get(u, -1)
                if cur < 0 || nd < cur(
                    dist.set(u, nd)
                    prev.set(u, v)
                    heap.push((nd, u))
                )
            )
        )
        new target = dist.get(b, -1)
        if target < 0( =null )
        new path = [b]
        new cur = b
        while cur != a(
            new p = prev.get(cur, null)
            if p == null( =null )
            cur = p
            path.push(cur)
        )
        =(target, Seq.reverse(path))
    )

    -- Floyd–Warshall：O(V^3) 动态规划，返回 (顶点表, 距离矩阵)
    all_pairs()(
        new names = vertices()
        new n = len(names)
        new at = Dict()
        for (i, v) in enumerate(names)(
            at.set(v, i)
        )
        new inf = 1000000000
        new d = []
        for i in 0 to n - 1(
            new row = []
            for j in 0 to n - 1(
                if i == j( row.push(0) ) else ( row.push(inf) )
            )
            d.push(row)
        )
        for e in _edge_list(
            new i = at.get(e[0])
            new j = at.get(e[1])
            new w = e[2]
            if w < d[i][j](
                d[i][j] = w
                if !_directed( d[j][i] = w )
            )
        )
        for k in 0 to n - 1(
            for i in 0 to n - 1(
                if d[i][k] < inf(
                    for j in 0 to n - 1(
                        new cand = d[i][k] + d[k][j]
                        if cand < d[i][j](
                            d[i][j] = cand
                        )
                    )
                )
            )
        )
        =(names, d)
    )

    -- ---------------------------------------------------------- 结构与排序
    connected_components()(
        new out = []
        new seen = Set()
        for v in vertices()(
            if seen.has(v)( continue )
            new comp = bfs(v)
            for u in comp( seen.add(u) )
            out.push(comp)
        )
        =out
    )

    -- Kahn 拓扑排序：O(V + E)
    topological_sort()(
        new indeg = Dict()
        for v in vertices()(
            indeg.set(v, 0)
        )
        for v in vertices()(
            for u in neighbors(v)(
                indeg.set(u, indeg.get(u, 0) + 1)
            )
        )
        new queue = []
        for v in vertices()(
            if indeg.get(v, 0) == 0(
                queue.push(v)
            )
        )
        new order = []
        new head = 0
        while head < len(queue)(
            new v = queue[head]
            head = head + 1
            order.push(v)
            for u in neighbors(v)(
                indeg.set(u, indeg.get(u, 0) - 1)
                if indeg.get(u, 0) == 0(
                    queue.push(u)
                )
            )
        )
        if len(order) != vertex_count()( =null )    -- 有环
        =order
    )

    has_cycle()(
        if !_directed(
            -- 无向图：并查集，边连接同一集合即有环
            new names = vertices()
            new at = Dict()
            for (i, v) in enumerate(names)(
                at.set(v, i)
            )
            new dsu = DSU(len(names))
            for e in _edge_list(
                if !dsu.union(at.get(e[0]), at.get(e[1]))( =true )
            )
            =false
        )
        -- 有向图：三色 DFS
        new color = Dict()
        for v in vertices()(
            color.set(v, 0)
        )
        new stack = []
        for v in vertices()(
            if color.get(v) != 0( continue )
            stack.push((v, 0))
            while len(stack) > 0(
                new top = stack[len(stack) - 1]
                new node = top[0]
                new phase = top[1]
                if phase == 0(
                    color.set(node, 1)
                    stack[len(stack) - 1] = (node, 1)
                    for u in neighbors(node)(
                        if color.get(u) == 1( =true )
                        if color.get(u) == 0(
                            stack.push((u, 0))
                        )
                    )
                ) else (
                    color.set(node, 2)
                    stack.pop()
                )
            )
        )
        =false
    )

    is_bipartite()(
        new color = Dict()
        for v in vertices()(
            color.set(v, -1)
        )
        for start in vertices()(
            if color.get(start) != -1( continue )
            color.set(start, 0)
            new queue = [start]
            new head = 0
            while head < len(queue)(
                new v = queue[head]
                head = head + 1
                for u in neighbors(v)(
                    if color.get(u) == -1(
                        color.set(u, 1 - color.get(v))
                        queue.push(u)
                    ) elif color.get(u) == color.get(v)(
                        =false
                    )
                )
            )
        )
        =true
    )

    __len__() = _adj.size()

    __str__() = "Graph(" + str(vertex_count()) + " vertices, " + str(edge_count()) + " edges)"

    -- ---------------------------------------------------------- 堆（(距离, 顶点) 对）
    [[static]]

    [[static]]

    -- 最小生成树：Kruskal（按权重排序 + 并查集）O(E log E)
    [[static]]
    kruskal(n, edge_list)(
        new ordered = Seq.merge_sort(edge_list, (p, q)( =p[2] < q[2] ))
        new dsu = DSU(n)
        new chosen = []
        new total = 0
        for e in ordered(
            if dsu.union(e[0], e[1])(
                chosen.push(e)
                total = total + e[2]
            )
        )
        =(total, chosen)
    )
)

-[ DSU : 并查集（路径压缩 + 按秩合并），近似 O(1) ]-
DSU(n)=(
    _parent:List = []
    _rank:List = []
    _count:int = 0

    __init__()(
        _parent = []
        _rank = []
        for i in 0 to n - 1(
            _parent.push(i)
            _rank.push(0)
        )
        _count = n
    )

    find(x)(
        new root = x
        while _parent[root] != root(
            root = _parent[root]
        )
        new cur = x
        while _parent[cur] != root(          -- 路径压缩
            new next = _parent[cur]
            _parent[cur] = root
            cur = next
        )
        =root
    )

    union(a, b)(
        new ra = find(a)
        new rb = find(b)
        if ra == rb( =false )
        if _rank[ra] < _rank[rb](
            _parent[ra] = rb
        ) elif _rank[ra] > _rank[rb](
            _parent[rb] = ra
        ) else (
            _parent[rb] = ra
            _rank[ra] = _rank[ra] + 1
        )
        _count = _count - 1
        =true
    )

    connected(a, b)( =find(a) == find(b) )

    count() = _count

    groups()(
        new by_root = Dict()
        for i in 0 to len(_parent) - 1(
            new r = find(i)
            if !by_root.has(r)(
                by_root.set(r, [])
            )
            new g = by_root.get(r)
            g.push(i)
            by_root.set(r, g)
        )
        =by_root.values_list()
    )
)
