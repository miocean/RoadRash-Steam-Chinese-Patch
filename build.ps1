$ErrorActionPreference = 'Stop'
$compiler = Join-Path $env:WINDIR 'Microsoft.NET\Framework\v4.0.30319\csc.exe'
if (!(Test-Path -LiteralPath $compiler)) { throw '.NET Framework 4.x C# compiler not found.' }
$payload = Join-Path $PSScriptRoot 'payload\ENU.DLL'
$expected = 'ebb8b2c7168907730a3d7a57638646918581f48cfed4e20cb3afad7c4df8c1cb'
if ((Get-FileHash -LiteralPath $payload -Algorithm SHA256).Hash.ToLowerInvariant() -ne $expected) { throw 'Payload hash mismatch.' }
$out = Join-Path $PSScriptRoot 'dist'
New-Item -ItemType Directory -Path $out -Force | Out-Null
$output = Join-Path $out 'RoadRash_Steam_ZH_v1.3.exe'
& $compiler /nologo /target:winexe /platform:anycpu /optimize+ /utf8output /reference:System.Windows.Forms.dll /reference:System.Drawing.dll "/win32manifest:$(Join-Path $PSScriptRoot 'src\installer.manifest')" "/resource:$payload,RoadRash.ZH.DLL" "/resource:$(Join-Path $PSScriptRoot 'payload\portraits.tsv'),RoadRash.Portraits" "/out:$output" (Join-Path $PSScriptRoot 'src\Installer.cs')
if ($LASTEXITCODE -ne 0) { throw 'Compilation failed.' }
Get-FileHash -LiteralPath $output -Algorithm SHA256
