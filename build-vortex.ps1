param(
    [string]$VcpkgRoot = $env:VCPKG_ROOT,
    [string]$Configuration = "Release",
    [string]$PatchedIedDll = ""
)

$ErrorActionPreference = "Stop"

$ProjectRoot = Split-Path -Parent $MyInvocation.MyCommand.Path
$BuildRoot = Join-Path $ProjectRoot "build"
$PackageRoot = Join-Path $BuildRoot "package"
$PluginRoot = Join-Path $PackageRoot "Data\SKSE\Plugins"
$DistRoot = Join-Path $ProjectRoot "dist"
$BundledPatchedIedDll = Join-Path $ProjectRoot "third-party\IED-1.7.4\ImmersiveEquipmentDisplays.dll"
$IedLicenseSource = Join-Path $ProjectRoot "third-party\IED-LICENSE.txt"
$IedLicenseDestination = Join-Path $PackageRoot "Data\IEDSyncTogether\licenses\IED-LICENSE.txt"

if (-not $VcpkgRoot) {
    $VcpkgRoot = "C:\dev\vcpkg"
}

$Toolchain = Join-Path $VcpkgRoot "scripts\buildsystems\vcpkg.cmake"
if (-not (Test-Path -LiteralPath $Toolchain)) {
    throw "Toolchain vcpkg introuvable: $Toolchain"
}

$CMakeText = Get-Content -LiteralPath (Join-Path $ProjectRoot "CMakeLists.txt") -Raw
$VersionMatch = [regex]::Match($CMakeText, 'VERSION\s+([0-9]+\.[0-9]+\.[0-9]+)')
if (-not $VersionMatch.Success) {
    throw "Impossible de lire la VERSION depuis CMakeLists.txt"
}
$ProjectVersion = $VersionMatch.Groups[1].Value

function Write-Subrecord {
    param(
        [System.IO.BinaryWriter]$Writer,
        [string]$Signature,
        [byte[]]$Data
    )
    $Writer.Write([System.Text.Encoding]::ASCII.GetBytes($Signature))
    $Writer.Write([uint16]$Data.Length)
    $Writer.Write($Data)
}

function Write-MinimalPlugin {
    param([string]$Path)

    $memory = [System.IO.MemoryStream]::new()
    $dataWriter = [System.IO.BinaryWriter]::new($memory)
    try {
        $hedr = [System.IO.MemoryStream]::new()
        $hedrWriter = [System.IO.BinaryWriter]::new($hedr)
        try {
            $hedrWriter.Write([single]1.7)
            $hedrWriter.Write([uint32]0)
            $hedrWriter.Write([uint32]0x800)
            Write-Subrecord $dataWriter "HEDR" $hedr.ToArray()
        } finally {
            $hedrWriter.Dispose()
            $hedr.Dispose()
        }

        Write-Subrecord $dataWriter "CNAM" ([System.Text.Encoding]::UTF8.GetBytes("Caelvanost`0"))
        Write-Subrecord $dataWriter "SNAM" ([System.Text.Encoding]::UTF8.GetBytes("IEDSyncTogether STRPM proxy-isolation marker`0"))

        $file = [System.IO.File]::Open($Path, [System.IO.FileMode]::Create)
        $writer = [System.IO.BinaryWriter]::new($file)
        try {
            $writer.Write([System.Text.Encoding]::ASCII.GetBytes("TES4"))
            $writer.Write([uint32]$memory.Length)
            $writer.Write([uint32]0x200)
            $writer.Write([uint32]0)
            $writer.Write([uint16]0)
            $writer.Write([uint16]0)
            $writer.Write([uint16]44)
            $writer.Write([uint16]0)
            $writer.Write($memory.ToArray())
        } finally {
            $writer.Dispose()
            $file.Dispose()
        }
    } finally {
        $dataWriter.Dispose()
        $memory.Dispose()
    }
}

