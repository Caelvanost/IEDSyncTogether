param(
    [string]$VcpkgRoot = $env:VCPKG_ROOT,
    [string]$Configuration = "Release"
)

$ErrorActionPreference = "Stop"

$ProjectRoot = Split-Path -Parent $MyInvocation.MyCommand.Path
$BuildRoot = Join-Path $ProjectRoot "build"
$PackageRoot = Join-Path $ProjectRoot "package"
$PluginRoot = Join-Path $PackageRoot "Data\SKSE\Plugins"
$ConfigSource = Join-Path $ProjectRoot "config\IEDSyncTogether.ini"
$OptionalRelayHostPackage = Join-Path $ProjectRoot "optional\RelayHost\package"
$FomodSource = Join-Path $ProjectRoot "fomod"
$StageRoot = [System.IO.Path]::GetFullPath((Join-Path $BuildRoot "fomod-stage"))

if (-not $VcpkgRoot) {
    $VcpkgRoot = "C:\dev\vcpkg"
}

$Toolchain = Join-Path $VcpkgRoot "scripts\buildsystems\vcpkg.cmake"
if (-not (Test-Path -LiteralPath $Toolchain)) {
    throw "Toolchain vcpkg introuvable: $Toolchain"
}

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
        Write-Subrecord $dataWriter "SNAM" ([System.Text.Encoding]::UTF8.GetBytes("IEDSyncTogether runtime marker`0"))

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

& cmake -S $ProjectRoot -B $BuildRoot "-DCMAKE_TOOLCHAIN_FILE=$Toolchain"
if ($LASTEXITCODE -ne 0) {
    throw "La configuration CMake a echoue."
}

$GeneratedPlugin = Join-Path $BuildRoot "__IEDSyncTogetherPlugin.cpp"
if (Test-Path -LiteralPath $GeneratedPlugin) {
    $content = Get-Content -LiteralPath $GeneratedPlugin -Raw
    if ($content -notmatch "using namespace std::literals") {
        $content = "using namespace std::literals;`r`n" + $content
        [System.IO.File]::WriteAllText($GeneratedPlugin, $content)
    }
}

& cmake --build $BuildRoot --config $Configuration
if ($LASTEXITCODE -ne 0) {
    throw "La compilation a echoue."
}

$dll = Get-ChildItem -LiteralPath $BuildRoot -Recurse -Filter "IEDSyncTogether.dll" |
    Sort-Object LastWriteTime -Descending |
    Select-Object -First 1
if (-not $dll) {
    throw "IEDSyncTogether.dll est introuvable apres compilation."
}

New-Item -ItemType Directory -Force -Path $PluginRoot | Out-Null
Copy-Item -LiteralPath $dll.FullName -Destination (Join-Path $PluginRoot "IEDSyncTogether.dll") -Force
Copy-Item -LiteralPath $ConfigSource -Destination (Join-Path $PluginRoot "IEDSyncTogether.ini") -Force
Write-MinimalPlugin (Join-Path $PackageRoot "Data\IEDSyncTogether.esp")

$CoreIni = Join-Path $PluginRoot "IEDSyncTogether.ini"
$RelayHostIni = Join-Path $OptionalRelayHostPackage "Data\SKSE\Plugins\IEDSyncTogether_RelayHost.ini"
$ModuleConfig = Join-Path $FomodSource "ModuleConfig.xml"
$Info = Join-Path $FomodSource "info.xml"

foreach ($RequiredPath in @($CoreIni, $RelayHostIni, $ModuleConfig, $Info)) {
    if (-not (Test-Path -LiteralPath $RequiredPath)) {
        throw "Fichier FOMOD requis introuvable: $RequiredPath"
    }
}

$CoreIniContent = Get-Content -LiteralPath $CoreIni -Raw
$RelayHostIniContent = Get-Content -LiteralPath $RelayHostIni -Raw
if ($CoreIniContent -notmatch '(?ms)^\[Network\].*?^Transport=STR\s*$') {
    throw "Le profil FOMOD principal doit utiliser Transport=STR."
}
if ($CoreIniContent -notmatch '(?ms)^\[Network\].*?^UdpFallback=0\s*$') {
    throw "Le profil FOMOD principal doit desactiver UdpFallback."
}
if ($CoreIniContent -notmatch '(?ms)^\[Network\].*?^RelayMode=0\s*$') {
    throw "Le profil FOMOD principal doit desactiver RelayMode."
}
if ($RelayHostIniContent -notmatch '(?ms)^\[Network\].*?^Transport=UDP\s*$') {
    throw "Le profil FOMOD UDP legacy doit utiliser Transport=UDP."
}
if ($RelayHostIniContent -notmatch '(?ms)^\[Network\].*?^RelayMode=1\s*$') {
    throw "Le profil FOMOD UDP legacy doit activer RelayMode."
}

try {
    [void][xml](Get-Content -LiteralPath $ModuleConfig -Raw)
    [void][xml](Get-Content -LiteralPath $Info -Raw)
} catch {
    throw "Metadonnees FOMOD XML invalides: $($_.Exception.Message)"
}

$BuildRootFull = [System.IO.Path]::GetFullPath($BuildRoot).TrimEnd('\')
if (-not $StageRoot.StartsWith("$BuildRootFull\", [System.StringComparison]::OrdinalIgnoreCase)) {
    throw "Le repertoire temporaire FOMOD est hors du repertoire de build: $StageRoot"
}
if (Test-Path -LiteralPath $StageRoot) {
    Remove-Item -LiteralPath $StageRoot -Recurse -Force
}

$CoreStage = Join-Path $StageRoot "00 Core"
$RelayHostStage = Join-Path $StageRoot "20 Legacy UDP Relay Host"
$FomodStage = Join-Path $StageRoot "fomod"
New-Item -ItemType Directory -Force -Path $CoreStage, $RelayHostStage, $FomodStage | Out-Null

Copy-Item -Path (Join-Path $PackageRoot "*") -Destination $CoreStage -Recurse -Force
Copy-Item -Path (Join-Path $OptionalRelayHostPackage "*") -Destination $RelayHostStage -Recurse -Force
Copy-Item -Path (Join-Path $FomodSource "*") -Destination $FomodStage -Recurse -Force

$Archive = Join-Path $ProjectRoot "IEDSyncTogether-v0.3.0-STRPM-FOMOD.zip"
if (Test-Path -LiteralPath $Archive) {
    Remove-Item -LiteralPath $Archive -Force
}

Compress-Archive -Path (Join-Path $StageRoot "*") -DestinationPath $Archive -Force

Add-Type -AssemblyName System.IO.Compression.FileSystem
$Zip = [System.IO.Compression.ZipFile]::OpenRead($Archive)
try {
    $Entries = @($Zip.Entries | ForEach-Object { $_.FullName.Replace('\', '/') })
    $RequiredEntries = @(
        "00 Core/Data/IEDSyncTogether.esp",
        "00 Core/Data/SKSE/Plugins/IEDSyncTogether.dll",
        "00 Core/Data/SKSE/Plugins/IEDSyncTogether.ini",
        "20 Legacy UDP Relay Host/Data/SKSE/Plugins/IEDSyncTogether_RelayHost.ini",
        "fomod/ModuleConfig.xml",
        "fomod/info.xml"
    )
    foreach ($RequiredEntry in $RequiredEntries) {
        if ($Entries -notcontains $RequiredEntry) {
            throw "Entree FOMOD absente de l'archive: $RequiredEntry"
        }
    }
} finally {
    $Zip.Dispose()
}

Write-Host "Package FOMOD cree: $Archive" -ForegroundColor Green
