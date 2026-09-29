param(
  [switch]$Clean,
  [switch]$Debug,
  [int]$Jobs = 0
)

$ErrorActionPreference = "Stop"

$Root = Split-Path -Parent $MyInvocation.MyCommand.Path
$MingwBin = "C:\mingw64\bin"
$Gcc = Join-Path $MingwBin "gcc.exe"
$Gxx = Join-Path $MingwBin "g++.exe"
$Windres = Join-Path $MingwBin "windres.exe"
$Output = Join-Path $Root "auto_hack_7in1.exe"
$Configuration = if ($Debug) { "Debug" } else { "Release" }
$BuildDir = Join-Path $Root ("build-mingw-" + $Configuration.ToLowerInvariant())

foreach ($compiler in @($Gcc, $Gxx, $Windres)) {
  if (-not (Test-Path -LiteralPath $compiler)) {
    throw "Required MinGW tool not found: $compiler"
  }
}

function Resolve-RequiredCommand([string]$name) {
  $command = Get-Command $name -ErrorAction SilentlyContinue
  if (-not $command) {
    throw "$name was not found in PATH. Install CMake and Ninja, then restart the terminal."
  }
  return $command.Source
}

$Cmake = Resolve-RequiredCommand "cmake.exe"
$Ninja = Resolve-RequiredCommand "ninja.exe"
$env:PATH = "$MingwBin;$env:PATH"

if ($Clean) {
  Remove-Item -LiteralPath $BuildDir -Recurse -Force -ErrorAction SilentlyContinue
  Remove-Item -LiteralPath $Output -Force -ErrorAction SilentlyContinue
}

$configureArgs = @(
  "-S", $Root,
  "-B", $BuildDir,
  "-G", "Ninja",
  "-DCMAKE_BUILD_TYPE=$Configuration",
  "-DCMAKE_C_COMPILER=$($Gcc.Replace('\', '/'))",
  "-DCMAKE_CXX_COMPILER=$($Gxx.Replace('\', '/'))",
  "-DCMAKE_RC_COMPILER=$($Windres.Replace('\', '/'))",
  "-DCMAKE_MAKE_PROGRAM=$($Ninja.Replace('\', '/'))"
)

Write-Host "Configuring $Configuration with MinGW + Ninja..."
& $Cmake @configureArgs
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }

$ResourceScript = Join-Path $Root "src\resources\app.rc"
if (-not (Test-Path -LiteralPath $ResourceScript)) {
  throw "Resource script was not found: $ResourceScript"
}
# windres does not reliably report dependencies for the language .rc files
# included by app.rc, so force only the resource object to rebuild each time.
(Get-Item -LiteralPath $ResourceScript).LastWriteTime = Get-Date

$buildArgs = @("--build", $BuildDir, "--parallel")
if ($Jobs -gt 0) { $buildArgs += $Jobs }
Write-Host "Building in parallel..."
& $Cmake @buildArgs
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }

$BuiltOutput = Join-Path $BuildDir "src\auto_hack_7in1.exe"
if (-not (Test-Path -LiteralPath $BuiltOutput)) {
  throw "Build completed but output was not found: $BuiltOutput"
}
Copy-Item -LiteralPath $BuiltOutput -Destination $Output -Force

Write-Host "Build succeeded: $Output"
