# Installs the combined build: the gencast client exe with the plugin host, plus the overlay plugin
# and its configurator, into one game install.
#
# Run on the Windows box, from tools/combined/deploy-combined.sh - not by hand.
#
#   powershell -ExecutionPolicy Bypass -File install-combined.ps1
#
# Both halves are built from their own repos and only meet here, in the install:
#   <install>\GeneralsOnlineZH_60.exe                        the engine, plugin host compiled in
#   <install>\plugins\OverlayPlugin\OverlayPlugin.goplugin.dll
#   <install>\plugins\OverlayPlugin\configurator\index.html

param(
    [string]$BuildDir = 'C:\build\plugin-capture',
    [string]$OverlayDir = 'C:\build\overlay-plugin-capture',
    [string]$InstallDir = 'C:\Program Files (x86)\Origin Games\Command and Conquer Generals Zero Hour\Command and Conquer Generals Zero Hour - Gencast',
    # The overlay tree is synced without .git, so the driver passes the commit it built.
    [string]$OverlayCommit = ''
)

$ErrorActionPreference = 'Stop'
$failures = @()

function Step($text) { Write-Output ("-- " + $text) }
function Fail($text) { $script:failures += $text; Write-Output ("   FAILED: " + $text) }

$exe = Join-Path $BuildDir 'build\win32-vcpkg\GeneralsMD\Release\GeneralsOnlineZH_60.exe'
$dll = Join-Path $OverlayDir 'build\Release\OverlayPlugin.goplugin.dll'
$studio = Join-Path $OverlayDir 'studio\dist'
$pluginDir = Join-Path $InstallDir 'plugins\OverlayPlugin'
$configurator = Join-Path $pluginDir 'configurator'

if (-not (Test-Path $exe)) { throw "no combined exe at $exe - run tools/combined/build-combined.bat" }
if (-not (Test-Path $dll)) { throw "no plugin dll at $dll - run the overlay build first" }
if (-not (Test-Path (Join-Path $studio 'index.html'))) { throw "no packaged configurator at $studio" }
if (-not (Test-Path $InstallDir)) { throw "no install at $InstallDir" }

$running = Get-Process -Name GeneralsOnlineZH_60 -ErrorAction SilentlyContinue
if ($running) { throw "the game is running (pid $($running.Id)); close it first" }

$stamp = Get-Date -Format 'yyyyMMdd-HHmm'

Step "installing the combined exe"
$target = Join-Path $InstallDir 'GeneralsOnlineZH_60.exe'
if ((Test-Path $target) -and ((Get-FileHash $target -Algorithm SHA256).Hash -ne (Get-FileHash $exe -Algorithm SHA256).Hash)) {
    Copy-Item $target "$target.pre-$stamp" -Force
    Write-Output ("   backed up the previous exe as " + (Split-Path "$target.pre-$stamp" -Leaf))
}
Copy-Item $exe $target -Force
if ((Get-FileHash $target -Algorithm SHA256).Hash -ne (Get-FileHash $exe -Algorithm SHA256).Hash) {
    Fail "the installed exe is not the built one"
}
Write-Output ("   " + [math]::Round((Get-Item $target).Length / 1MB, 1) + "MB, sha " + (Get-FileHash $target -Algorithm SHA256).Hash.Substring(0, 12))

# The host has to be inside this exe, or the plugin is a file nobody loads.
Step "checking the exe carries the plugin host"
$bytes = [Text.Encoding]::ASCII.GetString([IO.File]::ReadAllBytes($target))
if ($bytes.Contains('*.goplugin.dll')) {
    Write-Output "   the plugin loader is compiled in"
} else {
    Fail "the exe has no plugin loader - this is not the combined build"
}
if ($bytes.Contains('GUI:CheckingForPatches')) {
    Write-Output "   NOTE: the patch-check string is still in the binary; check the log after a launch"
} else {
    Write-Output "   no pre-online patch/version check in this build"
}

Step "installing the plugin"
New-Item -ItemType Directory -Force -Path $pluginDir | Out-Null
if ((Test-Path (Join-Path $pluginDir 'OverlayPlugin.goplugin.dll')) -and
    ((Get-FileHash (Join-Path $pluginDir 'OverlayPlugin.goplugin.dll') -Algorithm SHA256).Hash -ne (Get-FileHash $dll -Algorithm SHA256).Hash)) {
    Copy-Item (Join-Path $pluginDir 'OverlayPlugin.goplugin.dll') (Join-Path $pluginDir "OverlayPlugin.goplugin.dll.pre-$stamp") -Force
}
Copy-Item $dll (Join-Path $pluginDir 'OverlayPlugin.goplugin.dll') -Force
Write-Output ("   " + [math]::Round((Get-Item (Join-Path $pluginDir 'OverlayPlugin.goplugin.dll')).Length / 1KB) + "KB")

Step "installing the configurator"
if (Test-Path $configurator) { Remove-Item -Recurse -Force $configurator }
New-Item -ItemType Directory -Path $configurator | Out-Null
Copy-Item (Join-Path $studio '*') $configurator -Recurse -Force
Write-Output ("   " + (Get-ChildItem $configurator -Recurse -File).Count + " files")

