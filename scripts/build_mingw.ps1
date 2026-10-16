# MinGW verification build. Mirrors scripts/build.ps1, but uses a plain gcc/g++ toolchain so the
# production sources and the native tests can still be compiled and linked on machines without the
# Visual Studio C++ workload (or when CMake's compiler probing is blocked).
#
# This is a verification path only: the release artifacts must be built with MSVC, because the
# reflective loader, the injector and the JNI entry points are validated against MSVC output.
#
#   ./scripts/build_mingw.ps1                     # compile + link + run the native tests
#   ./scripts/build_mingw.ps1 -SkipTests          # compile + link only
#   ./scripts/build_mingw.ps1 -OutDir ./build/other -Configuration Debug
param(
    [string]$OutDir = "$PSScriptRoot/../build/mingw-verify",
    [ValidateSet('Release', 'Debug')][string]$Configuration = 'Release',
    [switch]$SkipTests,
    [string]$MinGWBin
)
$ErrorActionPreference = 'Stop'
$root = (Resolve-Path "$PSScriptRoot/..").Path
$OutDir = [System.IO.Path]::GetFullPath($OutDir)

function Find-MinGW {
    param([string]$Hint)
    $candidates = @()
    if ($Hint) { $candidates += $Hint }
    # An already configured GCC on PATH wins: the caller may use MSYS2, llvm-mingw or WinLibs.
    foreach ($tool in 'g++', 'gcc') {
        $found = Get-Command $tool -ErrorAction SilentlyContinue
        if ($found) { $candidates += (Split-Path -Parent $found.Source) }
    }
    $winget = Join-Path $env:LOCALAPPDATA 'Microsoft/WinGet/Packages'
    if (Test-Path -LiteralPath $winget) {
        $candidates += (Get-ChildItem $winget -Directory -Filter '*WinLibs*' -ErrorAction SilentlyContinue |
            ForEach-Object { Get-ChildItem $_.FullName -Directory -Recurse -Depth 2 -Filter 'mingw64' -ErrorAction SilentlyContinue } |
            ForEach-Object { Join-Path $_.FullName 'bin' })
    }
    foreach ($candidate in $candidates) {
        if ($candidate -and (Test-Path -LiteralPath (Join-Path $candidate 'g++.exe'))) { return $candidate }
    }
    throw 'No MinGW toolchain found. Install one or pass -MinGWBin <path containing g++.exe>.'
}

$mingw = Find-MinGW -Hint $MinGWBin
$gcc = Join-Path $mingw 'gcc.exe'
$gxx = Join-Path $mingw 'g++.exe'
Write-Host "Toolchain: $mingw"
New-Item -ItemType Directory -Force $OutDir | Out-Null
$objDir = Join-Path $OutDir 'obj'
New-Item -ItemType Directory -Force $objDir | Out-Null

$defines = @('-DREFLECTIVEDLLINJECTION_CUSTOM_DLLMAIN', '-DWIN_X64', '-DNOMINMAX', '-D_CRT_SECURE_NO_WARNINGS')
$optimize = if ($Configuration -eq 'Debug') { '-O0' } else { '-O2' }

# Compiler output is captured instead of flowing into PowerShell's error stream: warnings are data,
# and only a non-zero exit code aborts the build.
function Invoke-Tool {
    param([string]$Exe, [string[]]$Arguments)
    $output = & $Exe @Arguments 2>&1
    if ($LASTEXITCODE -ne 0) {
        $output | Select-Object -Last 25 | ForEach-Object { Write-Host "    $_" }
        throw "$(Split-Path -Leaf $Exe) failed with exit code $LASTEXITCODE"
    }
}

# C sources must be compiled as C: g++ would mangle the reflective-loader and injector symbols.
function Compile-Object {
    param([string]$Source, [string[]]$Includes)
    $isC = $Source.EndsWith('.c')
    $compiler = if ($isC) { $gcc } else { $gxx }
    $object = Join-Path $objDir ((Split-Path -Leaf $Source) + '.o')
    $arguments = New-Object System.Collections.Generic.List[string]
    if ($isC) { $arguments.Add('-std=c11') } else { $arguments.Add('-std=c++17') }
    $arguments.Add($optimize)
    $arguments.Add('-c')
    $arguments.Add($Source)
    $arguments.Add('-o')
    $arguments.Add($object)
    foreach ($include in $Includes) { $arguments.Add($include) }
    foreach ($define in $defines) { $arguments.Add($define) }
    Invoke-Tool -Exe $compiler -Arguments $arguments.ToArray()
    return $object
}

