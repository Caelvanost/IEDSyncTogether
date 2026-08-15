param(
    [Parameter(Mandatory = $true)]
    [string]$ImguiRoot,
    [string]$Commit = "03298fe875c0855097af6f79be631952054f984d"
)

$ErrorActionPreference = "Stop"

if (-not (Test-Path -LiteralPath $ImguiRoot -PathType Container)) {
    throw "ImGui checkout introuvable: $ImguiRoot"
}

# Normalize the checkout path before using .NET file APIs. PowerShell can
# preserve a caller-relative path while System.IO resolves it against a
# different process working directory when this script is invoked from a
# repository subdirectory.
$ImguiRoot = (Resolve-Path -LiteralPath $ImguiRoot -ErrorAction Stop).Path

$git = Get-Command git.exe -ErrorAction Stop
& $git.Source -C $ImguiRoot checkout $Commit
if ($LASTEXITCODE -ne 0) {
    throw "Impossible d'epingle ImGui sur $Commit."
}

$deployment = Join-Path $ImguiRoot "deployment\a"
New-Item -ItemType Directory -Force -Path $deployment | Out-Null
$project = Join-Path $deployment "imgui.vcxproj"

# IED 1.7.4 references this historical local deployment project, which is not
# committed in clayne/imgui. Recreate the one configuration needed by IED's
# Release MT Post 629 143 build as a static library.
$xml = @'
<?xml version="1.0" encoding="utf-8"?>
<Project DefaultTargets="Build" xmlns="http://schemas.microsoft.com/developer/msbuild/2003">
  <ItemGroup Label="ProjectConfigurations">
    <ProjectConfiguration Include="Release MT Post 629 143|x64">
      <Configuration>Release MT Post 629 143</Configuration>
      <Platform>x64</Platform>
    </ProjectConfiguration>
  </ItemGroup>
  <PropertyGroup Label="Globals">
    <VCProjectVersion>16.0</VCProjectVersion>
    <Keyword>Win32Proj</Keyword>
    <ProjectGuid>{9F316E83-5AE5-4939-A723-305A94F48005}</ProjectGuid>
    <RootNamespace>ImGui</RootNamespace>
    <WindowsTargetPlatformVersion>10.0</WindowsTargetPlatformVersion>
    <ProjectName>ImGui</ProjectName>
  </PropertyGroup>
  <Import Project="$(VCTargetsPath)\Microsoft.Cpp.Default.props" />
  <PropertyGroup Condition="'$(Configuration)|$(Platform)'=='Release MT Post 629 143|x64'" Label="Configuration">
    <ConfigurationType>StaticLibrary</ConfigurationType>
    <UseDebugLibraries>false</UseDebugLibraries>
    <PlatformToolset>v143</PlatformToolset>
    <WholeProgramOptimization>true</WholeProgramOptimization>
    <CharacterSet>Unicode</CharacterSet>
  </PropertyGroup>
  <Import Project="$(VCTargetsPath)\Microsoft.Cpp.props" />
  <ImportGroup Label="ExtensionSettings" />
  <ImportGroup Label="Shared" />
  <ImportGroup Label="PropertySheets" Condition="'$(Configuration)|$(Platform)'=='Release MT Post 629 143|x64'">
    <Import Project="$(UserRootDir)\Microsoft.Cpp.$(Platform).user.props" Condition="exists('$(UserRootDir)\Microsoft.Cpp.$(Platform).user.props')" Label="LocalAppDataPlatform" />
  </ImportGroup>
  <PropertyGroup Label="UserMacros" />
  <ItemDefinitionGroup Condition="'$(Configuration)|$(Platform)'=='Release MT Post 629 143|x64'">
    <ClCompile>
      <WarningLevel>Level3</WarningLevel>
      <FunctionLevelLinking>true</FunctionLevelLinking>
      <IntrinsicFunctions>true</IntrinsicFunctions>
      <SDLCheck>true</SDLCheck>
      <PreprocessorDefinitions>NDEBUG;_LIB;%(PreprocessorDefinitions)</PreprocessorDefinitions>
      <ConformanceMode>true</ConformanceMode>
      <RuntimeLibrary>MultiThreaded</RuntimeLibrary>
      <Optimization>MaxSpeed</Optimization>
      <LanguageStandard>stdcpp17</LanguageStandard>
      <AdditionalIncludeDirectories>$(ProjectDir)..\..;%(AdditionalIncludeDirectories)</AdditionalIncludeDirectories>
    </ClCompile>
    <Lib>
      <LinkTimeCodeGeneration>true</LinkTimeCodeGeneration>
    </Lib>
  </ItemDefinitionGroup>
  <ItemGroup>
    <ClCompile Include="..\..\imgui.cpp" />
    <ClCompile Include="..\..\imgui_demo.cpp" />
    <ClCompile Include="..\..\imgui_draw.cpp" />
    <ClCompile Include="..\..\imgui_tables.cpp" />
    <ClCompile Include="..\..\imgui_widgets.cpp" />
  </ItemGroup>
  <ItemGroup>
    <ClInclude Include="..\..\imconfig.h" />
    <ClInclude Include="..\..\imgui.h" />
    <ClInclude Include="..\..\imgui_internal.h" />
    <ClInclude Include="..\..\imstb_rectpack.h" />
    <ClInclude Include="..\..\imstb_textedit.h" />
    <ClInclude Include="..\..\imstb_truetype.h" />
  </ItemGroup>
  <Import Project="$(VCTargetsPath)\Microsoft.Cpp.targets" />
  <ImportGroup Label="ExtensionTargets" />
</Project>
'@

[System.IO.File]::WriteAllText($project, $xml, [System.Text.UTF8Encoding]::new($false))
Write-Host "Projet ImGui IED genere: $project" -ForegroundColor Green
