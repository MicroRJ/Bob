param(
	[string]$Configuration = "windows-x64"
)

$ErrorActionPreference = "Stop"

if ($Configuration -notmatch '^[A-Za-z0-9._-]+$') {
	throw "Invalid package configuration: '$Configuration'."
}

$repositoryRoot = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot ".."))
$bobExecutable = Join-Path $repositoryRoot "blessed\bob.exe"
$licenseFile = Join-Path $repositoryRoot "LICENSE"
$exampleRoot = Join-Path $repositoryRoot "example"
$distributionRoot = Join-Path $repositoryRoot "dist"

if (-not (Test-Path -LiteralPath $bobExecutable -PathType Leaf)) {
	throw "Missing blessed\bob.exe. Build and bless Bob before packaging it."
}

$versionOutput = @(& $bobExecutable --version)
if ($LASTEXITCODE -ne 0) {
	throw "Could not read the Bob version from blessed\bob.exe."
}

$versionLine = $versionOutput | Where-Object { $_ -like "bob *" } | Select-Object -First 1
if (-not $versionLine) {
	throw "blessed\bob.exe did not report a Bob version."
}

$version = $versionLine.Substring(4).Trim()
if (-not $version -or $version.IndexOfAny([IO.Path]::GetInvalidFileNameChars()) -ge 0) {
	throw "Bob reported an invalid package version: '$version'."
}

$packageName = "bob-$version-$Configuration"
$packageRoot = [IO.Path]::GetFullPath((Join-Path $distributionRoot $packageName))
$archivePath = [IO.Path]::GetFullPath((Join-Path $distributionRoot "$packageName.zip"))
$resolvedDistributionRoot = [IO.Path]::GetFullPath($distributionRoot)

if (-not $packageRoot.StartsWith($resolvedDistributionRoot, [StringComparison]::OrdinalIgnoreCase) -or
	[IO.Path]::GetFileName($packageRoot) -ne $packageName) {
	throw "Refusing to replace an invalid package path: $packageRoot"
}

if (Test-Path -LiteralPath $packageRoot) {
	Remove-Item -LiteralPath $packageRoot -Recurse -Force
}
if (Test-Path -LiteralPath $archivePath) {
	Remove-Item -LiteralPath $archivePath -Force
}

$helloRoot = Join-Path $packageRoot "hello"
$null = New-Item -ItemType Directory -Path $helloRoot -Force

Copy-Item -LiteralPath $bobExecutable -Destination (Join-Path $packageRoot "bob.exe")
Copy-Item -LiteralPath $licenseFile -Destination (Join-Path $packageRoot "LICENSE")
Copy-Item -LiteralPath (Join-Path $exampleRoot "QUICKSTART.txt") -Destination (Join-Path $packageRoot "QUICKSTART.txt")

$exampleFiles = @("build.elf", "main.c", "message.c", "message.h")
foreach ($exampleFile in $exampleFiles) {
	Copy-Item -LiteralPath (Join-Path $exampleRoot $exampleFile) -Destination (Join-Path $helloRoot $exampleFile)
}

Compress-Archive -LiteralPath $packageRoot -DestinationPath $archivePath -CompressionLevel Optimal

Write-Output "Created $archivePath"
