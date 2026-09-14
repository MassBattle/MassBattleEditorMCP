param(
    [string]$PluginRoot = "",
    [string]$ServerName = "massbattle-editor-mcp",
    [ValidateSet("user", "project")]
    [string]$Scope = "user",
    [switch]$EditConfigOnly,
    [switch]$WhatIf
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
    param([string]$ResolvedPluginRoot, [string]$InstallScope)

    if ($InstallScope -eq "project") {
        $codexDir = Join-Path $ResolvedPluginRoot ".codex"
        return Join-Path $codexDir "config.toml"
    }

    $codexHome = $env:CODEX_HOME
    if ([string]::IsNullOrWhiteSpace($codexHome)) {
        $codexHome = Join-Path $HOME ".codex"
    }

    return Join-Path $codexHome "config.toml"
}

function Escape-TomlString {
    param([string]$Value)
    return ($Value -replace "\\", "\\" -replace '"', '\"')
}

function Update-CodexConfig {
    param(
        [string]$ConfigPath,
        [string]$Name,
        [string]$PythonRoot
    )

    $configDir = Split-Path -Parent $ConfigPath
    if (-not (Test-Path -LiteralPath $configDir)) {
        New-Item -ItemType Directory -Path $configDir | Out-Null
    }

    $block = @"

[mcp_servers.$Name]
command = "uv"
args = ["run", "--directory", "$(Escape-TomlString $PythonRoot)", "MassBattleMcpServer.py"]
startup_timeout_sec = 20
tool_timeout_sec = 120
enabled = true

"@

    $content = ""
    if (Test-Path -LiteralPath $ConfigPath) {
        $content = Get-Content -LiteralPath $ConfigPath -Raw
    }

    $escapedName = [Regex]::Escape($Name)
    $pattern = "(?ms)^\[mcp_servers\.$escapedName\]\r?\n.*?(?=^\[|\z)"
    if ($content -match $pattern) {
        $content = [Regex]::Replace($content, $pattern, $block.TrimStart())
    }
    else {
        if (-not $content.EndsWith("`n") -and $content.Length -gt 0) {
            $content += "`n"
        }
        $content += $block
    }

    if ($WhatIf) {
        Write-Host "Would update $ConfigPath with MCP server '$Name'."
        return
    }

    Set-Content -LiteralPath $ConfigPath -Value $content -Encoding UTF8
    Write-Host "Updated Codex config: $ConfigPath"
}

$resolvedPluginRoot = Resolve-PluginRoot $PluginRoot
$pythonRoot = Join-Path $resolvedPluginRoot "Resources\Python"
$serverScript = Join-Path $pythonRoot "MassBattleMcpServer.py"

if (-not (Test-Path -LiteralPath $serverScript)) {
    throw "MassBattleMcpServer.py was not found at: $serverScript"
}

$uv = Get-Command uv -ErrorAction SilentlyContinue
if (-not $uv) {
    throw "uv is required to launch the MassBattle MCP Python server."
}

if (-not $EditConfigOnly) {
    $codex = Get-Command codex -ErrorAction SilentlyContinue
    if ($codex -and $Scope -eq "user") {
        if ($WhatIf) {
            Write-Host "Would run: codex mcp add $ServerName -- uv run --directory `"$pythonRoot`" MassBattleMcpServer.py"
        }
        else {
            try {
                & codex mcp add $ServerName -- uv run --directory $pythonRoot MassBattleMcpServer.py
                if ($LASTEXITCODE -eq 0) {
                    Write-Host "Installed Codex MCP server '$ServerName' with codex mcp add."
                    exit 0
                }
                Write-Host "codex mcp add exited with code $LASTEXITCODE; falling back to config.toml edit." -ForegroundColor Yellow
            }
            catch {
                Write-Host "codex mcp add failed: $($_.Exception.Message)" -ForegroundColor Yellow
                Write-Host "Falling back to config.toml edit." -ForegroundColor Yellow
            }
        }
    }
}

$configPath = Get-CodexConfigPath $resolvedPluginRoot $Scope
Update-CodexConfig -ConfigPath $configPath -Name $ServerName -PythonRoot $pythonRoot

Write-Host "Restart Codex or start a new Codex session, then use /mcp to verify '$ServerName'."
