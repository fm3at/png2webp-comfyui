$ErrorActionPreference = "Stop"
$gcc = "path\to\gcc.exe"
$root = "path\to\png2webp\sources"
$inc1 = "$root\third_party"
$inc2 = "$root\third_party\libwebp"
$inc3 = "$root\third_party\libwebp\src"
$dsp  = "$root\third_party\libwebp\src\dsp"
$obj  = "$root\obj"
New-Item -ItemType Directory -Force -Path $obj | Out-Null

# ---------------- dependencies (auto-download if missing) ----------------
[Net.ServicePointManager]::SecurityProtocol = [Net.ServicePointManager]::SecurityProtocol -bor [Net.SecurityProtocolType]::Tls12

function Download-File([string]$url, [string]$dest) {
    Write-Host "  downloading $url"
    Invoke-WebRequest -Uri $url -OutFile $dest -UseBasicParsing
}

$stb = "$root\third_party\stb_image.h"
if (-not (Test-Path -LiteralPath $stb)) {
    New-Item -ItemType Directory -Force -Path "$root\third_party" | Out-Null
    Download-File "https://raw.githubusercontent.com/nothings/stb/master/stb_image.h" $stb
}

$lpCheck = "$root\third_party\libwebp\src\enc\webp_enc.c"
if (-not (Test-Path -LiteralPath $lpCheck)) {
    if (-not (Get-Command tar -ErrorAction SilentlyContinue)) {
        Write-Error "tar.exe not found (required to unpack libwebp)"; exit 1
    }
    $tmp = "$root\third_party\_dl"
    if (Test-Path $tmp) { Remove-Item -Recurse -Force $tmp }
    New-Item -ItemType Directory -Force -Path $tmp | Out-Null
    $tgz = "$tmp\libwebp-1.5.0.tar.gz"
    Download-File "https://github.com/webmproject/libwebp/archive/refs/tags/v1.5.0.tar.gz" $tgz
    & tar -xzf $tgz -C $tmp
    $extracted = Get-ChildItem $tmp -Directory | Where-Object { $_.Name -eq "libwebp-1.5.0" }
    if (-not $extracted) { Write-Error "libwebp archive extracted unexpectedly"; exit 1 }
    $destDir = "$root\third_party\libwebp"
    if (Test-Path $destDir) { Remove-Item -Recurse -Force $destDir }
    Move-Item -Path $extracted.FullName -Destination $destDir
    Remove-Item -Recurse -Force $tmp
    if (-not (Test-Path -LiteralPath $lpCheck)) { Write-Error "libwebp download failed"; exit 1 }
    Write-Host "  libwebp 1.5.0 ready"
}

$enc  = Get-ChildItem "$root\third_party\libwebp\src\enc\*.c"   | Where-Object { $_.Name -ne "picture_csp_enc.c" } | Select-Object -ExpandProperty FullName
$util = Get-ChildItem "$root\third_party\libwebp\src\utils\*.c" | Select-Object -ExpandProperty FullName

function Compile([string[]]$flags, [string[]]$files, [string]$tag) {
    $objs = @()
    foreach ($f in $files) {
        $o = "$obj\" + [System.IO.Path]::GetFileNameWithoutExtension($f) + "_" + $tag + ".o"
        & $gcc @flags @("-Os", "-ffunction-sections", "-fdata-sections", "-c", $f, "-o", $o, "-I", $inc1, "-I", $inc2, "-I", $inc3)
        if ($LASTEXITCODE -ne 0) { Write-Error "compile failed: $f"; exit 1 }
        $objs += $o
    }
    return $objs
}

$common = @()

$base = @("alpha_processing.c","cost.c","cpu.c","dec.c","dec_clip_tables.c",
          "enc.c","filters.c","lossless.c","lossless_enc.c","rescaler.c",
          "ssim.c","upsampling.c","yuv.c") | ForEach-Object { Join-Path $dsp $_ }
$sse2 = @("alpha_processing_sse2.c","cost_sse2.c","dec_sse2.c","enc_sse2.c",
          "filters_sse2.c","lossless_sse2.c","lossless_enc_sse2.c",
          "rescaler_sse2.c","ssim_sse2.c","upsampling_sse2.c","yuv_sse2.c") | ForEach-Object { Join-Path $dsp $_ }
$sse41 = @("dec_sse41.c","enc_sse41.c","lossless_enc_sse41.c",
           "lossless_sse41.c","upsampling_sse41.c","yuv_sse41.c") | ForEach-Object { Join-Path $dsp $_ }

Write-Host "Compiling dsp base..."
$objs = Compile @() $base "base"
Write-Host "Compiling dsp sse2..."
$objs += Compile @("-msse2") $sse2 "sse2"
Write-Host "Compiling dsp sse41..."
$objs += Compile @("-msse4.1") $sse41 "sse41"

Write-Host "Compiling main + libwebp enc/utils..."
$rest = @("$root\png2webp.c","$root\stb_impl.c","$root\csp_shim.c") + $enc + $util
$objs += Compile @() $rest "main"

Write-Host "Linking..."
& $gcc @("-Os","-municode","-mconsole","-static","-s",
          "-Wl,--gc-sections","-Wl,--exclude-libs,ALL",
          "-o","$root\png2webp.exe") $objs
if ($LASTEXITCODE -ne 0) { Write-Error "link failed"; exit 1 }
Get-Item "$root\png2webp.exe" | Select-Object Name, Length

Write-Host "Compressing with UPX..."
& upx -9 --lzma "$root\png2webp.exe"
if ($LASTEXITCODE -ne 0) { Write-Error "upx failed"; exit 1 }
Get-Item "$root\png2webp.exe" | Select-Object Name, Length
