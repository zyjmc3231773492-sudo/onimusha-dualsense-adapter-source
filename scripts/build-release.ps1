[CmdletBinding()]
param(
    [switch]$DevelopmentBuild,
    [switch]$SkipInstaller,
    [string]$InnoCompiler
)

$ErrorActionPreference = 'Stop'
$repository = [System.IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..'))
$versionMetadata = Get-Content -LiteralPath (Join-Path $repository 'version.json') -Raw | ConvertFrom-Json
$releaseRoot = [System.IO.Path]::GetFullPath((Join-Path $repository 'release'))
$staging = Join-Path $releaseRoot 'staging'
$managerPublish = Join-Path $repository "build\manager-release-$($versionMetadata.productVersion)"
$runtimeFrameworkVersion = '8.0.31'

function Assert-WorkspaceDirectory([string]$Path, [string]$Parent) {
    $resolvedPath = [System.IO.Path]::GetFullPath($Path)
    $resolvedParent = [System.IO.Path]::GetFullPath($Parent).TrimEnd('\', '/') + [System.IO.Path]::DirectorySeparatorChar
    if (-not $resolvedPath.StartsWith($resolvedParent, [StringComparison]::OrdinalIgnoreCase)) {
        throw "Refusing a file operation outside the intended workspace directory: $resolvedPath"
    }
    return $resolvedPath
}

function Archive-ExistingDirectory([string]$Path, [string]$Parent, [string]$ArchiveRoot) {
    $resolvedPath = Assert-WorkspaceDirectory $Path $Parent
    if (Test-Path -LiteralPath $resolvedPath) {
        $resolvedArchive = Assert-WorkspaceDirectory $ArchiveRoot $Parent
        New-Item -ItemType Directory -Path $resolvedArchive -Force | Out-Null
        $archiveName = (Split-Path -Leaf $resolvedPath) + '-before-' + (Get-Date -Format 'yyyyMMdd-HHmmssfff')
        $archiveDestination = Assert-WorkspaceDirectory (Join-Path $resolvedArchive $archiveName) $Parent
        Move-Item -LiteralPath $resolvedPath -Destination $archiveDestination
    }
}

[void](Assert-WorkspaceDirectory $releaseRoot $repository)
[void](Assert-WorkspaceDirectory $staging $releaseRoot)
[void](Assert-WorkspaceDirectory $managerPublish (Join-Path $repository 'build'))

$requiredDonationAssets = @('donate-alipay.jpg', 'donate-wechat.jpg')
foreach ($asset in $requiredDonationAssets) {
    if (-not (Test-Path -LiteralPath (Join-Path $repository "assets\branding\$asset"))) {
        throw "Missing donation image: assets\branding\$asset"
    }
}

& (Join-Path $repository 'scripts\build-opus.ps1')
& (Join-Path $repository 'scripts\build-native.cmd')
if ($LASTEXITCODE -ne 0) { throw 'Native component compilation failed.' }

foreach ($managedTest in @('CoreSmokeTests', 'NormalAttackSettingsSmoke', 'SpeakerSettingsSmoke', 'DeviceConnectionSmoke')) {
    dotnet run --project (Join-Path $repository "tests\$managedTest\$managedTest.csproj") -c Release
    if ($LASTEXITCODE -ne 0) { throw "Managed regression test failed: $managedTest" }
}

& (Join-Path $repository 'build\native\native_product_runtime_smoke.exe')
if ($LASTEXITCODE -ne 0) { throw 'Free native runtime regression test failed.' }

foreach ($nativeTest in Get-ChildItem -LiteralPath (Join-Path $repository 'build\native') -Filter '*_smoke.exe' -File | Where-Object Name -ne 'native_product_runtime_smoke.exe') {
    & $nativeTest.FullName
    if ($LASTEXITCODE -ne 0) { throw "Native regression test failed: $($nativeTest.Name)" }
}

Archive-ExistingDirectory $managerPublish (Join-Path $repository 'build') (Join-Path $repository 'build\archive')
dotnet publish (Join-Path $repository 'src\Manager\OnimushaPS5Adapter.Manager.csproj') `
    -c Release -r win-x64 --self-contained true `
    -p:RuntimeFrameworkVersion=$runtimeFrameworkVersion -p:BluetoothEnabled=true -o $managerPublish
if ($LASTEXITCODE -ne 0) { throw 'Manager publishing failed.' }

Archive-ExistingDirectory $staging $releaseRoot (Join-Path $releaseRoot 'archive')
New-Item -ItemType Directory -Path $staging -Force | Out-Null
Copy-Item -Path (Join-Path $managerPublish '*') -Destination $staging -Recurse
foreach ($file in @('version.json', 'LICENSE.txt', 'CHANGELOG.md')) {
    Copy-Item -LiteralPath (Join-Path $repository $file) -Destination $staging
}
foreach ($language in @('zh-CN', 'en-US')) {
    $guide = "USER_GUIDE_$($versionMetadata.productVersion)_$language.md"
    Copy-Item -LiteralPath (Join-Path $repository "docs\$guide") -Destination $staging
}

$legalTarget = Join-Path $staging 'legal'
New-Item -ItemType Directory -Path $legalTarget -Force | Out-Null
$publicLegalFiles = @(
    'SOURCE_AVAILABLE_LICENSE_zh-CN.txt',
    'SOURCE_AVAILABLE_LICENSE_en-US.txt',
    'FREE_EDITION_NOTICE_zh-CN.txt',
    'FREE_EDITION_NOTICE_en-US.txt',
    'BASELINE_THIRD_PARTY_NOTICES.md',
    'REFramework-LICENSE.txt',
    'Nielk1-Trigger-Effects-LICENSE.txt',
    'BLUETOOTH_THIRD_PARTY_NOTICES.txt',
    'Inno-Chinese-Translation-LICENSE.txt'
)
foreach ($file in $publicLegalFiles) {
    Copy-Item -LiteralPath (Join-Path $repository "legal\$file") -Destination $legalTarget
}

$payloadTarget = Join-Path $staging 'payload'
New-Item -ItemType Directory -Path (Join-Path $payloadTarget 'bin') -Force | Out-Null
Copy-Item -LiteralPath (Join-Path $repository 'build\native\codex_dualsense_bridge.dll') -Destination (Join-Path $payloadTarget 'bin')
Copy-Item -LiteralPath (Join-Path $repository 'build\native\codex_dualsense_hid_helper.exe') -Destination (Join-Path $payloadTarget 'bin')
foreach ($directory in @('reframework', 'haptics', 'gauntlet_voice')) {
    Copy-Item -LiteralPath (Join-Path $repository "payload\$directory") -Destination $payloadTarget -Recurse
}

$assetTarget = Join-Path $staging 'assets'
New-Item -ItemType Directory -Path $assetTarget -Force | Out-Null
foreach ($asset in $requiredDonationAssets) {
    Copy-Item -LiteralPath (Join-Path $repository "assets\branding\$asset") -Destination $assetTarget -Force
}
if ($DevelopmentBuild) {
    [System.IO.File]::WriteAllText(
        (Join-Path $staging 'DEVELOPMENT_BUILD.txt'),
        "Development build. Free to use; commercial resale is prohibited. See legal\SOURCE_AVAILABLE_LICENSE_en-US.txt.`r`n",
        [System.Text.UTF8Encoding]::new($false))
}

Get-ChildItem -LiteralPath $staging -Filter '*.pdb' -File -Recurse | Remove-Item -Force
$privateFiles = Get-ChildItem -LiteralPath $staging -File -Recurse |
    Where-Object { $_.Name -match '(?i)encrypted-pkcs8|private[-_.]?key|issued-licenses\.csv|LicenseIssuer|probe|_smoke|BLUETOOTH-DEVELOPMENT|COMMERCIAL_LICENSE|^license\.dat$|^trial.*\.(bin|dat)$' }
if ($privateFiles) { throw 'Public staging contains private keys, orders, issuer tools, commercial licenses, entitlement state or test files. Packaging refused.' }

$hashLines = Get-ChildItem -LiteralPath $staging -File -Recurse |
    Sort-Object FullName |
    ForEach-Object {
        $relative = [System.IO.Path]::GetRelativePath($staging, $_.FullName).Replace('\', '/')
        '{0} *{1}' -f (Get-FileHash -LiteralPath $_.FullName -Algorithm SHA256).Hash, $relative
    }
[System.IO.File]::WriteAllLines((Join-Path $staging 'SHA256SUMS.txt'), $hashLines, [System.Text.UTF8Encoding]::new($false))

if (-not $SkipInstaller) {
    if ([string]::IsNullOrWhiteSpace($InnoCompiler)) {
        $candidates = @(
            (Join-Path ${env:ProgramFiles(x86)} 'Inno Setup 6\ISCC.exe'),
            (Join-Path $env:ProgramFiles 'Inno Setup 6\ISCC.exe'),
            (Join-Path $env:LOCALAPPDATA 'Programs\Inno Setup 6\ISCC.exe')
        )
        $InnoCompiler = $candidates | Where-Object { Test-Path -LiteralPath $_ } | Select-Object -First 1
    }
    if ([string]::IsNullOrWhiteSpace($InnoCompiler) -or -not (Test-Path -LiteralPath $InnoCompiler)) {
        throw 'Inno Setup 6 was not found. Install it or pass -SkipInstaller to create only the staging directory.'
    }
    $innoArguments = @("/DMyAppVersion=$($versionMetadata.productVersion)", "/DReleaseStaging=$staging")
    if ($DevelopmentBuild) { $innoArguments += '/DDevelopmentBuild=1' }
    $innoArguments += (Join-Path $repository 'installer\OnimushaPS5Adapter.iss')
    & $InnoCompiler $innoArguments
    if ($LASTEXITCODE -ne 0) { throw 'Installer compilation failed.' }

    $installerName = if ($DevelopmentBuild) {
        "Onimusha-PS5-Adapter-Setup-$($versionMetadata.productVersion)-DEV.exe"
    } else {
        "Onimusha-PS5-Adapter-Setup-$($versionMetadata.productVersion).exe"
    }
    $installer = Join-Path $releaseRoot $installerName
    $installerHash = (Get-FileHash -LiteralPath $installer -Algorithm SHA256).Hash
    [System.IO.File]::WriteAllText(
        (Join-Path $releaseRoot 'SHA256SUMS.txt'),
        "$installerHash *$installerName`r`n",
        [System.Text.UTF8Encoding]::new($false))
    Write-Host "Bilingual installer: $installer"
}

Write-Host "Free edition staging directory: $staging"
