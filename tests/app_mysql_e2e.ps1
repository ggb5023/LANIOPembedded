param(
    [Parameter(Mandatory = $true)]
    [string]$ServerExe,

    [Parameter(Mandatory = $true)]
    [string]$ClientExe,

    [string]$SchemaPath = "",
    [string]$MysqlExe = "",
    [string]$MysqlHost = "127.0.0.1",
    [int]$MysqlPort = 3306,
    [string]$MysqlUser = "root",
    [string]$MysqlPassword = "123456",
    [string]$MysqlDatabase = "lan_chat_e2e"
)

$ErrorActionPreference = "Stop"

function Resolve-MysqlExe {
    param([string]$Explicit)
    if ($Explicit -and (Test-Path $Explicit)) {
        return (Resolve-Path $Explicit).Path
    }
    if ($env:MYSQL_EXE -and (Test-Path $env:MYSQL_EXE)) {
        return (Resolve-Path $env:MYSQL_EXE).Path
    }
    $cmd = Get-Command mysql.exe -ErrorAction SilentlyContinue
    if ($cmd) {
        return $cmd.Source
    }
    $candidates = @(
        "C:\Program Files\MySQL\MySQL Server 8.0\bin\mysql.exe",
        "D:\mysql\bin\mysql.exe"
    )
    foreach ($candidate in $candidates) {
        if (Test-Path $candidate) {
            return $candidate
        }
    }
    throw "mysql.exe not found. Set MYSQL_EXE or pass -MysqlExe."
}

function Invoke-Mysql {
    param(
        [string]$Exe,
        [string]$Sql,
        [string]$Database = ""
    )
    $mysqlArgs = @("-h", $MysqlHost, "-P", "$MysqlPort", "-u", $MysqlUser, "--batch", "--raw")
    if ($Database) {
        $mysqlArgs += $Database
    }
    $oldPreference = $ErrorActionPreference
    $oldMysqlPwd = $env:MYSQL_PWD
    $ErrorActionPreference = "Continue"
    $env:MYSQL_PWD = $MysqlPassword
    try {
        $output = $Sql | & $Exe @mysqlArgs 2>&1
    } finally {
        $ErrorActionPreference = $oldPreference
        $env:MYSQL_PWD = $oldMysqlPwd
    }
    if ($LASTEXITCODE -ne 0) {
        $output | ForEach-Object { Write-Host $_ }
        throw "mysql command failed with exit code $LASTEXITCODE"
    }
    return $output
}

function Wait-ServerReady {
    param([System.Diagnostics.Process]$Server, [int]$Port)
    for ($i = 0; $i -lt 100; ++$i) {
        if ($Server.HasExited) {
            throw "chat_server exited before accepting connections"
        }
        try {
            $tcp = [System.Net.Sockets.TcpClient]::new()
            $async = $tcp.BeginConnect("127.0.0.1", $Port, $null, $null)
            if ($async.AsyncWaitHandle.WaitOne(100)) {
                $tcp.EndConnect($async)
                $tcp.Close()
                return
            }
            $tcp.Close()
        } catch {
        }
        Start-Sleep -Milliseconds 100
    }
    throw "chat_server did not become ready on port $Port"
}

function Run-Client {
    param([string[]]$ClientArgs)
    $output = & $ClientExe @ClientArgs 2>&1
    $exitCode = $LASTEXITCODE
    $output | ForEach-Object { Write-Host $_ }
    if ($exitCode -ne 0) {
        throw "chat_cli failed with exit code ${exitCode}: $($ClientArgs -join ' ')"
    }
    return ($output -join "`n")
}

