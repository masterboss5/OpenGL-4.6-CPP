[CmdletBinding()]
param(
	[ValidateSet("Debug", "Release")]
	[string]$Configuration = "Debug"
)

Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"

$Solution = Join-Path $PSScriptRoot "OpenGL.sln"
$VsWhere = Join-Path ${env:ProgramFiles(x86)} "Microsoft Visual Studio\Installer\vswhere.exe"
if (-not (Test-Path -LiteralPath $VsWhere -PathType Leaf)) {
	throw "Visual Studio Installer's vswhere.exe was not found."
}

$MSBuild = & $VsWhere -latest -products * -requires Microsoft.Component.MSBuild -find "MSBuild\**\Bin\MSBuild.exe" |
	Select-Object -First 1
if ([string]::IsNullOrWhiteSpace($MSBuild) -or -not (Test-Path -LiteralPath $MSBuild -PathType Leaf)) {
	throw "A Visual Studio installation containing MSBuild was not found."
}

Write-Host "Building $Configuration | x64..." -ForegroundColor Cyan
& $MSBuild $Solution /m /nr:false "/p:Configuration=$Configuration" "/p:Platform=x64"
if ($LASTEXITCODE -ne 0) {
	throw "MSBuild failed with exit code $LASTEXITCODE."
}

Write-Host "Build completed: Engine.dll, Editor.exe, Game.exe, Validation.exe, and SandboxGame.dll" -ForegroundColor Green
