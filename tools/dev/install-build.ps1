# Copies the built mod (and the server) into the game folder.
# A running game locks dinput8.dll: the old file is renamed to a backup, the new one loads on the next start.
param(
    [string]$GameDir = "D:\Steam\steamapps\common\NieRAutomata",
    [switch]$Server # Also build server.exe and put it into automatamp_server\ (for "Host Game").
)

$ErrorActionPreference = "Stop"
$repo = Resolve-Path "$PSScriptRoot\..\.."
$stamp = Get-Date -Format "yyyyMMdd_HHmmss"

function Install-File($source, $target) {
    if (Test-Path $target) {
        if ((Get-FileHash $source).Hash -eq (Get-FileHash $target).Hash) {
            Write-Host "Up to date: $target"
            return
        }

        Rename-Item $target "$target.bak_$stamp"
    }

    Copy-Item $source $target
    Write-Host "Installed: $target"
}

Install-File "$repo\build\bin\automatamp\dinput8.dll" "$GameDir\dinput8.dll"

if ($Server) {
    Push-Location "$repo\server"
    try {
        go build -o "$repo\build\bin\server.exe" .
        if ($LASTEXITCODE -ne 0) { throw "go build failed" }
    } finally {
        Pop-Location
    }

    New-Item -ItemType Directory -Force "$GameDir\automatamp_server" | Out-Null
    Install-File "$repo\build\bin\server.exe" "$GameDir\automatamp_server\server.exe"
}