function Start-Listen {
    param([string]$Username, [string]$Password, [int]$ExpectCount, [string]$OutputDir = "")
    $clientArgs = @(
        "listen", "--server", "127.0.0.1:$port",
        "--username", $Username, "--password", $Password,
        "--expect-count", "$ExpectCount", "--timeout-ms", "15000"
    )
    if ($OutputDir) {
        $clientArgs += @("--output-dir", $OutputDir)
    }
    $startInfo = [System.Diagnostics.ProcessStartInfo]::new()
    $startInfo.FileName = $ClientExe
    $startInfo.Arguments = ($clientArgs | ForEach-Object { '"' + ($_ -replace '"', '\"') + '"' }) -join " "
    $startInfo.UseShellExecute = $false
    $startInfo.CreateNoWindow = $true
    $startInfo.RedirectStandardOutput = $true
    $startInfo.RedirectStandardError = $true
    $process = [System.Diagnostics.Process]::new()
    $process.StartInfo = $startInfo
    [void]$process.Start()
    return $process
}

function Read-ProcessOutput {
    param([System.Diagnostics.Process]$Process)
    $stdoutTask = $Process.StandardOutput.ReadToEndAsync()
    $stderrTask = $Process.StandardError.ReadToEndAsync()
    $Process.WaitForExit()
    $stdout = $stdoutTask.Result
    $stderr = $stderrTask.Result
    if ($stdout) { Write-Host $stdout }
    if ($stderr) { Write-Host $stderr }
    if ($Process.ExitCode -ne 0) {
        throw "listen failed with exit code $($Process.ExitCode)"
    }
    return "$stdout`n$stderr"
}

function Extract-Id {
    param([string]$Text, [string]$Name)
    if ($Text -match "$Name=(\d+)") {
        return [UInt64]$Matches[1]
    }
    throw "missing $Name in output"
}

if (-not $SchemaPath) {
    $SchemaPath = Join-Path (Split-Path $PSScriptRoot -Parent) "db\mysql\schema.sql"
}
if (-not (Test-Path $SchemaPath)) {
    throw "schema not found: $SchemaPath"
}
if ($env:LAN_CHAT_MYSQL_TEST_PASSWORD) {
    $MysqlPassword = $env:LAN_CHAT_MYSQL_TEST_PASSWORD
}
if ($env:LAN_CHAT_MYSQL_TEST_HOST) {
    $MysqlHost = $env:LAN_CHAT_MYSQL_TEST_HOST
}
if ($env:LAN_CHAT_MYSQL_TEST_PORT) {
    $MysqlPort = [int]$env:LAN_CHAT_MYSQL_TEST_PORT
}
if ($env:LAN_CHAT_MYSQL_TEST_USER) {
    $MysqlUser = $env:LAN_CHAT_MYSQL_TEST_USER
}

$mysql = Resolve-MysqlExe $MysqlExe
$schema = Get-Content $SchemaPath -Raw
$schemaForDb = $schema -replace "CREATE DATABASE IF NOT EXISTS ``lan_chat``", "CREATE DATABASE IF NOT EXISTS ``$MysqlDatabase``"
$schemaForDb = $schemaForDb -replace "USE ``lan_chat``;", "USE ``$MysqlDatabase``;"
Invoke-Mysql -Exe $mysql -Sql "DROP DATABASE IF EXISTS ``$MysqlDatabase``; CREATE DATABASE ``$MysqlDatabase`` DEFAULT CHARACTER SET utf8mb4 DEFAULT COLLATE utf8mb4_unicode_ci;"
Invoke-Mysql -Exe $mysql -Sql $schemaForDb

$port = Get-Random -Minimum 20000 -Maximum 50000
$serverArgs = @(
    "--storage", "mysql",
    "--host", "127.0.0.1",
    "--port", "$port",
    "--mysql-host", $MysqlHost,
    "--mysql-port", "$MysqlPort",
    "--mysql-database", $MysqlDatabase,
    "--mysql-user", $MysqlUser,
    "--mysql-password", $MysqlPassword
)
$server = Start-Process -FilePath $ServerExe -ArgumentList $serverArgs -PassThru -WindowStyle Hidden
$workDir = Join-Path ([System.IO.Path]::GetTempPath()) ("lan_chat_e2e_" + [Guid]::NewGuid().ToString("N"))
New-Item -ItemType Directory -Path $workDir | Out-Null

