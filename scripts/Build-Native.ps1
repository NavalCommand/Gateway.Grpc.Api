param([string]$VcpkgRoot=$env:VCPKG_ROOT,[string]$Installed=$env:NC_VCPKG_INSTALLED_DIR,[string]$CMake=$env:NC_CMAKE)
$ErrorActionPreference='Stop'
if (!$VcpkgRoot -or !$Installed -or !$CMake) { throw 'Set VCPKG_ROOT, NC_VCPKG_INSTALLED_DIR and NC_CMAKE (CMake >=4.2).' }
$env:VCPKG_ROOT=$VcpkgRoot
$env:NC_VCPKG_INSTALLED_DIR=$Installed
$vs=& "${env:ProgramFiles(x86)}/Microsoft Visual Studio/Installer/vswhere.exe" -latest -property installationPath
$dev=Join-Path $vs 'Common7/Tools/VsDevCmd.bat'
$lines=& cmd.exe /s /c "`"`"$dev`" -no_logo -arch=x64 >nul && set`""
foreach($line in $lines) { if($line -match '^([^=]+)=(.*)$' -and $Matches[1] -notin @('HOME','CODEX_HOME')) { [Environment]::SetEnvironmentVariable($Matches[1],$Matches[2],'Process') } }
Push-Location $PSScriptRoot/..
try {
 & $CMake --preset windows-x64-release
 if($LASTEXITCODE){throw 'Configure failed'}
 & $CMake --build --preset windows-x64-release
 if($LASTEXITCODE){throw 'Build failed'}
 & (Join-Path (Split-Path $CMake) ctest.exe) --preset windows-x64-release
 if($LASTEXITCODE){throw 'Tests failed'}
} finally { Pop-Location }

