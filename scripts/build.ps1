<#
.SYNOPSIS
    Builds ConsoleBgTool end to end and packages a runnable version.

.DESCRIPTION
    Four stages, each of which must succeed before the next runs:

      1. native   compile the C core and the C++ platform layer as DLLs (P/Invoke surface)
                  and as static objects for the native test executables
      2. managed  build the interop library, the CLI and the test suite with the .NET SDK
      3. package  copy the native DLLs next to every executable that needs them and, with
                  -Publish, produce a self-contained folder under dist\
      4. test     run all three suites: C core, C++ platform, managed

    Everything is compiled with warnings as errors. The native layers enable -Wconversion and
    -Wsign-conversion because they translate between Win32's signed SHORT and the unsigned sizes
    the model uses; the managed projects enable the .NET analyzers at latest-recommended.

.PARAMETER MingwRoot
    Directory containing bin\gcc.exe and bin\g++.exe.

.PARAMETER Configuration
    Debug or Release. Release is what -Publish packages.

.PARAMETER Test
    Run all three test suites after building. Strongly recommended: the suites are what caught
    four real defects in the native layers, including one that wrote to the wrong registry key.

.PARAMETER Publish
    Produce dist\ConsoleBgTool-win-x64\ containing a self-contained ConsoleBgTool.exe and the
    native DLLs, runnable on a machine without the .NET runtime or MinGW installed.

.PARAMETER Clean
    Delete build\ and dist\ first.

.EXAMPLE
    powershell -NoProfile -ExecutionPolicy Bypass -File scripts\build.ps1 -Test

.EXAMPLE
    powershell -NoProfile -ExecutionPolicy Bypass -File scripts\build.ps1 -Clean -Test -Publish

.NOTES
    If PowerShell refuses to run the script ("running scripts is disabled on this system"), the
    machine's execution policy is Restricted. Run it with a one-off bypass instead of changing the
    machine policy:

        powershell -NoProfile -ExecutionPolicy Bypass -File scripts\build.ps1 -Test

    Works on Windows PowerShell 5.1 and PowerShell 7+.
