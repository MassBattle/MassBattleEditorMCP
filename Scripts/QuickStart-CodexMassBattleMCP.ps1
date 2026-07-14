param(
    [string]$PluginRoot = "",
    [string]$ServerName = "massbattle-editor-mcp",
    [int]$Port = 55258,
    [switch]$SkipBridgePing
)

$ErrorActionPreference = "Stop"

function Resolve-PluginRoot {
    param([string]$InputPath)

    if (-not [string]::IsNullOrWhiteSpace($InputPath)) {
        return (Resolve-Path -LiteralPath $InputPath).Path
    }

    return (Resolve-Path -LiteralPath (Join-Path $PSScriptRoot "..")).Path
}

function Get-CodexConfigPath {
    $codexHome = $env:CODEX_HOME
    if ([string]::IsNullOrWhiteSpace($codexHome)) {
        $codexHome = Join-Path $HOME ".codex"
    }

    return Join-Path $codexHome "config.toml"
}

function Write-Step {
    param([string]$Text)
    Write-Host ""
    Write-Host "== $Text ==" -ForegroundColor Cyan
}

function Invoke-BridgePing {
    param(
        [string]$HostName,
        [int]$TargetPort
    )

    $client = [System.Net.Sockets.TcpClient]::new()
    try {
        $async = $client.BeginConnect($HostName, $TargetPort, $null, $null)
        if (-not $async.AsyncWaitHandle.WaitOne([TimeSpan]::FromSeconds(5), $false)) {
            throw "Timed out connecting to $HostName`:$TargetPort"
        }
        $client.EndConnect($async)

        $stream = $client.GetStream()
        $payload = [System.Text.Encoding]::UTF8.GetBytes('{"command":"ping","params":{}}' + [char]0)
        $stream.Write($payload, 0, $payload.Length)
        $stream.Flush()

        $buffer = New-Object byte[] 4096
        $bytes = [System.Collections.Generic.List[byte]]::new()
        $deadline = (Get-Date).AddSeconds(5)
        $done = $false

        while ((Get-Date) -lt $deadline -and -not $done) {
            if (-not $stream.DataAvailable) {
                Start-Sleep -Milliseconds 50
                continue
            }

            $read = $stream.Read($buffer, 0, $buffer.Length)
            if ($read -le 0) {
                break
            }

            for ($i = 0; $i -lt $read; ++$i) {
                if ($buffer[$i] -eq 0) {
                    $done = $true
                    break
                }
                $bytes.Add($buffer[$i])
            }
        }

        if ($bytes.Count -eq 0) {
            throw "Bridge returned no response."
        }

        return [System.Text.Encoding]::UTF8.GetString($bytes.ToArray())
    }
    finally {
        $client.Close()
    }
}

$resolvedPluginRoot = Resolve-PluginRoot $PluginRoot
$pythonRoot = Join-Path $resolvedPluginRoot "Resources\Python"
$serverScript = Join-Path $pythonRoot "MassBattleMcpServer.py"
$configPath = Get-CodexConfigPath

Write-Step "MassBattleEditorMCP quick start"
Write-Host "Plugin root: $resolvedPluginRoot"
Write-Host "Python MCP root: $pythonRoot"
Write-Host "Codex config: $configPath"

Write-Step "Check files and uv"
if (-not (Test-Path -LiteralPath $serverScript)) {
    throw "Missing MCP server script: $serverScript"
}
Write-Host "Found server script."

$uv = Get-Command uv -ErrorAction SilentlyContinue
if (-not $uv) {
    throw "uv is required. Install uv or run Scripts\Install-CodexMassBattleMCP.ps1 on a machine with uv."
}
Write-Host "Found uv: $($uv.Source)"

Write-Step "Check Python MCP server"
& uv run --directory $pythonRoot python -m py_compile MassBattleMcpServer.py
if ($LASTEXITCODE -ne 0) {
    throw "Python compile check failed."
}
& uv run --directory $pythonRoot python -c "import MassBattleMcpServer; print('server import ok')"
if ($LASTEXITCODE -ne 0) {
    throw "Python import check failed."
}

Write-Step "Check Codex config"
if (-not (Test-Path -LiteralPath $configPath)) {
    Write-Host "Codex config does not exist yet. Run Scripts\Install-CodexMassBattleMCP.ps1." -ForegroundColor Yellow
}
else {
    $content = Get-Content -LiteralPath $configPath -Raw
    if ($content -match "(?m)^\[mcp_servers\.$([Regex]::Escape($ServerName))\]") {
        Write-Host "Found [$ServerName] MCP server block."
    }
    else {
        Write-Host "MCP server block '$ServerName' was not found. Run Scripts\Install-CodexMassBattleMCP.ps1." -ForegroundColor Yellow
    }
}

Write-Step "Check Unreal bridge"
$portOpen = Test-NetConnection 127.0.0.1 -Port $Port -InformationLevel Quiet -WarningAction SilentlyContinue
if (-not $portOpen) {
    Write-Host "UE bridge is not listening on 127.0.0.1:$Port." -ForegroundColor Yellow
    Write-Host "Open the Unreal project with MassBattleEditorMCP enabled, then rerun this script."
}
else {
    Write-Host "UE bridge port is open."
    if (-not $SkipBridgePing) {
        $response = Invoke-BridgePing -HostName "127.0.0.1" -TargetPort $Port
        Write-Host "Bridge ping response: $response"
    }
}

Write-Step "Next checks inside Codex"
Write-Host "After restarting Codex, search for MassBattle tools and call:"
Write-Host "  massbattle_ping"
Write-Host "  unit_get_api_status"
Write-Host "  niagara_get_api_status"
Write-Host ""
Write-Host "For FFxConfig.AgentBehaviorState use: None, Appearing, Sleeping, Patrolling, Attacking, Hit, Dying."