# Aurora.dll: the resident proxy inside client A.
$productionSources = Get-Content (Join-Path $root 'native/sources.txt') |
    Where-Object { $_ -match '\.(cpp|c)$' } |
    ForEach-Object { Join-Path $root ($_ -replace '/', '\') }
if ($productionSources.Count -lt 10) { throw 'native/sources.txt looks incomplete.' }
$productionObjects = @()
foreach ($source in $productionSources) {
    $productionObjects += (Compile-Object $source @('-Inative', '-Inative/include'))
}
$auroraArgs = New-Object System.Collections.Generic.List[string]
$auroraArgs.Add('-shared')
$auroraArgs.Add('-o')
$auroraArgs.Add((Join-Path $OutDir 'Aurora.dll'))
foreach ($object in $productionObjects) { $auroraArgs.Add($object) }
foreach ($lib in @('-lpsapi', '-lws2_32', '-lkernel32', '-luser32', '-ladvapi32')) { $auroraArgs.Add($lib) }
Invoke-Tool -Exe $gxx -Arguments $auroraArgs.ToArray()
Write-Host '  ok  Aurora.dll'

# Compass.exe and Prism.dll share the same injection implementation.
$injectionSources = @('injector/Inject.c', 'injector/LoadLibraryR.c', 'injector/GetProcAddressR.c') |
    ForEach-Object { Join-Path $root ($_ -replace '/', '\') }
$injectionObjects = @()
foreach ($source in $injectionSources) {
    $injectionObjects += (Compile-Object $source @('-Iinjector'))
}
$compassArgs = New-Object System.Collections.Generic.List[string]
$compassArgs.Add($optimize)
$compassArgs.Add('-o')
$compassArgs.Add((Join-Path $OutDir 'Compass.exe'))
$compassArgs.Add('-Iinjector')
foreach ($define in $defines) { $compassArgs.Add($define) }
$compassArgs.Add((Join-Path $root 'injector/cli.c'))
foreach ($object in $injectionObjects) { $compassArgs.Add($object) }
foreach ($lib in @('-ladvapi32', '-liphlpapi', '-lkernel32', '-luser32')) { $compassArgs.Add($lib) }
Invoke-Tool -Exe $gcc -Arguments $compassArgs.ToArray()
Write-Host '  ok  Compass.exe'

$prismObject = Compile-Object (Join-Path $root 'native/jni_inject.cpp') @('-Inative/include', '-Iinjector')
$prismArgs = New-Object System.Collections.Generic.List[string]
$prismArgs.Add('-shared')
$prismArgs.Add('-o')
$prismArgs.Add((Join-Path $OutDir 'Prism.dll'))
$prismArgs.Add($prismObject)
foreach ($object in $injectionObjects) { $prismArgs.Add($object) }
foreach ($lib in @('-lkernel32', '-luser32', '-ladvapi32')) { $prismArgs.Add($lib) }
Invoke-Tool -Exe $gxx -Arguments $prismArgs.ToArray()
Write-Host '  ok  Prism.dll'

if ($SkipTests) {
    Write-Host "Build complete (tests skipped). Verification binaries: $OutDir"
    return
}

# The native tests mirror the CTest set that does not need a JVM.
$testDir = Join-Path $OutDir 'tests'
New-Item -ItemType Directory -Force $testDir | Out-Null
$native = Join-Path $root 'native'
$cases = @(
    @{ Name = 'login_handoff'; Sources = @("$root/tests/login_handoff_test.cpp") },
    @{ Name = 'runtime_gate'; Sources = @("$root/tests/runtime_gate_test.cpp") },
    @{ Name = 'packet_policy'; Sources = @("$root/tests/packet_policy_test.cpp") },
    @{ Name = 'team_state'; Sources = @("$root/tests/team_state_test.cpp") },
    @{ Name = 'class_editor'; Sources = @("$root/tests/edit_self_test.cpp", "$native/classfile.cpp", "$native/class_edit.cpp") },
    @{ Name = 'class_interface'; Sources = @("$root/tests/class_interface_test.cpp", "$native/classfile.cpp") }
)
$failed = @()
foreach ($case in $cases) {
    $exe = Join-Path $testDir ("{0}_test.exe" -f $case.Name)
    $build = & $gxx @('-std=c++17', '-O1', "-I$native", '-o', $exe) $case.Sources 2>&1
    if ($LASTEXITCODE -ne 0) {
        $build | Select-Object -Last 15 | ForEach-Object { Write-Host "    $_" }
        throw "building $($case.Name) failed"
    }
    $output = & $exe 2>&1
    if ($LASTEXITCODE -eq 0) {
        Write-Host ("  PASS  {0,-15} {1}" -f $case.Name, ($output -join ' | '))
    } else {
        Write-Host ("  FAIL  {0,-15} {1}" -f $case.Name, ($output -join ' | '))
        $failed += $case.Name
    }
}
if ($failed.Count -gt 0) { throw "tests failed: $($failed -join ', ')" }
Write-Host "Build complete. Verification binaries: $OutDir (tests: $testDir)"
