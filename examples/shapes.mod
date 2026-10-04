-[ shapes.mod : a user supplied module (文档 13.4 / 13.5) ]-

[[module: shapes]]
[[version: 1.0]]
[[author: "annotateam"]]
[[macro_depth: 64]]

Shape(name)=(
    name:String

    area() = 0

    describe() = name + " with area " + area()

    __str__() = name + "(" + area() + ")"
)

Rect(w, h)=(
    :Shape("rect")
    w:int
    h:int

    area() = w * h
)

Circle(r)=(
    :Shape("circle")
    r:float

    area() = 3.14159 * r * r
)

-- a module may also register new annotations (文档 14.11)
macro [[bounds: $cond]](
    [[assert: $cond]]
)
macro [[shapes_version: $v]](
    [[version: $v]]
)
