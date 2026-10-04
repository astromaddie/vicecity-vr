# Assembles a buildable source tree for the MiamiVR Quest port.
#
#   .\tools\assemble.ps1 -Revc <path to reVC source> -Out <output dir>
#
# The reVC source tree is not part of this repository and is not distributed
# with it. Obtain the miami branch separately; the tested patch base is commit
# 026cd10f3fdbd92c089830e5067c4457c53c1b51 from mrxenginner/reVC.
param(
    [Parameter(Mandatory=$true)][string]$Revc,
    [Parameter(Mandatory=$true)][string]$Out
)
$ErrorActionPreference = "Stop"
$repo = Split-Path $PSScriptRoot -Parent

if (-not (Test-Path (Join-Path $Revc "src\core\main.cpp"))) {
    Write-Error "'$Revc' does not look like a reVC source tree (src\core\main.cpp not found)"
}
if (Test-Path $Out) { Write-Error "'$Out' already exists; refusing to overwrite" }

Write-Host "[1/4] Copying the reVC tree..."
New-Item -ItemType Directory -Path $Out | Out-Null
robocopy $Revc $Out /E /NFL /NDL /NJH /NJS /XD ".git" | Out-Null
if ($LASTEXITCODE -ge 8) {
    Write-Error "Failed to copy reVC (robocopy exit code $LASTEXITCODE). Remove the partial output and retry."
}

Write-Host "[2/4] Applying the port patch..."
Push-Location $Out
git init -q 2>$null
git apply --whitespace=nowarn (Join-Path $repo "patches\revc-public-base-compat.patch")
if ($LASTEXITCODE -ne 0) {
    Pop-Location
    Write-Error "Public reVC compatibility patch did not apply. The source must be a clean checkout of commit 026cd10f3fdbd92c089830e5067c4457c53c1b51 with no local edits and no patches already applied."
}
git apply --whitespace=nowarn (Join-Path $repo "patches\revc-quest.patch")
if ($LASTEXITCODE -ne 0) {
    Pop-Location
    Write-Error "Patch did not apply after public-base compatibility."
}
git apply --whitespace=nowarn (Join-Path $repo "patches\revc-quest-v050.patch")
if ($LASTEXITCODE -ne 0) {
    Pop-Location
    Write-Error "The v0.5.0 parity patch did not apply. Make sure the base Quest patch applied cleanly."
}
git apply --whitespace=nowarn (Join-Path $repo "patches\revc-quest-v050-integration.patch")
if ($LASTEXITCODE -ne 0) {
    Pop-Location
    Write-Error "The v0.5.0 integration patch did not apply after the parity patch."
}
git apply --whitespace=nowarn (Join-Path $repo "patches\revc-quest-runtime-v2.patch")
if ($LASTEXITCODE -ne 0) {
    Pop-Location
    Write-Error "The runtime v2 patch did not apply after the v0.5.0 integration patch."
}
git apply --whitespace=nowarn (Join-Path $repo "patches\revc-quest-runtime-v3.patch")
if ($LASTEXITCODE -ne 0) {
    Pop-Location
    Write-Error "The runtime v3 patch did not apply after the runtime v2 patch."
}
git apply --whitespace=nowarn (Join-Path $repo "patches\revc-quest-runtime-v4.patch")
if ($LASTEXITCODE -ne 0) {
    Pop-Location
    Write-Error "The runtime v4 patch did not apply after the runtime v3 patch."
}
git apply --whitespace=nowarn (Join-Path $repo "patches\revc-quest-runtime-v5.patch")
if ($LASTEXITCODE -ne 0) {
    Pop-Location
    Write-Error "The runtime v5 patch did not apply after the runtime v4 patch."
}
git apply --whitespace=nowarn (Join-Path $repo "patches\revc-quest-runtime-v6.patch")
if ($LASTEXITCODE -ne 0) {
    Pop-Location
    Write-Error "The runtime v6 patch did not apply after the runtime v5 patch."
}
git apply --whitespace=nowarn (Join-Path $repo "patches\revc-quest-runtime-v7.patch")
if ($LASTEXITCODE -ne 0) {
    Pop-Location
    Write-Error "The runtime v7 patch did not apply after the runtime v6 patch."
}
git apply --whitespace=nowarn (Join-Path $repo "patches\revc-quest-runtime-v8.patch")
if ($LASTEXITCODE -ne 0) {
    Pop-Location
    Write-Error "The runtime v8 patch did not apply after the runtime v7 patch."
}
git apply --whitespace=nowarn (Join-Path $repo "patches\revc-quest-runtime-v9.patch")
if ($LASTEXITCODE -ne 0) {
    Pop-Location
    Write-Error "The runtime v9 patch did not apply after the runtime v8 patch."
}
git apply --whitespace=nowarn (Join-Path $repo "patches\revc-quest-runtime-v10.patch")
if ($LASTEXITCODE -ne 0) {
    Pop-Location
    Write-Error "The runtime v10 patch did not apply after the runtime v9 patch."
}