# The page has to name the presets folder, and "Documents" is not a fixed name: it is localized,
# can be renamed, and is often redirected to OneDrive. So ask Windows for the known folder - the same
# source the game uses (SHGetKnownFolderPath / CSIDL_PERSONAL) - and write it beside the page. The
# game overrides this at runtime with ?presets=, which is the only truly authoritative answer.
Step "recording where the client keeps presets"
$leaf = 'Command and Conquer Generals Zero Hour Data'
$documents = [Environment]::GetFolderPath('MyDocuments')
if ([string]::IsNullOrWhiteSpace($documents)) { Fail "Windows did not report a Documents folder" }
$presetsDir = Join-Path (Join-Path $documents $leaf) 'OverlayPlugin\presets'
$asJson = $presetsDir | ConvertTo-Json -Compress
Set-Content -Path (Join-Path $configurator 'presets-path.js') -Encoding UTF8 `
    -Value "// Written by the installer: the folder Windows reports for Documents, plus the client's leaf.`n// The game replaces it at runtime with ?presets=, which is authoritative.`nwindow.OVERLAY_PRESETS_DIR = $asJson;`n"
Write-Output ("   " + $presetsDir)

Step "checking the configurator's files"
$html = Get-Content (Join-Path $configurator 'index.html') -Raw
$refs = [regex]::Matches($html, '"(assets|schema)/[^"]+"') | ForEach-Object { $_.Value.Trim('"') } | Sort-Object -Unique
foreach ($ref in $refs) {
    if (-not (Test-Path (Join-Path $configurator ($ref -replace '/', '\')))) { Fail "the page asks for $ref, which was not installed" }
}
Write-Output ("   " + $refs.Count + " references resolved")
$presetsPath = Join-Path $configurator 'presets-path.js'
if (-not (Test-Path $presetsPath)) { Fail "the page's presets folder was not recorded" }
$written = Get-Content $presetsPath -Raw
if ($written -notmatch 'OVERLAY_PRESETS_DIR\s*=\s*"') { Fail "presets-path.js does not define OVERLAY_PRESETS_DIR" }
if ($written -notmatch 'OverlayPlugin\\\\presets') { Fail "presets-path.js does not point at OverlayPlugin\presets" }
Write-Output "   presets folder recorded for the page"

Step "loading the installed plugin against the ABI"
$verify = Join-Path $env:TEMP 'overlay-verify-combined'
New-Item -ItemType Directory -Force -Path $verify | Out-Null
Copy-Item (Join-Path $pluginDir 'OverlayPlugin.goplugin.dll') (Join-Path $verify 'OverlayPlugin.goplugin.dll') -Force
$harness = 'C:\build\plugin-abi-test\host_test.exe'
if (Test-Path $harness) {
    Copy-Item $harness (Join-Path $verify 'host_test.exe') -Force
    Push-Location $verify
    try {
        $out = & .\host_test.exe 2>&1 | Out-String
        if ($out -notmatch 'INIT_OK') { Fail "the installed plugin did not initialize" }
        Write-Output ("   " + (($out -split "`n" | Where-Object { $_ -match 'plugin: name|INIT_' }) -join ' | ').Trim())

        # The configurator is served from the plugin, so a build that cannot bind loopback would
        # silently fall back to the file:// page - which cannot write presets in every browser.
        # The line is only logged once bind+listen succeeded; host_test exits immediately, so the
        # HTTP side is exercised by tests/server_route.test.cpp and the deploy's own e2e run.
        $served = [regex]::Match($out, 'configurator at http://127\.0\.0\.1:(\d+)/\?t=[0-9a-f]{32}')
        if (-not $served.Success) { Fail "the installed plugin did not start its configurator server" }
        Write-Output ("   configurator server listening on port " + $served.Groups[1].Value)
    } finally { Pop-Location }
} else {
    Write-Output "   no host_test.exe; skipped"
}

Step "opening the installed configurator"
$edge = @(
    'C:\Program Files (x86)\Microsoft\Edge\Application\msedge.exe',
    'C:\Program Files\Microsoft\Edge\Application\msedge.exe'
) | Where-Object { Test-Path $_ } | Select-Object -First 1
if ($edge) {
    $url = 'file:///' + ((Join-Path $configurator 'index.html') -replace '\\', '/')
    $prev = $ErrorActionPreference
    $ErrorActionPreference = 'Continue'
    $dom = & $edge --headless=new --disable-gpu --no-first-run --virtual-time-budget=5000 --dump-dom $url 2>$null | Out-String
    $ErrorActionPreference = $prev
    $cards = ([regex]::Matches($dom, 'class="stageCard')).Count
    if ($cards -gt 0) { Write-Output ("   the stage rendered ($cards cards)") } else { Fail "the page loaded but drew nothing" }
} else {
    Write-Output "   no Edge; open the page by hand to check it"
}

Step "recording what was installed"
# The synced overlay tree has no .git, and git writes "not a git repository" to stderr, which with
# $ErrorActionPreference = 'Stop' is a terminating error. A missing commit is not a failed install.
function CommitOf($dir) {
    $prev = $ErrorActionPreference
    $ErrorActionPreference = 'Continue'
    $c = (& git -C $dir rev-parse --short HEAD 2>$null | Out-String).Trim()
    $ErrorActionPreference = $prev
    if ($c -and $c -notmatch 'fatal') { return $c } else { return 'unknown' }
}

$engineCommit = CommitOf $BuildDir
if ($OverlayCommit) { $overlayCommit = $OverlayCommit } else { $overlayCommit = CommitOf $OverlayDir }
$stampFile = Join-Path $pluginDir 'BUILD.txt'
@(
    "engine   $engineCommit  (combined/gencast-overlay)"
    "overlay  $overlayCommit"
    "installed $stamp"
) | Set-Content $stampFile
Write-Output ("   " + $stampFile)

Write-Output ""
if ($failures.Count -gt 0) {
    Write-Output "INSTALL_FAILED"
    $failures | ForEach-Object { Write-Output ("  - " + $_) }
    exit 1
}
Write-Output "INSTALL_OK"
exit 0
