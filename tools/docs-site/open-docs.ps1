# Opens the FatzAudio documentation in your browser through a tiny local web server (browsers block the
# site's menu and search when pages are opened straight from disk). Needs only Windows PowerShell. Ctrl+C stops it.
$root = Split-Path -Parent $MyInvocation.MyCommand.Path
$port = 8765
$http = New-Object System.Net.HttpListener
$http.Prefixes.Add("http://localhost:$port/")
$http.Start()
Start-Process "http://localhost:$port/index.html"
$types = @{ ".html"="text/html"; ".css"="text/css"; ".js"="text/javascript"; ".json"="application/json"; ".svg"="image/svg+xml"; ".png"="image/png"; ".ico"="image/x-icon"; ".woff2"="font/woff2"; ".yml"="text/plain" }
while ($http.IsListening) {
    $ctx = $http.GetContext()
    $rel = [Uri]::UnescapeDataString($ctx.Request.Url.AbsolutePath).TrimStart("/")
    if ($rel -eq "") { $rel = "index.html" }
    $file = Join-Path $root $rel
    if ((Test-Path $file -PathType Leaf) -and ($file.StartsWith($root))) {
        $ext = [IO.Path]::GetExtension($file).ToLower()
        $ctx.Response.ContentType = if ($types.ContainsKey($ext)) { $types[$ext] } else { "application/octet-stream" }
        $bytes = [IO.File]::ReadAllBytes($file)
        $ctx.Response.OutputStream.Write($bytes, 0, $bytes.Length)
    } else { $ctx.Response.StatusCode = 404 }
    $ctx.Response.Close()
}
