[CmdletBinding()]
param(
	[string]$Project,

	[ValidateSet("Debug", "Release")]
	[string]$Configuration = "Debug",

	[switch]$NoBuild
)

Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"

if ([string]::IsNullOrWhiteSpace($Project)) {
	Add-Type -AssemblyName System.Windows.Forms
	$Dialog = [System.Windows.Forms.OpenFileDialog]::new()
	$Dialog.Title = "Select an engine project to run"
	$Dialog.Filter = "Engine projects (*.engineproject)|*.engineproject|All files (*.*)|*.*"
	$Dialog.CheckFileExists = $true
	if ($Dialog.ShowDialog() -ne [System.Windows.Forms.DialogResult]::OK) {
		Write-Host "Game launch cancelled."
		return
	}
	$Project = $Dialog.FileName
}

$Project = [System.IO.Path]::GetFullPath($Project)
if (-not (Test-Path -LiteralPath $Project -PathType Leaf) -or [System.IO.Path]::GetExtension($Project) -ne ".engineproject") {
	throw "Project must be an existing .engineproject file: $Project"
}

if (-not $NoBuild) {
	& (Join-Path $PSScriptRoot "Build.ps1") -Configuration $Configuration
}

$Executable = Join-Path $PSScriptRoot "x64\$Configuration\Game.exe"
if (-not (Test-Path -LiteralPath $Executable -PathType Leaf)) {
	throw "Game executable was not found. Run .\Build.ps1 first."
}

Push-Location (Join-Path $PSScriptRoot "OpenGL")
try {
	& $Executable $Project
	if ($LASTEXITCODE -ne 0) {
		throw "The game exited with code $LASTEXITCODE."
	}
}
finally {
	Pop-Location
}
