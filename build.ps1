$ErrorActionPreference = 'Stop'
$compiler = Join-Path $env:WINDIR 'Microsoft.NET\Framework\v4.0.30319\csc.exe'
if (!(Test-Path -LiteralPath $compiler)) { throw '.NET Framework 4.x C# compiler not found.' }
$payload = Join-Path $PSScriptRoot 'payload\ENU.DLL'
$expected = '72719efdf007c46ca50c7b40b30c170a26d335595010af98839cfa0563e81d26'
if ((Get-FileHash -LiteralPath $payload -Algorithm SHA256).Hash.ToLowerInvariant() -ne $expected) { throw 'Payload hash mismatch.' }
$out = Join-Path $PSScriptRoot 'dist'
New-Item -ItemType Directory -Path $out -Force | Out-Null
$output = Join-Path $out 'RoadRash_Steam_ZH_v1.1.exe'
& $compiler /nologo /target:winexe /platform:anycpu /optimize+ /utf8output /reference:System.Windows.Forms.dll /reference:System.Drawing.dll "/win32manifest:$(Join-Path $PSScriptRoot 'src\installer.manifest')" "/resource:$payload,RoadRash.ZH.DLL" "/out:$output" (Join-Path $PSScriptRoot 'src\Installer.cs')
if ($LASTEXITCODE -ne 0) { throw 'Compilation failed.' }
Get-FileHash -LiteralPath $output -Algorithm SHA256