git apply --whitespace=nowarn (Join-Path $repo "patches\revc-quest-runtime-v11.patch")
if ($LASTEXITCODE -ne 0) {
    Pop-Location
    Write-Error "The runtime v11 patch did not apply after the runtime v10 patch."
}

git apply --whitespace=nowarn (Join-Path $repo "patches\revc-quest-runtime-v12.patch")
if ($LASTEXITCODE -ne 0) {
    Pop-Location
    Write-Error "The runtime v12 patch did not apply after the runtime v11 patch."
}

git apply --whitespace=nowarn (Join-Path $repo "patches\revc-quest-runtime-v13.patch")
if ($LASTEXITCODE -ne 0) {
    Pop-Location
    Write-Error "The runtime v13 patch did not apply after the runtime v12 patch."
}

git apply --whitespace=nowarn (Join-Path $repo "patches\revc-quest-runtime-v14.patch")
if ($LASTEXITCODE -ne 0) {
    Pop-Location
    Write-Error "The runtime v14 patch did not apply after the runtime v13 patch."
}

git apply --whitespace=nowarn (Join-Path $repo "patches\revc-quest-runtime-v15.patch")
if ($LASTEXITCODE -ne 0) {
    Pop-Location
    Write-Error "The runtime v15 patch did not apply after the runtime v14 patch."
}

git apply --whitespace=nowarn (Join-Path $repo "patches\revc-quest-runtime-v16.patch")
if ($LASTEXITCODE -ne 0) {
    Pop-Location
    Write-Error "The runtime v16 patch did not apply after the runtime v15 patch."
}

git apply --whitespace=nowarn (Join-Path $repo "patches\revc-quest-runtime-v17.patch")
if ($LASTEXITCODE -ne 0) {
    Pop-Location
    Write-Error "The runtime v17 patch did not apply after the runtime v16 patch."
}

git apply --whitespace=nowarn (Join-Path $repo "patches\revc-quest-runtime-v18.patch")
if ($LASTEXITCODE -ne 0) {
    Pop-Location
    Write-Error "The runtime v18 patch did not apply after the runtime v17 patch."
}

# Steam Frame engine hooks, guarded by MIAMIVR_STEAM_FRAME (Quest builds are
# unaffected). Regenerated by tools/steam_frame/sync-from-tree.sh.
$framePatch = Join-Path $repo "patches\revc-steam-frame.patch"
if (Test-Path $framePatch) {
    git apply --whitespace=nowarn $framePatch
    if ($LASTEXITCODE -ne 0) {
        Pop-Location
        Write-Error "The Steam Frame patch did not apply after the runtime v18 patch."
    }
}
Remove-Item -Recurse -Force (Join-Path $Out ".git")
Pop-Location

Write-Host "[3/4] Copying the port sources..."
robocopy (Join-Path $repo "overlay") $Out /E /NFL /NDL /NJH /NJS | Out-Null
if ($LASTEXITCODE -ge 8) {
    Write-Error "Failed to copy the Quest overlay (robocopy exit code $LASTEXITCODE)."
}

Write-Host "[4/4] Copying librw with the Vulkan backend..."
robocopy (Join-Path $repo "librw") (Join-Path $Out "vendor\librw") /E /NFL /NDL /NJH /NJS | Out-Null
if ($LASTEXITCODE -ge 8) {
    Write-Error "Failed to copy librw (robocopy exit code $LASTEXITCODE)."
}

Write-Host ""
Write-Host "Done. Next steps:"
Write-Host "  cd $Out\android"
Write-Host "  gradle wrapper   (first time only, or open in Android Studio)"
Write-Host "  .\gradlew assembleDebug"
Write-Host "See BUILDING.md for prerequisites, game data, VR hands and headset installation."