try {
    Wait-ServerReady -Server $server -Port $port
    $suffix = [DateTimeOffset]::UtcNow.ToUnixTimeMilliseconds()
    $alice = "alice_$suffix"
    $bob = "bob_$suffix"
    $carol = "carol_$suffix"
    $alicePass = "alice-pass-$suffix"
    $bobPass = "bob-pass-$suffix"
    $carolPass = "carol-pass-$suffix"

    $aliceOut = Run-Client -ClientArgs @("register", "--server", "127.0.0.1:$port", "--username", $alice, "--password", $alicePass, "--nickname", "Alice")
    $bobOut = Run-Client -ClientArgs @("register", "--server", "127.0.0.1:$port", "--username", $bob, "--password", $bobPass, "--nickname", "Bob")
    $carolOut = Run-Client -ClientArgs @("register", "--server", "127.0.0.1:$port", "--username", $carol, "--password", $carolPass, "--nickname", "Carol")
    $aliceId = Extract-Id $aliceOut "user_id"
    $bobId = Extract-Id $bobOut "user_id"
    $carolId = Extract-Id $carolOut "user_id"

    $bobListen = Start-Listen -Username $bob -Password $bobPass -ExpectCount 1
    Start-Sleep -Milliseconds 300
    Run-Client -ClientArgs @("send", "--server", "127.0.0.1:$port", "--username", $alice, "--password", $alicePass, "--to-user-id", "$bobId", "--message", "phase6-private")
    $bobText = Read-ProcessOutput $bobListen
    if ($bobText -notmatch "PRIVATE_TEXT" -or $bobText -notmatch "phase6-private") {
        throw "private text E2E marker missing"
    }

    $groupOut = Run-Client -ClientArgs @("group-create", "--server", "127.0.0.1:$port", "--username", $alice, "--password", $alicePass, "--name", "phase6-group-$suffix", "--member-user-ids", "$bobId,$carolId")
    $conversationId = Extract-Id $groupOut "conversation_id"
    $bobGroupListen = Start-Listen -Username $bob -Password $bobPass -ExpectCount 1
    $carolGroupListen = Start-Listen -Username $carol -Password $carolPass -ExpectCount 1
    Start-Sleep -Milliseconds 300
    Run-Client -ClientArgs @("group-send", "--server", "127.0.0.1:$port", "--username", $alice, "--password", $alicePass, "--conversation-id", "$conversationId", "--message", "phase6-group")
    $bobGroup = Read-ProcessOutput $bobGroupListen
    $carolGroup = Read-ProcessOutput $carolGroupListen
    if ($bobGroup -notmatch "GROUP_TEXT" -or $carolGroup -notmatch "GROUP_TEXT") {
        throw "group text E2E marker missing"
    }

    $sendFile = Join-Path $workDir "phase6-file.txt"
    $recvDir = Join-Path $workDir "recv"
    New-Item -ItemType Directory -Path $recvDir | Out-Null
    Set-Content -Path $sendFile -Value "phase6 file transfer content $suffix" -NoNewline
    $bobFileListen = Start-Listen -Username $bob -Password $bobPass -ExpectCount 1 -OutputDir $recvDir
    Start-Sleep -Milliseconds 300
    Run-Client -ClientArgs @("send-file", "--server", "127.0.0.1:$port", "--username", $alice, "--password", $alicePass, "--to-user-id", "$bobId", "--path", $sendFile)
    $bobFile = Read-ProcessOutput $bobFileListen
    if ($bobFile -notmatch "FILE_RECEIVED") {
        throw "file E2E marker missing"
    }
    $receivedFile = Get-ChildItem -Path $recvDir -File | Select-Object -First 1
    if (-not $receivedFile) {
        throw "received file not found"
    }
    if ((Get-Content $sendFile -Raw) -ne (Get-Content $receivedFile.FullName -Raw)) {
        throw "received file content mismatch"
    }

    Write-Host "APP_E2E_OK"
} finally {
    if ($server -and -not $server.HasExited) {
        Stop-Process -Id $server.Id -Force
        $server.WaitForExit()
    }
    Remove-Item $workDir -Recurse -Force -ErrorAction SilentlyContinue
}
