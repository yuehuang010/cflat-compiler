param(
    [string]$Version = "0.1.0"
)

$ErrorActionPreference = "Stop"

$releaseDir = "$PSScriptRoot\x64\Release"
$publishDir = "$PSScriptRoot\out\publish"

if (-not (Test-Path "$releaseDir\cflat.exe")) {
    Write-Error "cflat.exe not found at $releaseDir - run a Release build first."
}

if (Test-Path $publishDir) { Remove-Item $publishDir -Recurse -Force }
New-Item -ItemType Directory $publishDir | Out-Null

Copy-Item "$releaseDir\cflat.exe"    "$publishDir\"
Copy-Item "$releaseDir\LTO.dll"      "$publishDir\"
Copy-Item "$releaseDir\Remarks.dll"  "$publishDir\"
Copy-Item "$releaseDir\lld-link.exe" "$publishDir\"
Copy-Item "$releaseDir\core"         "$publishDir\core" -Recurse
Copy-Item "$PSScriptRoot\doc"        "$publishDir\doc"     -Recurse

# Copy only .cb files from example/, preserving directory structure.
$exampleRoot = "$PSScriptRoot\example"
Get-ChildItem -Path $exampleRoot -Filter *.cb -Recurse -File | ForEach-Object {
    $rel  = $_.FullName.Substring($exampleRoot.Length + 1)
    $dest = Join-Path "$publishDir\example" $rel
    New-Item -ItemType Directory -Path (Split-Path $dest -Parent) -Force | Out-Null
    Copy-Item $_.FullName $dest
}

$vsix = Get-ChildItem "$PSScriptRoot\vscode-extension\*.vsix" | Sort-Object LastWriteTime -Descending | Select-Object -First 1
if (-not $vsix) { Write-Error "No .vsix found in vscode-extension\ - run build.bat first." }
Copy-Item $vsix.FullName "$publishDir\"

$outFile = "$PSScriptRoot\out\cflat-windows-x64-v$Version.zip"
if (Test-Path $outFile) { Remove-Item $outFile -Force }
Compress-Archive -Path "$publishDir\*" -DestinationPath $outFile

Write-Host "Published to: $publishDir"
Write-Host "Archive:      $outFile"
