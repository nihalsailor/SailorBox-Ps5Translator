$unique = Get-Content 'C:\Developments\@SailorBox\src\tools\unique_agc_exports.txt'
$agcCpp = Get-Content 'C:\Developments\@SailorBox\src\runtime\gpu\src\AgcExports.cpp' -Raw
$missing = @()
foreach ($fn in $unique) {
    if (-not ($agcCpp -match [regex]::Escape($fn))) {
        $missing += $fn
    }
}
Write-Host "Total unique: $($unique.Count)"
Write-Host "Missing count: $($missing.Count)"
if ($missing.Count -gt 0) {
    $missing | ForEach-Object { Write-Host "  Missing: $_" }
} else {
    Write-Host "100% COVERAGE! All 237 symbols are exported in AgcExports.cpp!"
}
