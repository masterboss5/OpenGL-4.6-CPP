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
	$ProjectsRoot = Join-Path $PSScriptRoot "Projects"
	$ExistingProjects = @()
	if (Test-Path -LiteralPath $ProjectsRoot -PathType Container) {
		$ExistingProjects = @(Get-ChildItem -LiteralPath $ProjectsRoot -Filter "*.engineproject" -File -Recurse)
	}

	if ($ExistingProjects.Count -eq 0) {
		$Project = & (Join-Path $PSScriptRoot "New-Project.ps1")
	}
	elseif ($ExistingProjects.Count -eq 1) {
		$Project = $ExistingProjects[0].FullName
	}
	else {
		Add-Type -AssemblyName System.Windows.Forms
		$Dialog = [System.Windows.Forms.OpenFileDialog]::new()
		$Dialog.Title = "Select an engine project"
		$Dialog.InitialDirectory = $ProjectsRoot
		$Dialog.Filter = "Engine projects (*.engineproject)|*.engineproject|All files (*.*)|*.*"
		$Dialog.CheckFileExists = $true
		if ($Dialog.ShowDialog() -ne [System.Windows.Forms.DialogResult]::OK) {
			Write-Host "Editor launch cancelled."
			return
		}
		$Project = $Dialog.FileName
	}
}

try {
	$Project = (Resolve-Path -LiteralPath $Project -ErrorAction Stop).ProviderPath
}
catch {
	$Project = [System.IO.Path]::GetFullPath($Project, (Get-Location).Path)
}
if (-not (Test-Path -LiteralPath $Project -PathType Leaf) -or [System.IO.Path]::GetExtension($Project) -ne ".engineproject") {
	throw "Project must be an existing .engineproject file: $Project"
}

if (-not $NoBuild) {
	& (Join-Path $PSScriptRoot "Build.ps1") -Configuration $Configuration
}

$Executable = Join-Path $PSScriptRoot "x64\$Configuration\Editor.exe"
if (-not (Test-Path -LiteralPath $Executable -PathType Leaf)) {
	throw "Editor executable was not found. Run .\Build.ps1 first."
}

$Executable = (Resolve-Path -LiteralPath $Executable).ProviderPath
Write-Host "Launching editor executable: $Executable"
Write-Host "Opening engine project: $Project"

Push-Location (Join-Path $PSScriptRoot "OpenGL")
try {
	& $Executable $Project
	if ($LASTEXITCODE -ne 0) {
		throw "The editor exited with code $LASTEXITCODE."
	}
}
finally {
	Pop-Location
}
