param(
    [Parameter(Mandatory = $true)]
    [string]$ServerExe,

    [Parameter(Mandatory = $true)]
    [string]$ClientExe
)

$ErrorActionPreference = "Stop"
$port = Get-Random -Minimum 20000 -Maximum 50000
$serverArgs = @("--storage", "memory", "--host", "127.0.0.1", "--port", "$port")
$server = Start-Process -FilePath $ServerExe -ArgumentList $serverArgs -PassThru -WindowStyle Hidden

try {
    $ready = $false
    for ($i = 0; $i -lt 100; ++$i) {
        if ($server.HasExited) {
            throw "chat_server exited before accepting connections"
        }
        try {
            $tcp = [System.Net.Sockets.TcpClient]::new()
            $async = $tcp.BeginConnect("127.0.0.1", $port, $null, $null)
            if ($async.AsyncWaitHandle.WaitOne(100)) {
                $tcp.EndConnect($async)
                $ready = $true
                $tcp.Close()
                break
            }
            $tcp.Close()
        } catch {
        }
        Start-Sleep -Milliseconds 100
    }

    if (-not $ready) {
        throw "chat_server did not become ready on port $port"
    }

    $output = & $ClientExe e2e --server "127.0.0.1:$port" 2>&1
    $exitCode = $LASTEXITCODE
    $output | ForEach-Object { Write-Host $_ }

    if ($exitCode -ne 0) {
        throw "chat_cli e2e failed with exit code $exitCode"
    }
    if (($output -join "`n") -notmatch "APP_E2E_OK") {
        throw "chat_cli e2e did not print APP_E2E_OK"
    }
} finally {
    if ($server -and -not $server.HasExited) {
        Stop-Process -Id $server.Id -Force
        $server.WaitForExit()
    }
}