if (-not $PatchedIedDll) {
    $PatchedIedDll = $BundledPatchedIedDll
}
if (-not (Test-Path -LiteralPath $PatchedIedDll -PathType Leaf)) {
    throw @"
ImmersiveEquipmentDisplays.dll 1.7.4 patche introuvable:
$PatchedIedDll

Recupere d'abord le dernier commit de feature/proxy-ied-isolation apres le workflow
'Build patched IED 1.7.4', ou passe -PatchedIedDll <chemin>.
"@
}
if (-not (Test-Path -LiteralPath $IedLicenseSource -PathType Leaf)) {
    throw "Licence IED introuvable: $IedLicenseSource"
}

& cmake -S $ProjectRoot -B $BuildRoot "-DCMAKE_TOOLCHAIN_FILE=$Toolchain"
if ($LASTEXITCODE -ne 0) { throw "La configuration CMake a echoue." }

$GeneratedPlugin = Join-Path $BuildRoot "__IEDSyncTogetherPlugin.cpp"
if (Test-Path -LiteralPath $GeneratedPlugin) {
    $content = Get-Content -LiteralPath $GeneratedPlugin -Raw
    if ($content -notmatch "using namespace std::literals") {
        $content = "using namespace std::literals;`r`n" + $content
        [System.IO.File]::WriteAllText($GeneratedPlugin, $content)
    }
}

& cmake --build $BuildRoot --config $Configuration
if ($LASTEXITCODE -ne 0) { throw "La compilation a echoue." }

$dll = Get-ChildItem -LiteralPath $BuildRoot -Recurse -Filter "IEDSyncTogether.dll" -File |
    Sort-Object LastWriteTime -Descending |
    Select-Object -First 1
if (-not $dll) { throw "IEDSyncTogether.dll est introuvable apres compilation." }

if (Test-Path -LiteralPath $PackageRoot) {
    Remove-Item -LiteralPath $PackageRoot -Recurse -Force
}
New-Item -ItemType Directory -Force -Path $PluginRoot, $DistRoot | Out-Null

Copy-Item -LiteralPath $dll.FullName -Destination (Join-Path $PluginRoot "IEDSyncTogether.dll") -Force
Copy-Item -LiteralPath $PatchedIedDll -Destination (Join-Path $PluginRoot "ImmersiveEquipmentDisplays.dll") -Force
Write-MinimalPlugin (Join-Path $PackageRoot "Data\IEDSyncTogether.esp")

New-Item -ItemType Directory -Force -Path (Split-Path -Parent $IedLicenseDestination) | Out-Null
Copy-Item -LiteralPath $IedLicenseSource -Destination $IedLicenseDestination -Force

$Archive = Join-Path $DistRoot "IEDSyncTogether-v$ProjectVersion.zip"
if (Test-Path -LiteralPath $Archive) {
    Remove-Item -LiteralPath $Archive -Force
}
Compress-Archive -Path (Join-Path $PackageRoot "*") -DestinationPath $Archive -Force

Add-Type -AssemblyName System.IO.Compression.FileSystem
$zip = [System.IO.Compression.ZipFile]::OpenRead($Archive)
try {
    $entries = @($zip.Entries | ForEach-Object { $_.FullName.Replace('\', '/') })
    foreach ($required in @(
        "Data/SKSE/Plugins/IEDSyncTogether.dll",
        "Data/SKSE/Plugins/ImmersiveEquipmentDisplays.dll",
        "Data/IEDSyncTogether.esp",
        "Data/IEDSyncTogether/licenses/IED-LICENSE.txt"
    )) {
        if ($entries -notcontains $required) {
            throw "Entree absente de l'archive: $required"
        }
    }
} finally {
    $zip.Dispose()
}

Write-Host "Package cree: $Archive" -ForegroundColor Green
Write-Host "Inclut ImmersiveEquipmentDisplays.dll 1.7.4 patche pour isoler les proxies STR." -ForegroundColor Green
