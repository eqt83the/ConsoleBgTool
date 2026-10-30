<#
.SYNOPSIS
    Builds and tests ConsoleBgTool's native layers (C core + C++ platform).

.DESCRIPTION
    Compiles the C core and the C++ platform layer with a MinGW-w64 toolchain, links the two test
    executables, and optionally runs them. Every warning is an error, including -Wconversion and
    -Wsign-conversion, because most of this code converts between Win32's signed SHORT and the
    unsigned sizes the model uses, and a silent truncation there would corrupt a console.

    The GNU runtime is linked statically so the produced executables run on a machine that has no
    MinGW installed. Do not remove -static: without it the binaries need libstdc++-6.dll and
    libgcc_s_seh-1.dll and fail with STATUS_DLL_NOT_FOUND (0xC0000135).

.PARAMETER MingwRoot
    Directory containing bin\gcc.exe and bin\g++.exe.

.PARAMETER Configuration
    Debug or Release. Release is optimised; Debug keeps assertions and full symbols.

.PARAMETER Test
    Run both test suites after building.

.PARAMETER Clean
    Delete the build directory first.

.EXAMPLE
    pwsh -File scripts\build-native.ps1 -Test

.EXAMPLE
    pwsh -File scripts\build-native.ps1 -MingwRoot 'C:\mingw64' -Configuration Debug -Clean -Test

.NOTES
    If PowerShell refuses to run the script with "running scripts is disabled on this system", the
    machine's execution policy is Restricted. Run it with a one-off bypass instead of changing the
    machine policy:

        powershell -NoProfile -ExecutionPolicy Bypass -File scripts\build-native.ps1 -Test

    This script works on both Windows PowerShell 5.1 and PowerShell 7+.
#>
[CmdletBinding()]
param(
    # MinGW-w64 root. Override with -MingwRoot, or set the CBT_MINGW_ROOT environment variable.
    # The neutral default keeps this file free of any particular machine's layout.
    [string]$MingwRoot = $env:CBT_MINGW_ROOT,
    [ValidateSet('Debug', 'Release')]
    [string]$Configuration = 'Release',
    [switch]$Test,
    [switch]$Clean
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

if ([string]::IsNullOrWhiteSpace($MingwRoot)) {
    $MingwRoot = 'C:\mingw64'
}

$root = Split-Path -Parent $PSScriptRoot
$native = Join-Path $root 'native'
$build = Join-Path $root 'build'
$objDir = Join-Path $build 'obj'
$binDir = Join-Path $build 'bin'

# ---- toolchain -------------------------------------------------------------

$gcc = Join-Path $MingwRoot 'bin\gcc.exe'
$gpp = Join-Path $MingwRoot 'bin\g++.exe'
foreach ($tool in @($gcc, $gpp)) {
    if (-not (Test-Path $tool)) {
        throw "toolchain not found: $tool (pass -MingwRoot with the directory containing bin\gcc.exe)"
    }
}

# Put the toolchain first on PATH so its own DLLs and subprograms resolve consistently.
$env:Path = (Join-Path $MingwRoot 'bin') + ';' + $env:Path

if ($Clean -and (Test-Path $build)) {
    Remove-Item -Recurse -Force $build
}
New-Item -ItemType Directory -Force -Path $objDir, $binDir | Out-Null

# ---- flags -----------------------------------------------------------------

$optimise = if ($Configuration -eq 'Release') { '-O2' } else { '-O0' }

# Shared by both languages. -D_WIN32_WINNT=0x0A00 targets Windows 10/11 so the console and DWM
# APIs used here are declared with their modern signatures.
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

# Static GNU runtime: makes the executables self-contained.
$linkFlags = @('-static', '-static-libgcc', '-static-libstdc++')

# ---- sources ---------------------------------------------------------------

$cCore = @(
    'src/cbt_console.c'
    'src/cbt_registry.c'
    'src/cbt_registry_memory.c'
    'src/cbt_registry_win32.c'
) | ForEach-Object { Join-Path $native $_ }

$cxxPlatform = Join-Path $native 'src/cbt_platform.cpp'
$cTests = Join-Path $native 'tests/test_core.c'
$cxxTests = Join-Path $native 'tests/test_platform.cpp'

# ---- build -----------------------------------------------------------------

$objects = @{}

function Invoke-Step {
    param([string]$Label, [string[]]$Command, [string[]]$Arguments)

    Write-Host "  $Label" -ForegroundColor DarkGray
    & $Command @Arguments
    if ($LASTEXITCODE -ne 0) {
        throw "$Label failed with exit code $LASTEXITCODE"
    }
}

Write-Host 'Building C core (GCC, C11)' -ForegroundColor Cyan
foreach ($source in $cCore) {
    $name = [System.IO.Path]::GetFileNameWithoutExtension($source)
    $obj = Join-Path $objDir "$name.o"
    Invoke-Step $name @($gcc) ($cFlags + @('-c', $source, '-o', $obj))
    $objects[$name] = $obj
}

Write-Host 'Building C++ platform layer (G++, C++17)' -ForegroundColor Cyan
$platformObj = Join-Path $objDir 'cbt_platform.o'
Invoke-Step 'cbt_platform.cpp' @($gpp) ($cxxFlags + @('-c', $cxxPlatform, '-o', $platformObj))

Write-Host 'Building tests' -ForegroundColor Cyan
$cTestObj = Join-Path $objDir 'test_core.o'
Invoke-Step 'test_core.c' @($gcc) ($cFlags + @('-c', $cTests, '-o', $cTestObj))

$cxxTestObj = Join-Path $objDir 'test_platform.o'
Invoke-Step 'test_platform.cpp' @($gpp) ($cxxFlags + @('-c', $cxxTests, '-o', $cxxTestObj))

$coreObjs = $cCore | ForEach-Object { $objects[[System.IO.Path]::GetFileNameWithoutExtension($_)] }

$cTestExe = Join-Path $binDir 'cbt_tests.exe'
Invoke-Step 'link cbt_tests.exe' @($gcc) ($linkFlags + @('-o', $cTestExe) + $coreObjs + @($cTestObj, '-ladvapi32'))

$cxxTestExe = Join-Path $binDir 'cbt_platform_tests.exe'
Invoke-Step 'link cbt_platform_tests.exe' @($gpp) ($linkFlags + @('-o', $cxxTestExe, $platformObj) + $coreObjs + @($cxxTestObj, '-ladvapi32', '-luser32'))

Write-Host ''
Write-Host 'Build succeeded:' -ForegroundColor Green
Write-Host "  $cTestExe"
Write-Host "  $cxxTestExe"

# ---- test ------------------------------------------------------------------

if ($Test) {
    Write-Host ''
    Write-Host 'Running native tests' -ForegroundColor Cyan

    Write-Host '  C core' -ForegroundColor DarkGray
    & $cTestExe
    $cResult = $LASTEXITCODE

    Write-Host '  C++ platform' -ForegroundColor DarkGray
    & $cxxTestExe
    $cxxResult = $LASTEXITCODE

    if ($cResult -ne 0 -or $cxxResult -ne 0) {
        throw "tests failed (C core exit $cResult, C++ platform exit $cxxResult)"
    }
    Write-Host ''
    Write-Host 'All native tests passed.' -ForegroundColor Green
}
