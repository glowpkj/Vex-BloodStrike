<#
    sign_driver.ps1  --  self-sign vexdrv.sys so it can load on your own machine
                         with Secure Boot OFF and test-signing ON.

    Run ONCE from an ELEVATED PowerShell:
        $env:VEXDRV_PFX_PASS = 'some-local-password'
        powershell -ExecutionPolicy Bypass -File .\sign_driver.ps1

    ---------------------------------------------------------------------------
    DO NOT COMMIT the .pfx / .cer this script produces.

    The PFX contains a PRIVATE KEY. Anyone who has it can sign any kernel driver
    as if it were trusted by your machine. It must never be pushed to a
    repository. .gitignore already excludes sign\*.pfx and sign\*.cer.
    ---------------------------------------------------------------------------
#>

$ErrorActionPreference = 'Continue'

$here     = Split-Path -Parent $MyInvocation.MyCommand.Path
$sys      = Join-Path $here 'vexdrv.sys'
$pfx      = Join-Path $here 'vexdrv.pfx'

# -------------------------------------------------------------------------
#  Password for the PFX. Read from the environment variable VEXDRV_PFX_PASS.
#  There is NO default baked into the script on purpose: a hardcoded password
#  in a public repo is a credential leak.
# -------------------------------------------------------------------------
$pass = $env:VEXDRV_PFX_PASS
if ([string]::IsNullOrEmpty($pass)) {
    Write-Host '[X] VEXDRV_PFX_PASS is not set.' -ForegroundColor Red
    Write-Host "    Set it before running, e.g.:" -ForegroundColor Yellow
    Write-Host "        `$env:VEXDRV_PFX_PASS = 'some-local-password'" -ForegroundColor Yellow
    Write-Host "        powershell -ExecutionPolicy Bypass -File .\sign_driver.ps1" -ForegroundColor Yellow
    Write-Host "    This password only protects the local copy of the .pfx." -ForegroundColor Yellow
    exit 1
}

# Locate signtool. Adjust if your installed SDK version differs.
$signtool = 'C:\Program Files (x86)\Windows Kits\10\bin\10.0.26100.0\x64\signtool.exe'
if (-not (Test-Path $signtool)) {
    $candidate = Get-ChildItem 'C:\Program Files (x86)\Windows Kits\10\bin' -Directory -ErrorAction SilentlyContinue |
        Sort-Object Name -Descending |
        ForEach-Object { Join-Path $_.FullName 'x64\signtool.exe' } |
        Where-Object { Test-Path $_ } |
        Select-Object -First 1
    if ($candidate) { $signtool = $candidate }
}

$certName = 'vexdrv-self-signed'

Write-Host ''
Write-Host '=== 0/5  checks ===' -ForegroundColor Cyan
if (-not (Test-Path $sys))       { Write-Host '[X] vexdrv.sys not found' -ForegroundColor Red; exit 1 }
if (-not (Test-Path $signtool))  { Write-Host '[X] signtool.exe not found' -ForegroundColor Red; exit 1 }
Write-Host '[ok] vexdrv.sys and signtool present' -ForegroundColor Green

Write-Host ''
Write-Host '=== 1/5  creating self-signed certificate ===' -ForegroundColor Cyan

# Clean up any prior attempt
Get-ChildItem Cert:\LocalMachine\My -ErrorAction SilentlyContinue |
    Where-Object { $_.Subject -eq "CN=$certName" } |
    ForEach-Object { Remove-Item $_.PSPath -Force -ErrorAction SilentlyContinue }

# -DnsName is required by the cmdlet; we use a local name that will never
# resolve. The certificate is for code signing, not TLS.
$cert = $null
try {
    $cert = New-SelfSignedCertificate `
        -DnsName 'vexdrv.local' `
        -Type CodeSigningCert `
        -KeyUsage DigitalSignature `
        -KeyExportPolicy Exportable `
        -KeyAlgorithm RSA `
        -KeyLength 2048 `
        -HashAlgorithm SHA256 `
        -CertStoreLocation 'Cert:\LocalMachine\My' `
        -NotAfter (Get-Date).AddYears(5) `
        -Subject "CN=$certName"
    Write-Host "[ok] cert created: $($cert.Thumbprint)" -ForegroundColor Green
} catch {
    Write-Host "[!] DnsName+Subject form failed: $($_.Exception.Message)" -ForegroundColor Yellow
    Write-Host '    trying DnsName only...' -ForegroundColor Yellow
    try {
        $cert = New-SelfSignedCertificate `
            -DnsName 'vexdrv.local' `
            -Type CodeSigningCert `
            -KeyUsage DigitalSignature `
            -KeyExportPolicy Exportable `
            -CertStoreLocation 'Cert:\LocalMachine\My' `
            -NotAfter (Get-Date).AddYears(5)
        Write-Host "[ok] cert created: $($cert.Thumbprint)" -ForegroundColor Green
    } catch {
        Write-Host "[X] certificate creation failed: $($_.Exception.Message)" -ForegroundColor Red
        exit 1
    }
}

if (-not $cert) { Write-Host '[X] no certificate object' -ForegroundColor Red; exit 1 }

Write-Host ''
Write-Host '=== 2/5  trusting the certificate ===' -ForegroundColor Cyan
$cerPath = Join-Path $here 'vexdrv.cer'
Export-Certificate -Cert $cert -FilePath $cerPath -Type CERT -Force | Out-Null

foreach ($storeName in @('Root', 'TrustedPublisher')) {
    try {
        $store = New-Object System.Security.Cryptography.X509Certificates.X509Store($storeName, 'LocalMachine')
        $store.Open('ReadWrite')
        $store.Add($cert)
        $store.Close()
        Write-Host "[ok] added to $storeName (LocalMachine)" -ForegroundColor Green
    } catch {
        Write-Host "[!] could not add to ${storeName}: $($_.Exception.Message)" -ForegroundColor Yellow
    }
}

Write-Host ''
Write-Host '=== 3/5  exporting private key to PFX ===' -ForegroundColor Cyan
$secPass = ConvertTo-SecureString -String $pass -Force -AsPlainText
try {
    Export-PfxCertificate -Cert $cert -FilePath $pfx -Password $secPass -Force | Out-Null
    Write-Host "[ok] PFX written" -ForegroundColor Green
} catch {
    Write-Host "[X] PFX export failed: $($_.Exception.Message)" -ForegroundColor Red
    exit 1
}

Write-Host ''
Write-Host '=== 4/5  signing vexdrv.sys ===' -ForegroundColor Cyan
& $signtool sign /fd sha256 /f $pfx /p $pass $sys
if ($LASTEXITCODE -ne 0) {
    Write-Host "[X] signtool failed ($LASTEXITCODE)" -ForegroundColor Red
    exit 1
}
Write-Host '[ok] signed' -ForegroundColor Green

Write-Host ''
Write-Host '=== 5/5  verifying ===' -ForegroundColor Cyan
& $signtool verify /v /pa $sys

Write-Host ''
Write-Host 'Done. vexdrv.sys is now self-signed.' -ForegroundColor Green
Write-Host ''
Write-Host 'Next steps:' -ForegroundColor Cyan
Write-Host '  1. bcdedit /set testsigning on    (elevated, then reboot)'
Write-Host '  2. copy vexdrv.sys next to bs_esp.exe'
Write-Host '  3. reboot'
Write-Host '  4. run bs_esp.exe'