#>
[CmdletBinding()]
param(
    # MinGW-w64 root. Override with -MingwRoot, or set the CBT_MINGW_ROOT environment variable.
    # The neutral default keeps this file free of any particular machine's layout.
    [string]$MingwRoot = $env:CBT_MINGW_ROOT,
    [ValidateSet('Debug', 'Release')]
    [string]$Configuration = 'Release',
    [switch]$Test,
    [switch]$Publish,
    [switch]$Clean
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

if ([string]::IsNullOrWhiteSpace($MingwRoot)) {
    $MingwRoot = 'C:\mingw64'
}

$root = Split-Path -Parent $PSScriptRoot
$native = Join-Path $root 'native'
$managed = Join-Path $root 'managed'
$build = Join-Path $root 'build'
$objDir = Join-Path $build 'obj'
$binDir = Join-Path $build 'bin'
$dist = Join-Path $root 'dist'

# ---- preconditions ---------------------------------------------------------

$gcc = Join-Path $MingwRoot 'bin\gcc.exe'
$gpp = Join-Path $MingwRoot 'bin\g++.exe'
foreach ($tool in @($gcc, $gpp)) {
    if (-not (Test-Path $tool)) {
        throw "toolchain not found: $tool (pass -MingwRoot with the directory containing bin\gcc.exe)"
    }
}

if (-not (Get-Command dotnet -ErrorAction SilentlyContinue)) {
    throw 'the .NET SDK (dotnet) was not found on PATH'
}

$env:Path = (Join-Path $MingwRoot 'bin') + ';' + $env:Path

if ($Clean) {
    foreach ($path in @($build, $dist)) {
        if (Test-Path $path) { Remove-Item -Recurse -Force $path }
    }
}
New-Item -ItemType Directory -Force -Path $objDir, $binDir | Out-Null

# ---- flags -----------------------------------------------------------------

$optimise = if ($Configuration -eq 'Release') { '-O2' } else { '-O0' }

# -D_WIN32_WINNT=0x0A00 targets Windows 10/11 so the console APIs used here are declared with
# their modern signatures.
$common = @(
    '-Wall', '-Wextra', '-Werror', '-Wpedantic', '-Wshadow',
    '-Wcast-qual', '-Wpointer-arith', '-Wwrite-strings', '-Wundef', '-Wformat=2',
    $optimise, '-g',
    '-D_WIN32_WINNT=0x0A00',
    '-I', (Join-Path $native 'include')
)

$cFlags = $common + @('-std=c11', '-Wstrict-prototypes', '-Wmissing-prototypes',
                      '-Wold-style-definition', '-Wconversion', '-Wsign-conversion')

$cxxFlags = $common + @('-std=c++17', '-Wconversion', '-Wsign-conversion',
                        '-Wnon-virtual-dtor', '-Wold-style-cast', '-Woverloaded-virtual',
                        '-Wdouble-promotion', '-DNOMINMAX')

# The GNU runtime is linked statically into every artefact. Without this the executables and DLLs
# depend on libgcc_s_seh-1.dll and libstdc++-6.dll, which are not present on a machine that has no
# MinGW installation: the DLL then fails with STATUS_DLL_NOT_FOUND (0x8007007E) and the P/Invoke
# surfaces as a bare DllNotFoundException with no useful detail.
$static = @('-static', '-static-libgcc', '-static-libstdc++')

$cCoreSources = @(
    'src/cbt_console.c'
    'src/cbt_registry.c'
    'src/cbt_registry_memory.c'
    'src/cbt_registry_win32.c'
) | ForEach-Object { Join-Path $native $_ }

$cxxPlatformSource = Join-Path $native 'src/cbt_platform.cpp'

function Invoke-Step {
    param([string]$Label, [string]$Command, [string[]]$Arguments)

    Write-Host "    $Label" -ForegroundColor DarkGray
    & $Command @Arguments
    if ($LASTEXITCODE -ne 0) {
        throw "$Label failed with exit code $LASTEXITCODE"
    }
}

function Resolve-ManagedOutput {
    param([string]$ProjectDirectory)
    $path = Join-Path $managed "$ProjectDirectory\bin\$Configuration\net10.0-windows\win-x64"
    if (-not (Test-Path $path)) {
        throw "expected build output not found: $path"
    }
    return $path
}

# ---- 1. native -------------------------------------------------------------

Write-Host '1. native layers' -ForegroundColor Cyan

$coreObjects = @()
foreach ($source in $cCoreSources) {
    $name = [System.IO.Path]::GetFileNameWithoutExtension($source)
    $obj = Join-Path $objDir "$name.o"
    Invoke-Step $name $gcc ($cFlags + @('-c', $source, '-o', $obj))
    $coreObjects += $obj
}

$platformObject = Join-Path $objDir 'cbt_platform.o'
Invoke-Step 'cbt_platform.cpp' $gpp ($cxxFlags + @('-c', $cxxPlatformSource, '-o', $platformObject))

# The P/Invoke surface: both layers as DLLs, plus import libraries so the C++ layer can link
# against the C layer.
$coreDll = Join-Path $binDir 'cbt_core.dll'
$platformDll = Join-Path $binDir 'cbt_platform.dll'
$coreImportLib = Join-Path $objDir 'libcbt_core.dll.a'

Invoke-Step 'cbt_core.dll' $gcc ($cFlags + @('-shared', '-o', $coreDll) + $cCoreSources +
    @("-Wl,--out-implib,$coreImportLib", '-ladvapi32'))

Invoke-Step 'cbt_platform.dll' $gpp ($cxxFlags + $static + @('-shared', '-o', $platformDll,
    $cxxPlatformSource, "-Wl,--out-implib,$(Join-Path $objDir 'libcbt_platform.dll.a')",
    $coreImportLib, '-ladvapi32', '-luser32'))

# ---- 2. managed ------------------------------------------------------------

Write-Host '2. managed layers' -ForegroundColor Cyan

$env:DOTNET_CLI_TELEMETRY_OPTOUT = '1'
$env:DOTNET_NOLOGO = '1'

$interopProject = Join-Path $managed 'ConsoleBgTool.Interop\ConsoleBgTool.Interop.csproj'
$cliProject = Join-Path $managed 'ConsoleBgTool.Cli\ConsoleBgTool.Cli.csproj'
$testsProject = Join-Path $managed 'ConsoleBgTool.Tests\ConsoleBgTool.Tests.csproj'

Invoke-Step 'ConsoleBgTool.Interop' 'dotnet' @('build', $interopProject, '-c', $Configuration, '-v', 'quiet', '--nologo')
Invoke-Step 'ConsoleBgTool.Cli' 'dotnet' @('build', $cliProject, '-c', $Configuration, '-v', 'quiet', '--nologo')
Invoke-Step 'ConsoleBgTool.Tests' 'dotnet' @('build', $testsProject, '-c', $Configuration, '-v', 'quiet', '--nologo')

# ---- 3. package ------------------------------------------------------------

Write-Host '3. package' -ForegroundColor Cyan

# Every managed executable needs the native DLLs beside it. The DllImport resolver deliberately
# looks only in the assembly's own directory - never in the working directory, an environment
# variable or PATH - so this copy is what makes the binaries self-contained and hijack-resistant.
$cliOutput = Resolve-ManagedOutput 'ConsoleBgTool.Cli'
$testsOutput = Resolve-ManagedOutput 'ConsoleBgTool.Tests'
foreach ($target in @($cliOutput, $testsOutput)) {
    Copy-Item $coreDll $target -Force
    Copy-Item $platformDll $target -Force
    Write-Host "    native DLLs -> $target" -ForegroundColor DarkGray
}

if ($Publish) {
    $publishDir = Join-Path $dist 'ConsoleBgTool-win-x64'
    New-Item -ItemType Directory -Force -Path $publishDir | Out-Null

    # Self-contained: the result runs without the .NET runtime installed.
    Invoke-Step 'dotnet publish' 'dotnet' @('publish', $cliProject, '-c', $Configuration,
        '-r', 'win-x64', '--self-contained', 'true', '-o', $publishDir, '-v', 'quiet', '--nologo')

    Copy-Item $coreDll $publishDir -Force
    Copy-Item $platformDll $publishDir -Force

    # Ship the licence and the handover notes alongside the binaries.
    foreach ($extra in @('HANDOFF.md', '..\LICENSE')) {
        $candidate = Join-Path $root $extra
        if (Test-Path $candidate) { Copy-Item $candidate $publishDir -Force }
    }

    $size = [math]::Round(((Get-ChildItem $publishDir -Recurse -File | Measure-Object Length -Sum).Sum / 1MB), 1)
    Write-Host "    published to $publishDir ($size MB)" -ForegroundColor DarkGray
}

# ---- 4. test ---------------------------------------------------------------

$cTestExe = Join-Path $binDir 'cbt_tests.exe'
$cxxTestExe = Join-Path $binDir 'cbt_platform_tests.exe'

Invoke-Step 'cbt_tests.exe' $gcc ($cFlags + $static + @('-o', $cTestExe) + $coreObjects +
    @((Join-Path $native 'tests\test_core.c'), '-ladvapi32'))

Invoke-Step 'cbt_platform_tests.exe' $gpp ($cxxFlags + $static + @('-o', $cxxTestExe, $platformObject) +
    $coreObjects + @((Join-Path $native 'tests\test_platform.cpp'), '-ladvapi32', '-luser32'))

if ($Test) {
    Write-Host '4. tests' -ForegroundColor Cyan

    $results = @()

    Write-Host '    C core' -ForegroundColor DarkGray
    & $cTestExe
    $results += @{ Name = 'C core'; Code = $LASTEXITCODE }

    Write-Host '    C++ platform' -ForegroundColor DarkGray
    & $cxxTestExe
    $results += @{ Name = 'C++ platform'; Code = $LASTEXITCODE }

    Write-Host '    managed' -ForegroundColor DarkGray
    & (Join-Path $testsOutput 'ConsoleBgTool.Tests.exe')
    $results += @{ Name = 'managed'; Code = $LASTEXITCODE }

    $failed = $results | Where-Object { $_.Code -ne 0 }
    if ($failed) {
        $summary = ($failed | ForEach-Object { "$($_.Name) exit $($_.Code)" }) -join ', '
        throw "tests failed: $summary"
    }
    Write-Host '    all suites passed' -ForegroundColor Green
}

# ---- summary ---------------------------------------------------------------

Write-Host ''
Write-Host 'Build succeeded' -ForegroundColor Green
Write-Host "  native DLLs     : $coreDll, $platformDll"
Write-Host "  native tests    : $cTestExe, $cxxTestExe"
Write-Host "  CLI             : $(Join-Path $cliOutput 'ConsoleBgTool.exe')"
Write-Host "  managed tests   : $(Join-Path $testsOutput 'ConsoleBgTool.Tests.exe')"
if ($Publish) {
    Write-Host "  package         : $(Join-Path $dist 'ConsoleBgTool-win-x64')"
}
