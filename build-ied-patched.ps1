param(
    [string]$IedSourceRoot = $env:IED_SOURCE_ROOT,
    [string]$Configuration = "Release MT Post 629 143",
    [string]$Platform = "x64",
    [string]$MsBuild = ""
)

$ErrorActionPreference = "Stop"

$ProjectRoot = Split-Path -Parent $MyInvocation.MyCommand.Path
$PatchPath = Join-Path $ProjectRoot "integration\ied-dev\ied-sync-together.patch"
$OutputRoot = Join-Path $ProjectRoot "build\ied-patched"
$OutputDll = Join-Path $OutputRoot "ImmersiveEquipmentDisplays.dll"

# Official SlavicPotato/ied-dev commit whose commit message is "1.7.4".
$ExpectedIedCommit = "3f014c3e8574ef0e88b2ec0b7cdf58b86c9737b0"

function Resolve-MSBuild {
    param([string]$ExplicitPath)

    if ($ExplicitPath) {
        if (-not (Test-Path -LiteralPath $ExplicitPath -PathType Leaf)) {
            throw "MSBuild introuvable: $ExplicitPath"
        }
        return (Resolve-Path -LiteralPath $ExplicitPath).Path
    }

    $vswhere = Join-Path ${env:ProgramFiles(x86)} "Microsoft Visual Studio\Installer\vswhere.exe"
    if (Test-Path -LiteralPath $vswhere -PathType Leaf) {
        $installation = & $vswhere -latest -products * -requires Microsoft.Component.MSBuild -property installationPath
        if ($LASTEXITCODE -eq 0 -and $installation) {
            $candidate = Join-Path $installation.Trim() "MSBuild\Current\Bin\MSBuild.exe"
            if (Test-Path -LiteralPath $candidate -PathType Leaf) {
                return $candidate
            }
        }
    }

    $command = Get-Command msbuild.exe -ErrorAction SilentlyContinue
    if ($command) {
        return $command.Source
    }

    throw "MSBuild introuvable. Installe le workload C++ de Visual Studio ou passe -MsBuild <chemin>."
}

if (-not (Test-Path -LiteralPath $PatchPath -PathType Leaf)) {
    throw "Patch IEDSyncTogether introuvable: $PatchPath"
}

if (-not $IedSourceRoot) {
    $defaultSource = Join-Path (Split-Path -Parent $ProjectRoot) "ied-dev"
    if (Test-Path -LiteralPath $defaultSource -PathType Container) {
        $IedSourceRoot = $defaultSource
    }
}

if (-not $IedSourceRoot -or -not (Test-Path -LiteralPath $IedSourceRoot -PathType Container)) {
    throw @"
Source IED introuvable.

IED 1.7.4 utilise un workspace Visual Studio avec des dependances soeurs
(sse-build-resources, imgui, assimp). Prepare un checkout compilable de
SlavicPotato/ied-dev, puis relance avec :

  .\build-ied-patched.ps1 -IedSourceRoot C:\chemin\vers\ied-dev

Tu peux aussi definir IED_SOURCE_ROOT.
"@
}

$IedSourceRoot = (Resolve-Path -LiteralPath $IedSourceRoot).Path
$GitDir = Join-Path $IedSourceRoot ".git"
if (-not (Test-Path -LiteralPath $GitDir)) {
    throw "Le dossier IED doit etre un checkout Git pour pouvoir creer un worktree propre: $IedSourceRoot"
}

$git = Get-Command git.exe -ErrorAction SilentlyContinue
if (-not $git) {
    throw "git.exe est requis pour construire la version IED patchee."
}

& $git.Source -C $IedSourceRoot cat-file -e "$ExpectedIedCommit^{commit}"
if ($LASTEXITCODE -ne 0) {
    throw "Le checkout IED ne contient pas le commit officiel 1.7.4 $ExpectedIedCommit. Fais un git fetch avant de relancer."
}

$IedWorkspaceRoot = Split-Path -Parent $IedSourceRoot
$WorktreeRoot = Join-Path $IedWorkspaceRoot ".iedsync-ied-dev-1.7.4"
$Solution = Join-Path $WorktreeRoot "ImmersiveEquipmentDisplays.sln"
$msbuildPath = Resolve-MSBuild $MsBuild

if (Test-Path -LiteralPath $WorktreeRoot) {
    & $git.Source -C $IedSourceRoot worktree remove --force $WorktreeRoot 2>$null
    if (Test-Path -LiteralPath $WorktreeRoot) {
        Remove-Item -LiteralPath $WorktreeRoot -Recurse -Force
    }
}

New-Item -ItemType Directory -Force -Path $OutputRoot | Out-Null
if (Test-Path -LiteralPath $OutputDll) {
    Remove-Item -LiteralPath $OutputDll -Force
}

try {
    & $git.Source -C $IedSourceRoot worktree add --detach $WorktreeRoot $ExpectedIedCommit
    if ($LASTEXITCODE -ne 0) {
        throw "Impossible de creer le worktree IED 1.7.4."
    }

    Push-Location $WorktreeRoot
    try {
        & $git.Source apply --check $PatchPath
        if ($LASTEXITCODE -ne 0) {
            throw "Le patch IEDSyncTogether ne s'applique pas proprement au commit IED 1.7.4."
        }

        & $git.Source apply $PatchPath
        if ($LASTEXITCODE -ne 0) {
            throw "Echec de l'application du patch IEDSyncTogether."
        }

        & $msbuildPath $Solution "/m" "/t:ImmersiveEquipmentDisplays" "/p:Configuration=$Configuration" "/p:Platform=$Platform"
        if ($LASTEXITCODE -ne 0) {
            throw @"
La compilation d'IED a echoue.
Le projet IED 1.7.4 attend ses dependances de build historiques a cote de ied-dev
(notamment sse-build-resources, imgui et assimp) ainsi que son environnement vcpkg.
"@
        }
    } finally {
        Pop-Location
    }

    $builtDll = Get-ChildItem -LiteralPath $WorktreeRoot -Recurse -Filter "ImmersiveEquipmentDisplays.dll" -File |
        Sort-Object LastWriteTime -Descending |
        Select-Object -First 1

    if (-not $builtDll) {
        throw "ImmersiveEquipmentDisplays.dll est introuvable apres compilation."
    }

    Copy-Item -LiteralPath $builtDll.FullName -Destination $OutputDll -Force

    $version = (Get-Item -LiteralPath $OutputDll).VersionInfo.ProductVersion
    Write-Host "IED patche construit: $OutputDll" -ForegroundColor Green
    if ($version) {
        Write-Host "Version detectee: $version"
    }
} finally {
    if (Test-Path -LiteralPath $WorktreeRoot) {
        & $git.Source -C $IedSourceRoot worktree remove --force $WorktreeRoot 2>$null
        if (Test-Path -LiteralPath $WorktreeRoot) {
            Remove-Item -LiteralPath $WorktreeRoot -Recurse -Force
        }
    }
}

Write-Output $OutputDll
