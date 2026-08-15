param(
    [string]$VcpkgRoot = $env:VCPKG_ROOT,
    [string]$Configuration = "Release",
    [string]$IedSourceRoot = $env:IED_SOURCE_ROOT,
    [string]$PatchedIedDll = ""
)

$ErrorActionPreference = "Stop"

$ProjectRoot = Split-Path -Parent $MyInvocation.MyCommand.Path
$BuildRoot = Join-Path $ProjectRoot "build"
$PackageRoot = Join-Path $ProjectRoot "package"
$PluginRoot = Join-Path $PackageRoot "Data\SKSE\Plugins"
$ConfigSource = Join-Path $ProjectRoot "config\IEDSyncTogether.ini"
$IedBuildScript = Join-Path $ProjectRoot "build-ied-patched.ps1"
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

$dll = Get-ChildItem -LiteralPath $BuildRoot -Recurse -Filter "IEDSyncTogether.dll" -File |
    Sort-Object LastWriteTime -Descending |
    Select-Object -First 1
if (-not $dll) {
    throw "IEDSyncTogether.dll est introuvable apres compilation."
}

if (-not $PatchedIedDll) {
    if (Test-Path -LiteralPath $BundledPatchedIedDll -PathType Leaf) {
        $PatchedIedDll = $BundledPatchedIedDll
        Write-Host "Utilisation du DLL IED 1.7.4 patche precompile: $PatchedIedDll" -ForegroundColor Cyan
    } else {
        $PatchedIedDll = Join-Path $BuildRoot "ied-patched\ImmersiveEquipmentDisplays.dll"

        if (-not (Test-Path -LiteralPath $PatchedIedDll -PathType Leaf)) {
            if (-not (Test-Path -LiteralPath $IedBuildScript -PathType Leaf)) {
                throw "Script de build IED patche introuvable: $IedBuildScript"
            }

            $iedArgs = @{}
            if ($IedSourceRoot) {
                $iedArgs.IedSourceRoot = $IedSourceRoot
            }

            & $IedBuildScript @iedArgs
            if ($LASTEXITCODE -ne 0) {
                throw "La compilation d'IED 1.7.4 patche a echoue."
            }
        }
    }
}

if (-not (Test-Path -LiteralPath $PatchedIedDll -PathType Leaf)) {
    throw "ImmersiveEquipmentDisplays.dll patche introuvable: $PatchedIedDll"
}
$PatchedIedDll = (Resolve-Path -LiteralPath $PatchedIedDll).Path

if (-not (Test-Path -LiteralPath $IedLicenseSource -PathType Leaf)) {
    throw "Licence IED introuvable: $IedLicenseSource"
}

if (Test-Path -LiteralPath $PackageRoot) {
    Remove-Item -LiteralPath $PackageRoot -Recurse -Force
}

New-Item -ItemType Directory -Force -Path $PluginRoot | Out-Null
Copy-Item -LiteralPath $dll.FullName -Destination (Join-Path $PluginRoot "IEDSyncTogether.dll") -Force
Copy-Item -LiteralPath $ConfigSource -Destination (Join-Path $PluginRoot "IEDSyncTogether.ini") -Force
Copy-Item -LiteralPath $PatchedIedDll -Destination (Join-Path $PluginRoot "ImmersiveEquipmentDisplays.dll") -Force
Write-MinimalPlugin (Join-Path $PackageRoot "Data\IEDSyncTogether.esp")

New-Item -ItemType Directory -Force -Path (Split-Path -Parent $IedLicenseDestination) | Out-Null
Copy-Item -LiteralPath $IedLicenseSource -Destination $IedLicenseDestination -Force

$DistRoot = Join-Path $ProjectRoot "dist"
New-Item -ItemType Directory -Force -Path $DistRoot | Out-Null

$Archive = Join-Path $DistRoot "IEDSyncTogether-v0.1.0.zip"
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
        "Data/SKSE/Plugins/IEDSyncTogether.ini",
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

Write-Host ""
Write-Host "Package Vortex complet cree:" -ForegroundColor Green
Write-Host $Archive
Write-Host "Inclut ImmersiveEquipmentDisplays.dll 1.7.4 patche pour IEDSyncTogether." -ForegroundColor Green
