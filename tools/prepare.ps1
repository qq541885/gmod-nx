# prepare.ps1 -- 从 GDroid APK 生成 gmod_nx 的 SD 卡布局
#
# 用法:
#   .\prepare.ps1 -Apk .\GDroid-0.1.31.apk -Out D:\switch\gmod_nx
#   .\prepare.ps1 -Apk .\GDroid-0.1.31.apk -Out .\stage -NoLoader
#
# 产出:
#   <Out>\lib\           32 个 Source/Android ARM64 模块（不含 SDL2/Vulkan/zstd）
#   <Out>\config.txt     首次运行配置
#   <Out>\verify.txt     逐个校验结果
#   <Out>\gmod_nx.nro    只有 -Nro 指定时才复制
#
# 游戏数据（garrysmod/ sourceengine/ platform/）不在 APK 里，需自行放入。

param(
  [Parameter(Mandatory=$true)][string]$Apk,
  [Parameter(Mandatory=$true)][string]$Out,
  [string]$Nro = '',
  [switch]$NoLoader
)

$ErrorActionPreference = 'Stop'
Add-Type -AssemblyName System.IO.Compression.FileSystem

# 这 32 个进 lib/，顺序即加载顺序；SDL2 / Vulkan / zstd 由 loader 侧接管或跳过
$LOAD = @(
  'libc++_shared.so','libtier0.so','libvstdlib.so','libsteam_api.so','libtogl.so',
  'libphonon.so','libfilesystem_stdio.so','libinputsystem.so','libdatacache.so',
  'libscenefilecache.so','libsoundemittersystem.so','libvideo_services.so',
  'libvaudio_minimp3.so','libvaudio_opus.so','libvaudio_speex.so','libvphysics.so',
  'libstudiorender.so','libmaterialsystem.so','libshaderapidx9.so','libstdshader_dx9.so',
  'libvtex_dll.so','libvgui2.so','libvguimatsurface.so','libengine.so','libGameUI.so',
  'libServerBrowser.so','liblua_shared.so','libclient.so','libserver.so',
  'libopenxr_loader.so','libvrxr.so','liblauncher.so'
)
$SKIP = @('libSDL2.so','libshaderapivulkan.so','libzstd-jni-1.5.7-6.so')

if (-not (Test-Path $Apk)) { throw "APK not found: $Apk" }
$OutFull = [System.IO.Path]::GetFullPath($Out)
if (-not (Test-Path $OutFull)) { New-Item -ItemType Directory -Path $OutFull -Force | Out-Null }
$libDir = Join-Path $OutFull 'lib'
if (-not (Test-Path $libDir)) { New-Item -ItemType Directory -Path $libDir -Force | Out-Null }

Write-Host ("APK : {0}  ({1:N0} bytes)" -f $Apk, (Get-Item $Apk).Length)
Write-Host ("sha256: {0}" -f (Get-FileHash $Apk -Algorithm SHA256).Hash.ToLower())
Write-Host ("out : {0}" -f $OutFull)

$zip = [System.IO.Compression.ZipFile]::OpenRead((Resolve-Path $Apk).Path)
try {
  $entries = @{}
  foreach ($e in $zip.Entries) {
    if ($e.FullName -like 'lib/arm64-v8a/*.so') { $entries[(Split-Path $e.FullName -Leaf)] = $e }
  }
  Write-Host ("APK 内 arm64 .so: {0}" -f $entries.Count)

  $log = New-Object System.Collections.Generic.List[string]
  $log.Add("prepare.ps1 report")
  $log.Add("apk    : $Apk")
  $log.Add("sha256 : $((Get-FileHash $Apk -Algorithm SHA256).Hash.ToLower())")
  $log.Add("")

  foreach ($n in $LOAD) {
    if (-not $entries.ContainsKey($n)) {
      $log.Add("MISSING in APK : $n")
      Write-Host ("  MISSING  {0}" -f $n) -ForegroundColor Red
      continue
    }
    $dest = Join-Path $libDir $n
    [System.IO.Compression.ZipFileExtensions]::ExtractToFile($entries[$n], $dest, $true)
    $h = (Get-FileHash $dest -Algorithm SHA256).Hash.ToLower()
    $log.Add(("ok  {0,-30} {1,10:N0}  {2}" -f $n, (Get-Item $dest).Length, $h))
    Write-Host ("  ok       {0}" -f $n)
  }

  foreach ($n in $SKIP) {
    $why = switch ($n) {
      'libSDL2.so'            { 'switch-sdl2 提供同一批符号' }
      'libshaderapivulkan.so' { '无 Vulkan 驱动；改走 shaderapidx9 -> togl -> GLES' }
      default                 { 'Java 侧使用，无模块依赖' }
    }
    $log.Add("SKIP           : $n  ($why)")
  }

  $log.Add("")
  $log.Add("skipped: " + ($SKIP -join ', '))
  $log | Set-Content (Join-Path $OutFull 'verify.txt') -Encoding UTF8

  $cfg = Join-Path $OutFull 'config.txt'
  if (-not (Test-Path $cfg)) {
    $cfgText = @"
screen_width=-1
screen_height=-1
gamedir=garrysmod
args=
lang=english
gamepad=1
console=0
gyro_enabled=0
gyro_sensitivity_x=100
gyro_sensitivity_y=100
"@
    $cfgText | Set-Content $cfg -Encoding ASCII
    Write-Host "  wrote    config.txt"
  }

  if ($Nro -and -not $NoLoader) {
    if (-not (Test-Path $Nro)) { throw "NRO not found: $Nro" }
    Copy-Item $Nro (Join-Path $OutFull 'gmod_nx.nro') -Force
    Write-Host "  copied   gmod_nx.nro"
  }
}
finally { $zip.Dispose() }

Write-Host ""
Write-Host "下一步：把 Steam 版 Garry's Mod 的三个文件夹放进 $OutFull" -ForegroundColor Cyan
Write-Host "  garrysmod\   sourceengine\   platform\"
Write-Host "PC 的 bin\ 不要复制。"
Write-Host ""
Write-Host "报告: $(Join-Path $OutFull 'verify.txt')"
