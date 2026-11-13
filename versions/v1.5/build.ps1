$ErrorActionPreference='Stop'
$candidate=$PSScriptRoot
$compiler=Join-Path $env:WINDIR 'Microsoft.NET\Framework\v4.0.30319\csc.exe'
$resources=@{
    'components.tsv'='components.tsv'
    'Language.Core'='ENU.Core.DLL'
    'Language.Music'='ENU.Music.DLL'
    'Language.Brands'='ENU.Vehicles.DLL'
    'Language.MusicBrands'='ENU.MusicVehicles.DLL'
    'RRMusicLayout.dll'='RRMusicLayout.dll'
    'RRSaveSlots.dll'='RRSaveSlots.dll'
    'RRWideScreen.dll'='RRWideScreen.dll'
    'RRWideScreen.ini'='RRWideScreen.ini'
    'RRWideScreen-LICENSE.txt'='RRWideScreen-LICENSE.txt'
    'RRWideScreen-NOTICE.txt'='RRWideScreen-NOTICE.txt'
    'Core.delta'='Core.delta'
    'Save.delta'='Save.delta'
    'Wide.delta'='Wide.delta'
    'SaveWide.delta'='SaveWide.delta'
    'Portraits'='portraits.tsv'
}
$common=@('/nologo','/platform:anycpu','/optimize+','/utf8output','/reference:System.Windows.Forms.dll','/reference:System.Drawing.dll')
foreach($name in $resources.Keys){$common+="/resource:$(Join-Path $PSScriptRoot ('payload\' + $resources[$name])),RoadRash.$name"}
$sources=@((Join-Path $candidate 'src\Installer.cs'),(Join-Path $PSScriptRoot 'src\ModuleInstaller.cs'),(Join-Path $PSScriptRoot 'src\InstalledModsForm.cs'),(Join-Path $PSScriptRoot 'src\SaveManagerFiles.cs'),(Join-Path $PSScriptRoot 'src\SaveManagerForm.cs'),(Join-Path $PSScriptRoot 'src\AssemblyInfo.cs'))
$distribution=Join-Path $candidate 'dist'
New-Item -ItemType Directory -Force -Path $distribution | Out-Null
$output=Join-Path $distribution 'RoadRash_Steam_ZH_v1.5.exe'
& $compiler @common /target:winexe "/win32manifest:$(Join-Path $candidate 'src\installer.manifest')" "/out:$output" @sources
if($LASTEXITCODE -ne 0){throw 'Installer compilation failed.'}
Get-FileHash -LiteralPath $output
