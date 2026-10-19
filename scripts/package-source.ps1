[CmdletBinding()]
param()

$ErrorActionPreference = 'Stop'
$sourceRepository = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..'))
$sourceVersion = (Get-Content -LiteralPath (Join-Path $sourceRepository 'version.json') -Raw | ConvertFrom-Json).productVersion
if ($sourceVersion -notmatch '^\d+\.\d+(\.\d+)?$') { throw 'Invalid product version.' }
$sourceName = "Onimusha-PS5-Adapter-Source-$sourceVersion"
$sourceOutputRoot = [IO.Path]::GetFullPath((Join-Path $sourceRepository 'release'))
$sourceStageParent = [IO.Path]::GetFullPath((Join-Path $sourceRepository 'artifacts'))
$sourceStage = [IO.Path]::GetFullPath((Join-Path $sourceStageParent ("source-$sourceVersion-" + (Get-Date -Format 'yyyyMMdd-HHmmssfff'))))
if (-not $sourceStage.StartsWith($sourceStageParent + '\', [StringComparison]::OrdinalIgnoreCase)) { throw 'Invalid source staging path.' }
$sourcePackage = Join-Path $sourceStage $sourceName
New-Item -ItemType Directory -Path $sourcePackage -Force | Out-Null
New-Item -ItemType Directory -Path $sourceOutputRoot -Force | Out-Null

$sourceSingleFiles = @(
    '.gitignore', 'README.md', 'README.en.md', 'CONTRIBUTING.md', 'LICENSE.txt', 'CHANGELOG.md', 'version.json', 'Directory.Build.props',
    'installer/OnimushaPS5Adapter.iss',
    'scripts/build-release.ps1', 'scripts/build-native.cmd', 'scripts/build-opus.ps1', 'scripts/package-source.ps1',
    "docs/USER_GUIDE_${sourceVersion}_zh-CN.md", "docs/USER_GUIDE_${sourceVersion}_en-US.md",
    "docs/RELEASE_NOTES_${sourceVersion}.md",
    'legal/SOURCE_AVAILABLE_LICENSE_zh-CN.txt', 'legal/SOURCE_AVAILABLE_LICENSE_en-US.txt',
    'legal/FREE_EDITION_NOTICE_zh-CN.txt', 'legal/FREE_EDITION_NOTICE_en-US.txt',
    'legal/BASELINE_THIRD_PARTY_NOTICES.md', 'legal/BLUETOOTH_THIRD_PARTY_NOTICES.txt',
    'legal/Inno-Chinese-Translation-LICENSE.txt',
    'legal/REFramework-LICENSE.txt', 'legal/Nielk1-Trigger-Effects-LICENSE.txt',
    'assets/branding/onimusha-ps5-adapter.ico', 'assets/branding/manager-controller-art-v1.png',
    'assets/branding/donate-alipay.jpg', 'assets/branding/donate-wechat.jpg',
    'assets/branding/wechat-contact.jpg', 'assets/branding/qq-update-group.jpg'
)
$sourceDirectories = @(
    'src/Manager', 'src/Product.Core', 'src/Ui.Shared', 'src/Native',
    'tests/CoreSmokeTests', 'tests/NormalAttackSettingsSmoke', 'tests/SpeakerSettingsSmoke', 'tests/DeviceConnectionSmoke',
    'third_party/inno-zh', 'third_party/reframework', 'third_party/opus-1.5.2', 'docs/screenshots',
    'payload/reframework', 'payload/haptics', 'payload/gauntlet_voice'
)
$sourceCandidates = [Collections.Generic.List[IO.FileInfo]]::new()
foreach ($sourceRelative in $sourceSingleFiles) {
    $sourceFile = Get-Item -LiteralPath (Join-Path $sourceRepository $sourceRelative)
    $sourceCandidates.Add($sourceFile)
}
foreach ($sourceRelative in $sourceDirectories) {
    foreach ($sourceFile in Get-ChildItem -LiteralPath (Join-Path $sourceRepository $sourceRelative) -Recurse -File) {
        $sourceCandidates.Add($sourceFile)
    }
}
foreach ($sourceFile in Get-ChildItem -LiteralPath (Join-Path $sourceRepository 'tests') -Filter '*_smoke.cpp' -File) {
    $sourceCandidates.Add($sourceFile)
}
$sourceQaRecord = Join-Path $sourceRepository "docs/RELEASE_QA_$sourceVersion.md"
if (Test-Path -LiteralPath $sourceQaRecord) { $sourceCandidates.Add((Get-Item -LiteralPath $sourceQaRecord)) }

$sourceLegacyFiles = @('EntitlementService.cs', 'MachineIdentity.cs', 'LicenseCodec.cs', 'DeviceCodeCodec.cs')
foreach ($sourceFile in $sourceCandidates | Sort-Object FullName -Unique) {
    $sourceRelative = [IO.Path]::GetRelativePath($sourceRepository, $sourceFile.FullName).Replace('\', '/')
    if ($sourceRelative -match '(^|/)(bin|obj|\.git|__pycache__)(/|$)') { continue }
    if ($sourceRelative.StartsWith('src/Product.Core/') -and $sourceFile.Name -in $sourceLegacyFiles) { continue }
    if ($sourceFile.Name -match '(?i)encrypted-pkcs8|issued-licenses|LicenseIssuer|COMMERCIAL_LICENSE|\.(pfx|p12|pem|key|pdb|obj|lib|exp|log)$') {
        throw "Unexpected private or generated file in source allowlist: $sourceRelative"
    }
    if (($sourceFile.Attributes -band [IO.FileAttributes]::ReparsePoint) -ne 0) { throw "Refusing a linked file: $sourceRelative" }
    $sourceDestination = [IO.Path]::GetFullPath((Join-Path $sourcePackage $sourceRelative))
    if (-not $sourceDestination.StartsWith($sourcePackage + '\', [StringComparison]::OrdinalIgnoreCase)) { throw 'Invalid source destination.' }
    New-Item -ItemType Directory -Path (Split-Path -Parent $sourceDestination) -Force | Out-Null
    Copy-Item -LiteralPath $sourceFile.FullName -Destination $sourceDestination
}
$sourceManifest = Get-ChildItem -LiteralPath $sourcePackage -Recurse -File | Sort-Object FullName | ForEach-Object {
    $sourceRelative = [IO.Path]::GetRelativePath($sourcePackage, $_.FullName).Replace('\', '/')
    '{0} *{1}' -f (Get-FileHash -LiteralPath $_.FullName -Algorithm SHA256).Hash, $sourceRelative
}
[IO.File]::WriteAllLines((Join-Path $sourcePackage 'SOURCE_SHA256SUMS.txt'), $sourceManifest, [Text.UTF8Encoding]::new($false))
$sourceZip = Join-Path $sourceOutputRoot "$sourceName.zip"
if (Test-Path -LiteralPath $sourceZip) {
    $sourceOldZip = Join-Path $sourceOutputRoot ("$sourceName-before-" + (Get-Date -Format 'yyyyMMdd-HHmmssfff') + '.zip')
    Move-Item -LiteralPath $sourceZip -Destination $sourceOldZip
}
Compress-Archive -LiteralPath $sourcePackage -DestinationPath $sourceZip -CompressionLevel Optimal
$sourceZipHash = (Get-FileHash -LiteralPath $sourceZip -Algorithm SHA256).Hash
[IO.File]::WriteAllText((Join-Path $sourceOutputRoot "$sourceName.sha256"), "$sourceZipHash *$sourceName.zip`r`n", [Text.UTF8Encoding]::new($false))
Write-Host "Source package: $sourceZip"
Write-Host "Source staging: $sourcePackage"
