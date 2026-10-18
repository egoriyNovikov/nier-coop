# Creates an isolated test copy of NieR:Automata for automated tests (coop-anything rule 2:
# never touch the owner's install or saves).
# - data\ is hardlinked: the game only reads it, so 42 GB are not duplicated.
# - everything else (exe, steam_api64.dll, ...) is copied.
# - files of the mod (dinput8.dll, automatamp_*, backups, logs) are not taken from the owner's install.
# - coop_test.ini turns on the test mode of the mod: saves and Graphic.ini go to <slot>\documents
#   instead of %USERPROFILE%\Documents (and Steam Cloud), config.ini to <slot>\appdata instead of %APPDATA%.
#   Writes to the owner's Documents, %APPDATA%\PlatinumGames and install fail.
# WARNING: NVIDIA App notices a new folder with the game on its first launch and "optimizes" it: its
# OAWrapper.exe rewrites the owner's Documents\My Games\NieR_Automata\SystemData.dat (outside the mod's reach).
# Reuse the existing slots, or turn off automatic optimization of new games in NVIDIA App first.
param(
    [string]$Source = "D:\Steam\steamapps\common\NieRAutomata",
    [string]$Slot = "slot1"
)
$ErrorActionPreference = "Stop"

$root = Join-Path $PSScriptRoot "..\..\.testinstall" | Join-Path -ChildPath $Slot
$root = [System.IO.Path]::GetFullPath($root)

if ((Get-Item $Source).PSDrive.Name -ne ([System.IO.Path]::GetPathRoot($root)).Substring(0, 1)) {
    throw "Hardlinks need the test install on the same drive as $Source"
}

New-Item -ItemType Directory -Force $root | Out-Null

foreach ($item in Get-ChildItem $Source -Recurse -File) {
    $relative = $item.FullName.Substring($Source.Length).TrimStart('\')

    if ($relative -match '^(dinput8\.dll|automatamp_|start_nier|.*\.bak)') {
        continue
    }

    $target = Join-Path $root $relative
    if (Test-Path $target) {
        continue
    }

    New-Item -ItemType Directory -Force (Split-Path $target) | Out-Null

    if ($relative -like 'data\*') {
        New-Item -ItemType HardLink -Path $target -Target $item.FullName | Out-Null
    } else {
        Copy-Item $item.FullName $target
    }
}

$documents = Join-Path $root "documents"
New-Item -ItemType Directory -Force (Join-Path $documents "My Games\NieR_Automata") | Out-Null

$appData = Join-Path $root "appdata"
New-Item -ItemType Directory -Force (Join-Path $appData "PlatinumGames\NierAutomata") | Out-Null

# Start from a copy of the owner's saves and settings (only read). Existing test files are kept.
$copies = @{
    (Join-Path ([Environment]::GetFolderPath('MyDocuments')) "My Games\NieR_Automata") = (Join-Path $documents "My Games\NieR_Automata")
    (Join-Path ([Environment]::GetFolderPath('ApplicationData')) "PlatinumGames\NierAutomata") = (Join-Path $appData "PlatinumGames\NierAutomata")
}
foreach ($from in $copies.Keys) {
    foreach ($file in Get-ChildItem $from -File -ErrorAction SilentlyContinue) {
        $to = Join-Path $copies[$from] $file.Name
        if (-not (Test-Path $to)) {
            Copy-Item $file.FullName $to
        }
    }
}

# owner_* paths are write-protected by the mod in test mode (coop_test_guard.txt logs it).
$ownerDocuments = [Environment]::GetFolderPath('MyDocuments')
$ownerAppData = Join-Path ([Environment]::GetFolderPath('ApplicationData')) "PlatinumGames"
$ini = Join-Path $root "coop_test.ini"
Set-Content -Encoding unicode $ini ("[test]`r`ndocuments=$documents`r`nappdata=$appData`r`n" +
    "owner_documents=$ownerDocuments`r`nowner_install=$Source`r`nowner_appdata=$ownerAppData`r`n")

Write-Output "Test install: $root"
