param([string]$VcpkgRoot=$env:VCPKG_ROOT,[string]$Installed=$env:NC_VCPKG_INSTALLED_DIR,[string]$CMake=$env:NC_CMAKE,[string]$Preset=$env:NC_NATIVE_PRESET)
$ErrorActionPreference='Stop'
if(!$Preset){$Preset='windows-x64-release'}
if (!$VcpkgRoot -or !$Installed -or !$CMake) { throw 'Set VCPKG_ROOT, NC_VCPKG_INSTALLED_DIR and NC_CMAKE (CMake >=4.2).' }
$env:VCPKG_ROOT=$VcpkgRoot
$env:NC_VCPKG_INSTALLED_DIR=$Installed
$vs=& "${env:ProgramFiles(x86)}/Microsoft Visual Studio/Installer/vswhere.exe" -latest -property installationPath
$dev=Join-Path $vs 'Common7/Tools/VsDevCmd.bat'
$env:NC_CLANG=Join-Path $vs 'VC/Tools/Llvm/bin/clang-cl.exe'
$lines=& cmd.exe /d /c "call `"$dev`" -no_logo -arch=x64 >nul && set"
foreach($line in $lines) { if($line -match '^([^=]+)=(.*)$' -and $Matches[1] -notin @('HOME','CODEX_HOME')) { [Environment]::SetEnvironmentVariable($Matches[1],$Matches[2],'Process') } }
$env:VCPKG_ROOT=$VcpkgRoot
$env:NC_VCPKG_INSTALLED_DIR=$Installed
$env:NC_LINK=Join-Path $env:VCToolsInstallDir 'bin/Hostx64/x64/link.exe'
Push-Location $PSScriptRoot/..
try {
 & $CMake --preset $Preset
 if($LASTEXITCODE){throw 'Configure failed'}
 & $CMake --build --preset $Preset
 if($LASTEXITCODE){throw 'Build failed'}
 & (Join-Path (Split-Path $CMake) ctest.exe) --preset $Preset
 if($LASTEXITCODE){throw 'Tests failed'}
} finally { Pop-Location }




