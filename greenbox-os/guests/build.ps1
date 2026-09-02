<#
    build.ps1 - build every guest program.

    Deliberately not an IDF project. A guest is compiled -nostdlib against the
    bare cross-compiler and linked with guest.ld, which is what keeps it a few
    kilobytes instead of the ~180 KB a standalone IDF app would cost.

    Each subdirectory here (other than gsdk and out) is one program: all of its
    .c files, plus the tiny runtime in gsdk, linked into out\<dir>.gbx.

    Run this BEFORE building the OS - the OS packs out\ into its SPIFFS image.
#>

$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $MyInvocation.MyCommand.Definition
$abi  = Join-Path (Split-Path -Parent $root) 'abi'
$gsdk = Join-Path $root 'gsdk'
$out  = Join-Path $root 'out'
$mkguest = Join-Path (Split-Path -Parent $root) 'tools\mkguest.py'

# --- toolchain -----------------------------------------------------------
$tcRoot = Join-Path $env:USERPROFILE '.espressif\tools\xtensa-esp-elf'
$gcc = Get-ChildItem -Path $tcRoot -Recurse -Filter 'xtensa-esp32-elf-gcc.exe' `
                     -ErrorAction SilentlyContinue | Select-Object -First 1
if (-not $gcc) {
    throw "xtensa-esp32-elf-gcc.exe not found under $tcRoot - see TOOLCHAIN.md"
}
$GCC = $gcc.FullName

$python = 'python'
if (Test-Path "$env:USERPROFILE\.espressif\python_env\idf5.2_py3.11_env\Scripts\python.exe") {
    $python = "$env:USERPROFILE\.espressif\python_env\idf5.2_py3.11_env\Scripts\python.exe"
}

Write-Host "gcc    $GCC"
Write-Host "python $python"

# --- flags ---------------------------------------------------------------
# -mlongcalls routes calls through literal pools, so nothing depends on how far
#   apart two functions land.
# -mtext-section-literals keeps those pools inside .text, where an l32r can
#   still reach them PC-relative after the loader moves the image.
# --no-relax because linker relaxation rewrites instructions after the
#   relocations are recorded, and --emit-relocs is only trustworthy without it.
$CFLAGS = @(
    '-mlongcalls', '-mtext-section-literals',
    '-Os', '-g0', '-std=gnu11', '-Wall', '-Wextra',
    '-ffunction-sections', '-fdata-sections', '-fno-common',
    '-fno-builtin', '-fno-jump-tables',
    "-I$abi", "-I$gsdk"
)

$LDFLAGS = @(
    '-nostdlib', '-nostartfiles',
    '-Wl,--gc-sections', '-Wl,--emit-relocs', '-Wl,--no-relax',
    '-Wl,-e,gb_main',
    "-Wl,-T,$(Join-Path $gsdk 'guest.ld')"
)

New-Item -ItemType Directory -Force -Path $out | Out-Null
$build = Join-Path $root '.build'
New-Item -ItemType Directory -Force -Path $build | Out-Null

# --- the shared runtime --------------------------------------------------
# Every .c in gsdk, linked into every program: the seven libc functions GCC
# assumes exist, and the surface that api->gfx draws into. --gc-sections drops
# whichever of them a given program never calls.
#
# -fno-tree-loop-distribute-patterns stops -Os from turning the loop inside
# memset into a call to memset.
$rtObjs = @()
foreach ($src in Get-ChildItem -Path $gsdk -Filter '*.c') {
    $obj = Join-Path $build "gsdk-$($src.BaseName).o"
    & $GCC @CFLAGS '-fno-tree-loop-distribute-patterns' '-c' $src.FullName '-o' $obj
    if ($LASTEXITCODE -ne 0) { throw "$($src.Name) failed to compile" }
    $rtObjs += $obj
}

# --- each program --------------------------------------------------------
# A directory with no .c in it is a program someone has started thinking about
# and not yet written. Skipping it beats failing the build for every other
# guest with "no .text - check that gb_main is reachable", which is true but
# unhelpful when the real answer is that the folder is empty.
$programs = Get-ChildItem -Path $root -Directory |
            Where-Object { $_.Name -notin @('gsdk', 'out', '.build') } |
            Where-Object { Get-ChildItem -Path $_.FullName -Filter '*.c' -File }

if (-not $programs) { Write-Host 'no programs to build'; exit 0 }

foreach ($p in $programs) {
    $name = $p.Name
    $objs = @($rtObjs)

    foreach ($src in Get-ChildItem -Path $p.FullName -Filter '*.c') {
        $obj = Join-Path $build "$name-$($src.BaseName).o"
        & $GCC @CFLAGS '-c' $src.FullName '-o' $obj
        if ($LASTEXITCODE -ne 0) { throw "$($src.Name) failed to compile" }
        $objs += $obj
    }

    $elf = Join-Path $build "$name.elf"
    & $GCC @LDFLAGS "-Wl,-Map,$(Join-Path $build "$name.map")" `
          @objs '-o' $elf '-lgcc'
    if ($LASTEXITCODE -ne 0) { throw "$name failed to link" }

    & $python $mkguest $elf '-o' (Join-Path $out "$name.gbx") '-n' $name -v
    if ($LASTEXITCODE -ne 0) { throw "$name failed to convert" }
}

Write-Host ''
Write-Host 'out\:'
Get-ChildItem $out -Filter '*.gbx' |
    ForEach-Object { Write-Host ("  {0,-16} {1,6} B" -f $_.Name, $_.Length) }
