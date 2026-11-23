$lines = Get-Content 'C:\Developments\@SailorBox\src\tools\agc_aliases.txt'
$names = @()
foreach ($l in $lines) {
    if ($l -match 'AddNidAlias\("[^"]+",\s*"([^"]+)"\)') {
        $names += $matches[1]
    }
}
$unique = $names | Sort-Object -Unique
Write-Host "Total: $($names.Count), Unique: $($unique.Count)"
$unique | Set-Content 'C:\Developments\@SailorBox\src\tools\unique_agc_exports.txt'
