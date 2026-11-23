$relinkerFile = 'C:\Developments\@SailorBox\src\relinker\src\Relinker.cpp'
$content = Get-Content $relinkerFile -Raw

$gpuModuleBlock = Get-Content 'C:\Developments\@SailorBox\src\tools\relinker_gpu_module.txt' -Raw
$agcAliasesBlock = Get-Content 'C:\Developments\@SailorBox\src\tools\relinker_agc_aliases.txt' -Raw

# 1. Replace sailor_gpu.dll import block
# Target: from '    importBuilder.AddModule("sailor_gpu.dll", {' to '    });'
$patternGpu = '(?s)    importBuilder\.AddModule\("sailor_gpu\.dll",\s*\{.*?\}\);'
if ($content -match $patternGpu) {
    $content = $content -replace $patternGpu, $gpuModuleBlock.Trim()
    Write-Host "Replaced sailor_gpu.dll import block."
} else {
    Write-Error "Could not find sailor_gpu.dll import block!"
}

# 2. Replace the old AGC NID aliases
# From line 1125: AddNidAlias("AhGvpITrf4M" ... down to AddNidAlias("AAeX-U5-P3M" ...
$patternAliases = '(?s)    AddNidAlias\("AhGvpITrf4M".*?AddNidAlias\("AAeX-U5-P3M", "sceAgcDriverClose"\);'
if ($content -match $patternAliases) {
    $content = $content -replace $patternAliases, $agcAliasesBlock.Trim()
    Write-Host "Replaced AGC NID aliases block."
} else {
    Write-Error "Could not find AGC NID aliases block!"
}

Set-Content -Path $relinkerFile -Value $content -Encoding UTF8
Write-Host "Relinker.cpp updated successfully."
