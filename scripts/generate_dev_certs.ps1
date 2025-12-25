$ErrorActionPreference = 'Stop'

$certDir = Join-Path $PSScriptRoot '..\\certs'
$resolvedCertDir = Resolve-Path $certDir -ErrorAction SilentlyContinue
if ($resolvedCertDir) {
  $certDir = $resolvedCertDir.Path
}

if (-not (Test-Path $certDir)) {
  New-Item -ItemType Directory -Path $certDir | Out-Null
}

$certPath = Join-Path $certDir 'dev.crt'
$keyPath = Join-Path $certDir 'dev.key'

function Write-PemFile {
  param(
    [string]$Path,
    [string]$Label,
    [byte[]]$Bytes
  )
  $b64 = [Convert]::ToBase64String($Bytes)
  $lines = ($b64 -split '(.{64})' | Where-Object { $_ -ne '' }) -join "`n"
  $pem = "-----BEGIN $Label-----`n$lines`n-----END $Label-----`n"
  Set-Content -Path $Path -Value $pem
}

$openssl = Get-Command openssl -ErrorAction SilentlyContinue
if ($openssl) {
  & $openssl.Path req -x509 -newkey rsa:2048 -nodes `
    -keyout $keyPath `
    -out $certPath `
    -days 365 `
    -subj '/CN=localhost' `
    -addext 'subjectAltName=DNS:localhost,IP:127.0.0.1'
} else {
  $rsa = [System.Security.Cryptography.RSA]::Create(2048)
  $hashAlgo = [System.Security.Cryptography.HashAlgorithmName]::SHA256
  $padding = [System.Security.Cryptography.RSASignaturePadding]::Pkcs1
  $req = New-Object System.Security.Cryptography.X509Certificates.CertificateRequest('CN=localhost', $rsa, $hashAlgo, $padding)

  $sanBuilder = New-Object System.Security.Cryptography.X509Certificates.SubjectAlternativeNameBuilder
  $sanBuilder.AddDnsName('localhost')
  $sanBuilder.AddIpAddress([System.Net.IPAddress]::Parse('127.0.0.1'))
  $req.CertificateExtensions.Add($sanBuilder.Build())
  $req.CertificateExtensions.Add([System.Security.Cryptography.X509Certificates.X509BasicConstraintsExtension]::new($false, $false, 0, $false))
  $usageFlags = [System.Security.Cryptography.X509Certificates.X509KeyUsageFlags]::DigitalSignature -bor `
                [System.Security.Cryptography.X509Certificates.X509KeyUsageFlags]::KeyEncipherment
  $req.CertificateExtensions.Add([System.Security.Cryptography.X509Certificates.X509KeyUsageExtension]::new($usageFlags, $false))
  $req.CertificateExtensions.Add([System.Security.Cryptography.X509Certificates.X509SubjectKeyIdentifierExtension]::new($req.PublicKey, $false))

  $cert = $req.CreateSelfSigned([datetime]::UtcNow.AddDays(-1), [datetime]::UtcNow.AddYears(1))
  $certBytes = $cert.Export([System.Security.Cryptography.X509Certificates.X509ContentType]::Cert)
  $pkcs8Method = $rsa.GetType().GetMethod('ExportPkcs8PrivateKey')
  if ($pkcs8Method) {
    $keyBytes = $rsa.ExportPkcs8PrivateKey()
    Write-PemFile -Path $keyPath -Label 'PRIVATE KEY' -Bytes $keyBytes
  } else {
    try {
      $cngKey = $rsa.Key
      $keyBytes = $cngKey.Export([System.Security.Cryptography.CngKeyBlobFormat]::Pkcs8PrivateBlob)
      Write-PemFile -Path $keyPath -Label 'PRIVATE KEY' -Bytes $keyBytes
    } catch {
      try {
        $keyBytes = $cngKey.Export([System.Security.Cryptography.CngKeyBlobFormat]::Pkcs1PrivateBlob)
        Write-PemFile -Path $keyPath -Label 'RSA PRIVATE KEY' -Bytes $keyBytes
      } catch {
        throw 'Unable to export private key without OpenSSL. Please install OpenSSL and rerun.'
      }
    }
  }
  Write-PemFile -Path $certPath -Label 'CERTIFICATE' -Bytes $certBytes
  $cert.Dispose()
  $rsa.Dispose()
}

Write-Host \"Generated dev certs:\" -ForegroundColor Green
Write-Host \"- $certPath\"
Write-Host \"- $keyPath\"
