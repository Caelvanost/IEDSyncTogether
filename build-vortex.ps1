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

$DistRoot = Join-Path $ProjectRoot "dist"
New-Item -ItemType Directory -Force -Path $DistRoot | Out-Null

$Archive = Join-Path $DistRoot "IEDSyncTogether-v0.1.0.zip"
if (Test-Path -LiteralPath $Archive) {
    Remove-Item -LiteralPath $Archive -Force
}
Compress-Archive -Path (Join-Path $PackageRoot "*") -DestinationPath $Archive -Force

Write-Host "Package Vortex cree: $Archive" -ForegroundColor Green
