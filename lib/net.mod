-[ net.mod : TCP 客户端 / 服务端与 HTTP，全部基于 _sys_socket 原语 ]-
-[ 原语：_sys_socket("resolve"|"connect"|"listen"|"accept"|"send"|"recv"|"close"|"shutdown"|"timeout", ...) ]-
-[ 本模块不再需要 C++ 参与：HTTP 报文解析、URL 编码、响应对象都在这里实现。 ]-

[[module: net]]
[[version: 1.0]]

Url()=(
    [[static]]
    is_safe(ch)(
        new c = ord(ch)
        if c >= 48 && c <= 57( =true )
        if c >= 65 && c <= 90( =true )
        if c >= 97 && c <= 122( =true )
        if ch == "-" || ch == "_" || ch == "." || ch == "~"( =true )
        =false
    )

    [[static]]
    encode(s)(
        new hex = "0123456789ABCDEF"
        new out = ""
        for ch in s(
            if Url.is_safe(ch)(
                out = out + ch
            ) else (
                new c = ord(ch)
                out = out + "%" + hex[int(c / 16)] + hex[c % 16]
            )
        )
        =out
    )

    [[static]]
    decode(s)(
        new out = ""
        new i = 0
        while i < len(s)(
            new c = s[i]
            if c == "%" && i + 2 < len(s)(
                out = out + chr(Url.hex_val(s[i + 1]) * 16 + Url.hex_val(s[i + 2]))
                i = i + 3
                continue
            )
            if c == "+"( out = out + " " ) else( out = out + c )
            i = i + 1
        )
        =out
    )

    [[static]]
    hex_val(c)(
        new n = ord(c)
        if n >= 48 && n <= 57( =n - 48 )
        if n >= 65 && n <= 70( =n - 55 )
        if n >= 97 && n <= 102( =n - 87 )
        =0
    )

    [[static]]
    parse(url)(
        -- 返回 [scheme, host, port, path]
        new p = url.find("://")
        if p < 0( throw "url 需要形如 http://host/path" )
        new scheme = url.substr(0, p)
        new rest = url.substr(p + 3, len(url))
        new slash = rest.find("/")
        new authority = rest
        new path = "/"
        if slash >= 0(
            authority = rest.substr(0, slash)
            path = rest.substr(slash, len(rest))
        )
        new host = authority
        new port = 80
        if scheme == "https"( port = 443 )
        new colon = authority.find(":")
        if colon >= 0(
            host = authority.substr(0, colon)
            port = int(authority.substr(colon + 1, len(authority)))
        )
        =[scheme, host, port, path]
    )

    [[static]]
    join(base, path)(
        if base.ends_with("/") && path.starts_with("/")( =base.substr(0, len(base) - 1) + path )
        if !base.ends_with("/") && !path.starts_with("/")( =base + "/" + path )
        =base + path
    )
)

Response()=(
    status = 0
    reason = ""
    headers = null
    body = ""
    raw = ""

    header(name, fallback = "")(
        if headers == null( =fallback )
        new key = name.lower()
        for pair in headers(
            if pair[0].lower() == key( =pair[1] )
        )
        =fallback
    )

    ok() = status >= 200 && status < 300

    json()(
        use json
        =json.parse(body)
    )

    __str__() = "HTTP " + str(status) + " " + reason + " (" + str(len(body)) + " bytes)"
)

Tcp()=(
    handle = 0

    [[static]]
    connect(host, port)(
        new t = Tcp()
        t.handle = _sys_socket("connect", host, port)
        =t
    )

    [[static]]
    listen(host, port)(
        new t = Tcp()
        t.handle = _sys_socket("listen", host, port)
        =t
    )

    accept()(
        new t = Tcp()
        t.handle = _sys_socket("accept", handle)
        =t
    )

    send(text) = _sys_socket("send", handle, text)

    send_line(text) = _sys_socket("send", handle, text + "\r\n")

    recv(max = 4096) = _sys_socket("recv", handle, max)

    recv_all(timeout_ms = 500, chunk = 4096)(
        _sys_socket("timeout", handle, timeout_ms)
        new out = ""
        while true(
            new part = _sys_socket("recv", handle, chunk)
            if part == ""( break )
            out = out + part
        )
        =out
    )

    recv_until(marker, timeout_ms = 2000, chunk = 1024)(
        _sys_socket("timeout", handle, timeout_ms)
        new out = ""
        while out.find(marker) < 0(
            new part = _sys_socket("recv", handle, chunk)
            if part == ""( break )
            out = out + part
        )
        =out
    )

    timeout(ms) = _sys_socket("timeout", handle, ms)

    close() = _sys_socket("close", handle)

    __str__() = "Tcp(handle " + str(handle) + ")"
)

Net()=(
    [[static]]
    available() = true

    [[static]]
    resolve(host) = _sys_socket("resolve", host)

    [[static]]
    tcp(host, port) = Tcp.connect(host, port)

    [[static]]
    server(host, port) = Tcp.listen(host, port)

    [[static]]
    parse_response(text)( =Net.parse_http(text) )

    [[static]]
    parse_http(text)(
        new r = Response()
        r.raw = text
        new head_end = text.find("\r\n\r\n")
        new head = text
        if head_end >= 0(
            head = text.substr(0, head_end)
            r.body = text.substr(head_end + 4, len(text))
        )
        new lines = head.split("\r\n")
        if len(lines) > 0(
            new status_line = lines[0].split(" ")
            if len(status_line) >= 2( r.status = int(status_line[1]) )
            if len(status_line) >= 3( r.reason = status_line[2] )
        )
        new headers = []
        new i = 1
        while i < len(lines)(
            new line = lines[i]
            new colon = line.find(":")
            if colon > 0(
                headers.push([line.substr(0, colon).trim(), line.substr(colon + 1, len(line)).trim()])
            )
            i = i + 1
        )
        r.headers = headers
        =r
    )

    [[static]]
    request(method, url, body = null, headers = [])(
        -- 只支持明文 http://（https 需要 TLS，本构建不带）
        new parts = Url.parse(url)
        new scheme = parts[0]
        if scheme == "https"(
            throw "https 需要 TLS 支持，本构建不包含；请改用 http:// 或原生模块"
        )
        new host = parts[1]
        new port = parts[2]
        new path = parts[3]
        new conn = Tcp.connect(host, port)
        new head = method + " " + path + " HTTP/1.1\r\n"
        head = head + "Host: " + host + "\r\n"
        head = head + "User-Agent: annota/1.0\r\n"
        head = head + "Accept: */*\r\n"
        head = head + "Connection: close\r\n"
        for h in headers(
            head = head + h[0] + ": " + h[1] + "\r\n"
        )
        if body != null(
            head = head + "Content-Length: " + str(len(body)) + "\r\n"
        )
        head = head + "\r\n"
        conn.send(head)
        if body != null( conn.send(body) )
        new raw = conn.recv_all(800)
        conn.close()
        =Net.parse_http(raw)
    )

    [[static]]
    get(url) = Net.request("GET", url)

    [[static]]
    body_of(url) = Net.get(url).body

    [[static]]
    post(url, body, headers = []) = Net.request("POST", url, body, headers)

    [[static]]
    encode(s) = Url.encode(s)

    [[static]]
    decode(s) = Url.decode(s)
)
