param([Parameter(Mandatory=$true)][string]$OriginalDll)
$ErrorActionPreference='Stop'
$project=Split-Path $PSScriptRoot -Parent
if((Get-FileHash -LiteralPath $OriginalDll -Algorithm SHA256).Hash.ToLowerInvariant() -ne '2309d5b30560103c7a8b3b97e626877fb89fd85164f82186082dc1c665466e70'){throw 'Original DLL hash mismatch'}
Add-Type -TypeDefinition @'
using System;
using System.Text;
using System.Runtime.InteropServices;
public static class LanguageQA {
 [DllImport("kernel32.dll",CharSet=CharSet.Unicode,SetLastError=true)] public static extern IntPtr LoadLibraryExW(string path,IntPtr file,uint flags);
 [DllImport("kernel32.dll")] public static extern bool FreeLibrary(IntPtr module);
 [DllImport("user32.dll",CharSet=CharSet.Unicode,ExactSpelling=true)] public static extern int LoadStringW(IntPtr module,uint id,StringBuilder buffer,int size);
 public static string Read(IntPtr module,uint id) { var b=new StringBuilder(8192); LoadStringW(module,id,b,b.Capacity); return b.ToString(); }
}
'@
$tr=Get-Content -LiteralPath (Join-Path $project 'translations/zh.json') -Raw -Encoding UTF8 | ConvertFrom-Json
$dll=Join-Path $project 'payload/ENU.DLL'
$original=[LanguageQA]::LoadLibraryExW((Resolve-Path -LiteralPath $OriginalDll).Path,[IntPtr]::Zero,0x22)
$patched=[LanguageQA]::LoadLibraryExW($dll,[IntPtr]::Zero,0x22)
if($original -eq [IntPtr]::Zero -or $patched -eq [IntPtr]::Zero){throw 'Resource load failed'}
try {
 $nonempty=0
 $encoding=[Text.Encoding]::GetEncoding(936,[Text.EncoderFallback]::ExceptionFallback,[Text.DecoderFallback]::ExceptionFallback)
 for($id=0;$id -lt 1408;$id++) {
  $en=[LanguageQA]::Read($original,$id)
  if($en.Length){$nonempty++}
  $property=$tr.PSObject.Properties[[string]$id]
  $expected=if($null -ne $property){[string]$property.Value}else{$en}
  if([LanguageQA]::Read($patched,$id) -cne $expected){throw "Resource mismatch: $id"}
  if($null -ne $property){
   $null=$encoding.GetBytes($expected)
   $pattern='%(?:[-+ #0]*\d*(?:\.\d+)?[hlL]?[diuoxXfFeEgGcs]|%)'
   if((([regex]::Matches($en,$pattern) | ForEach-Object Value) -join '|') -cne (([regex]::Matches($expected,$pattern) | ForEach-Object Value) -join '|')){throw "Placeholder mismatch: $id"}
  }
 }
 if(@($tr.PSObject.Properties).Count -ne 977){throw 'Translation count mismatch'}
 [ordered]@{slots_verified=1408;nonempty_resources=$nonempty;translated=977;gbk_encodable=$true;placeholders_match=$true;dll_sha256=(Get-FileHash -LiteralPath $dll -Algorithm SHA256).Hash.ToLowerInvariant()} | ConvertTo-Json
} finally { [void][LanguageQA]::FreeLibrary($original); [void][LanguageQA]::FreeLibrary($patched) }
