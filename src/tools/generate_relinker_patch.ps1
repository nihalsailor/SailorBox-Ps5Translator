$allExports = Get-Content 'C:\Developments\@SailorBox\src\tools\all_gpu_exports.txt'
$moduleLines = ($allExports | ForEach-Object { "        `"$_`"" }) -join ",`n"
$moduleBlock = "    importBuilder.AddModule(`"sailor_gpu.dll`", {`n$moduleLines`n    });"
$moduleBlock | Set-Content 'C:\Developments\@SailorBox\src\tools\relinker_gpu_module.txt'

$aliasLines = Get-Content 'C:\Developments\@SailorBox\src\tools\agc_aliases.txt'
$cleanAliases = @()
foreach ($l in $aliasLines) {
    if ($l -match 'AddNidAlias') {
        $cleanAliases += $l
    }
}
$cleanAliases -join "`n" | Set-Content 'C:\Developments\@SailorBox\src\tools\relinker_agc_aliases.txt'
Write-Host "Generated relinker blocks successfully."
