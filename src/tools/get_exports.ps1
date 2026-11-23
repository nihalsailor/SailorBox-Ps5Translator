$agcCpp = Get-Content 'C:\Developments\@SailorBox\src\runtime\gpu\src\AgcExports.cpp'
$exports = @()
foreach ($line in $agcCpp) {
    if ($line -match 'SAILOR_EXPORT\s+[\w\:\*]+\s+SAILOR_SYSV_ABI\s+(\w+)\(') {
        $exports += $matches[1]
    }
}
$allExports = $exports | Sort-Object -Unique
Write-Host "Total exported functions in AgcExports.cpp: $($allExports.Count)"
$allExports | Set-Content 'C:\Developments\@SailorBox\src\tools\all_gpu_exports.txt'
