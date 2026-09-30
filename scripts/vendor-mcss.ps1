#!/usr/bin/env pwsh
# vendor-mcss.ps1 — One-time script to populate vendor/mcss/ from the m.css repo.
# Run from the repository root:  .\scripts\vendor-mcss.ps1
# After running: git add vendor/mcss && git commit -m "vendor: add m.css documentation theme"

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

$Repo   = 'https://github.com/mosra/m.css.git'
$Tmp    = "_mcss_vendor_tmp_$(Get-Random)"
$Vendor = 'vendor\mcss'

# ---------------------------------------------------------------------------
Write-Host "`n==> Cloning m.css (depth 1)..."
git clone --depth 1 $Repo $Tmp
$SHA = (git -C $Tmp rev-parse HEAD).Trim()
Write-Host "    commit $SHA"

# ---------------------------------------------------------------------------
Write-Host "`n==> Creating vendor directory structure..."
$dirs = @(
    "$Vendor\documentation\templates\doxygen",
    "$Vendor\plugins\m",
    "$Vendor\css"
)
foreach ($d in $dirs) {
    New-Item -ItemType Directory -Force $d | Out-Null
}

# ---------------------------------------------------------------------------
Write-Host "`n==> Copying documentation/ ..."
$docFiles = @('__init__.py', '_search.py', 'doxygen.py', 'python.py', 'search.js')
foreach ($f in $docFiles) {
    Copy-Item "$Tmp\documentation\$f" "$Vendor\documentation\$f"
}
Copy-Item "$Tmp\documentation\templates\doxygen\*" "$Vendor\documentation\templates\doxygen\"

# ---------------------------------------------------------------------------
Write-Host "`n==> Copying plugins/ ..."
$pluginFiles = @('ansilexer.py', 'dot2svg.py', 'latex2svg.py', 'latex2svgextra.py')
foreach ($f in $pluginFiles) {
    if (Test-Path "$Tmp\plugins\$f") {
        Copy-Item "$Tmp\plugins\$f" "$Vendor\plugins\$f"
    }
}
# plugins/m — exclude test/ subdirectory
Get-ChildItem "$Tmp\plugins\m" -Exclude 'test', '.gitignore' |
    Copy-Item -Destination "$Vendor\plugins\m\" -Recurse -Force

# ---------------------------------------------------------------------------
Write-Host "`n==> Copying CSS ..."
Copy-Item "$Tmp\css\m-dark+documentation.compiled.css" "$Vendor\css\"

# ---------------------------------------------------------------------------
Write-Host "`n==> Writing version marker..."
$SHA | Set-Content "$Vendor\MCSS_VERSION"

# ---------------------------------------------------------------------------
Write-Host "`n==> Cleaning up temp clone..."
Remove-Item -Recurse -Force $Tmp

Write-Host "`nDone. m.css vendored to $Vendor at $SHA"
Write-Host "Next steps:"
Write-Host "  git add $Vendor"
Write-Host "  git commit -m 'vendor: add m.css documentation theme ($SHA)'"
