[CmdletBinding()]
param(
	[ValidateSet("Debug", "Release")]
	[string]$Configuration = "Debug",

	[switch]$NoBuild,

	[switch]$All
)

Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"

if (-not $NoBuild) {
	& (Join-Path $PSScriptRoot "Build.ps1") -Configuration $Configuration
}

$Executable = Join-Path $PSScriptRoot "x64\$Configuration\Validation.exe"
if (-not (Test-Path -LiteralPath $Executable -PathType Leaf)) {
	throw "Validation executable was not found. Run .\Build.ps1 first."
}

$Arguments = @()
if ($All) {
	$Arguments += "--all"

	$VsWhere = Join-Path ${env:ProgramFiles(x86)} "Microsoft Visual Studio\Installer\vswhere.exe"
	if (-not (Test-Path -LiteralPath $VsWhere -PathType Leaf)) {
		throw "Visual Studio Installer's vswhere.exe was not found."
	}
	$MSBuild = & $VsWhere -latest -products * -requires Microsoft.Component.MSBuild -find "MSBuild\**\Bin\MSBuild.exe" |
		Select-Object -First 1
	if ([string]::IsNullOrWhiteSpace($MSBuild) -or -not (Test-Path -LiteralPath $MSBuild -PathType Leaf)) {
		throw "A Visual Studio installation containing MSBuild was not found."
	}

	$Solution = Join-Path $PSScriptRoot "OpenGL.sln"
	$BuiltGameModule = Join-Path $PSScriptRoot "x64\$Configuration\SandboxGame.dll"
	$EngineLibrary = Join-Path $PSScriptRoot "x64\$Configuration\Engine.dll"
	foreach ($RequiredFile in @($BuiltGameModule, $EngineLibrary)) {
		if (-not (Test-Path -LiteralPath $RequiredFile -PathType Leaf)) {
			throw "Required validation fixture was not found: $RequiredFile"
		}
	}
	$Arguments += "--game-module"
	$Arguments += $BuiltGameModule
	$Arguments += $EngineLibrary
	$Arguments += "--project-build"
	$Arguments += $MSBuild
	$Arguments += $Solution
	$Arguments += $BuiltGameModule
	$Arguments += $Configuration
}
else {
	$Arguments += "--editor-core"
	$Arguments += "--render-core"
}

Push-Location (Join-Path $PSScriptRoot "OpenGL")
try {
	& $Executable @Arguments
	if ($LASTEXITCODE -ne 0) {
		throw "Validation exited with code $LASTEXITCODE."
	}
}
finally {
	Pop-Location
}
