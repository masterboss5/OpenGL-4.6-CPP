[CmdletBinding()]
param(
	[ValidateNotNullOrEmpty()]
	[string]$Name = "Sandbox",

	[string]$Directory
)

Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"

if ([string]::IsNullOrWhiteSpace($Directory)) {
	$Directory = Join-Path $PSScriptRoot "Projects\$Name"
}
$Directory = [System.IO.Path]::GetFullPath($Directory)

$InvalidNameCharacters = [System.IO.Path]::GetInvalidFileNameChars()
if ($Name.IndexOfAny($InvalidNameCharacters) -ge 0) {
	throw "Project name contains characters that cannot be used in a file name."
}

$DescriptorPath = Join-Path $Directory "$Name.engineproject"
if (Test-Path -LiteralPath $DescriptorPath) {
	Write-Host "Using existing project: $DescriptorPath" -ForegroundColor Yellow
	Write-Output $DescriptorPath
	return
}

foreach ($Folder in @(
	"Content",
	"Source\GameModule",
	"Config",
	"Saved\Autosaves",
	"Saved\Recovery",
	"Saved\Logs",
	"Saved\Layouts",
	"Intermediate\AssetRegistry",
	"Intermediate\Cook",
	"Intermediate\HotReload",
	"Intermediate\Build",
	"Build\Development",
	"Build\Shipping"
)) {
	[void][System.IO.Directory]::CreateDirectory((Join-Path $Directory $Folder))
}

$Descriptor = [ordered]@{
	FormatVersion = 1
	ID            = [guid]::NewGuid().ToString()
	EngineSchemaVersion = 1
	Name          = $Name
	ContentMounts = @(
		[ordered]@{ VirtualRoot = "/Game"; PhysicalRoot = "Content"; ReadOnly = $false }
		[ordered]@{ VirtualRoot = "/Engine"; PhysicalRoot = "Engine"; ReadOnly = $true }
	)
	StartupScene  = ""
	GameModule    = ""
	BuildConfigurations = @(
		[ordered]@{ Name = "Development"; Optimized = $false; IncludeDebugSymbols = $true }
		[ordered]@{ Name = "Shipping"; Optimized = $true; IncludeDebugSymbols = $false }
	)
	Cook = [ordered]@{
		CompressionLevel = 9
		ArchiveChunkSizeBytes = 67108864
		Deterministic = $true
	}
	ArchiveChunks = @(
		[ordered]@{ Name = "Main"; VirtualRoots = [string[]]@("/Game") }
	)
	EnabledFeatures = @()
}
$Json = $Descriptor | ConvertTo-Json -Depth 10
$RoundTrip = $Json | ConvertFrom-Json
if (-not ($RoundTrip.ContentMounts -is [System.Array]) -or
	-not ($RoundTrip.BuildConfigurations -is [System.Array]) -or
	-not ($RoundTrip.ArchiveChunks -is [System.Array]) -or
	-not ($RoundTrip.ArchiveChunks[0].VirtualRoots -is [System.Array]) -or
	-not ($RoundTrip.EnabledFeatures -is [System.Array])) {
	throw "Generated project descriptor did not preserve its required JSON arrays."
}
[System.IO.File]::WriteAllText($DescriptorPath, "$Json`n", [System.Text.UTF8Encoding]::new($false))

Write-Host "Created project: $DescriptorPath" -ForegroundColor Green
Write-Output $DescriptorPath
