# Builds out/shaderpack/cc1.pack from your own game files once the game is built: the shaders the deltas CSV lists,
# rebuilt from the title update's precache and the game image and translated to DXIL.
param(
    [string]$Misses = '',   # folder of missed-shader dumps (the game's gpu_dump_misses), listed in the deltas CSV first
    [string]$BuildDir = ''  # game build folder whose crashcourse1.exe dumps the image
)
$ErrorActionPreference = 'Continue'
if (-not ((Get-Command cmake -ErrorAction SilentlyContinue) -and (Get-Command clang -ErrorAction SilentlyContinue) -and
          (Get-Command ninja -ErrorAction SilentlyContinue))) {
    # Any 2022 edition will do, as long as it has the C++ workload with Clang.
    $vs = Get-ChildItem 'C:\Program Files\Microsoft Visual Studio\2022' -Directory -ErrorAction SilentlyContinue |
          Where-Object { Test-Path "$($_.FullName)\VC\Tools\Llvm\x64\bin\clang.exe" } |
          Select-Object -First 1 -ExpandProperty FullName
    if (-not $vs) { 'Visual Studio 2022 with the C++ workload not found; run this from a Developer PowerShell'; exit 1 }
    $env:PATH = "$vs\VC\Tools\Llvm\x64\bin;$vs\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin;$vs\Common7\IDE\CommonExtensions\Microsoft\CMake\Ninja;$env:PATH"
}
$root = (Resolve-Path "$PSScriptRoot\..\..").Path
if (-not $BuildDir) {
    # The most recently built game is the one the image comes from.
    $newest = Get-ChildItem "$root\out\build\*\crashcourse1.exe" -ErrorAction SilentlyContinue |
              Sort-Object LastWriteTime -Descending | Select-Object -First 1
    if (-not $newest) { 'crashcourse1.exe not found under out\build - build the game first'; exit 1 }
    $BuildDir = $newest.DirectoryName
}
if (-not (Test-Path "$BuildDir\crashcourse1.exe")) { "crashcourse1.exe missing in $BuildDir - build the game first"; exit 1 }
$BuildDir = (Resolve-Path $BuildDir).Path
$game = "$BuildDir\crashcourse1.exe"
$xrDir = "$root\tools\XenosRecomp"
$xrBuild = "$root\out\build\xenosrecomp"
$xr = "$xrBuild\XenosRecomp\XenosRecomp.exe"
$header = "$xrDir\XenosRecomp\rexglue_shader_common.h"
$deltas = "$root\tools\shaderpack\cc1_shader_deltas.csv"
$image = "$root\out\cc1_image.bin"
$pack = "$root\out\shaderpack\cc1.pack"
# The game reads the precache out of the title update's pack, which shadows the base game's copy.
$update = "$root\assets\DCC1-TU\patch.pak"
if (-not (Test-Path $update)) { $update = "$root\assets\patch.pak" }

if (-not (Test-Path $update)) { 'assets\DCC1-TU\patch.pak missing - the title update goes in assets\DCC1-TU'; exit 1 }
if (-not (Test-Path "$root\assets\default.xex")) { 'assets\default.xex missing - the extracted game goes in assets'; exit 1 }
if (-not (Test-Path "$root\assets\default.xexp")) { 'assets\default.xexp missing - the title update patch goes beside default.xex'; exit 1 }
if ($Misses -and -not (Test-Path $Misses -PathType Container)) { "misses folder not found: $Misses"; exit 1 }
# A game built without the setting would start normally instead of dumping.
$exeText = [System.Text.Encoding]::ASCII.GetString([System.IO.File]::ReadAllBytes($game))
if (-not $exeText.Contains('dump_image_path')) { "crashcourse1.exe in $BuildDir has no dump_image_path setting - rebuild the game"; exit 1 }
$exeText = $null

'=== image dump (the game writes its decrypted, patched image and exits)'
New-Item -ItemType Directory -Force "$root\out" | Out-Null
Remove-Item $image -ErrorAction SilentlyContinue
$dump = Start-Process -FilePath $game -ArgumentList "`"--dump_image_path=$image`"", '--fullscreen=false', '--cc1_online=false' -WorkingDirectory $BuildDir -Wait -PassThru
if ($dump.ExitCode -ne 0 -or -not (Test-Path $image) -or (Get-Item $image).Length -eq 0) { 'image dump failed - check that crashcourse1.toml in the build folder starts the game'; exit 1 }

'=== XenosRecomp build'
if (-not (Test-Path "$xrBuild\build.ninja")) {
    cmake -S $xrDir -B $xrBuild -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++ | Select-Object -Last 2
    if ($LASTEXITCODE -ne 0) { 'XenosRecomp configure failed'; exit 1 }
}
cmake --build $xrBuild | Select-Object -Last 2
if ($LASTEXITCODE -ne 0) { 'XenosRecomp build failed'; exit 1 }

'=== shader pack'
if ($Misses) {
    # A miss the game compiles at run time has no source in the game files, so its row carries the whole shader.
    & $xr --cc1-add-misses $deltas $Misses $image $update
    if ($LASTEXITCODE -ne 0) { 'adding the missed shaders failed'; exit 1 }
}
# Each CSV row rebuilds one runtime shader: a container or image code from the game files plus the listed word
# patches, or every word of a shader the game compiles at run time.
& $xr --cc1-pack $deltas $header $pack $image $update
$built = $LASTEXITCODE
Remove-Item $image -ErrorAction SilentlyContinue
if ($built -ne 0) { 'pack generation failed'; exit 1 }

# The game looks for shaderpack\cc1.pack beside its exe when gpu_shader_pack is unset.
New-Item -ItemType Directory -Force "$BuildDir\shaderpack" | Out-Null
Copy-Item $pack "$BuildDir\shaderpack\cc1.pack" -Force
"shader pack ready: $pack (copied to $BuildDir\shaderpack\cc1.pack)"
