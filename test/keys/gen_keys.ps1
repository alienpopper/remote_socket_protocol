$ecdsa = [System.Security.Cryptography.ECDsa]::Create([System.Security.Cryptography.ECCurve]::NamedCurves.nistP256)
$privBytes = $ecdsa.ExportECPrivateKey()
$pubBytes = $ecdsa.ExportSubjectPublicKeyInfo()
Write-Host "Private key bytes: $($privBytes.Length)"
Write-Host "Public key bytes: $($pubBytes.Length)"
$privB64 = [Convert]::ToBase64String($privBytes)
$pubB64 = [Convert]::ToBase64String($pubBytes)

function MakePem($header, $footer, $b64) {
    $lines = @($header)
    for ($i = 0; $i -lt $b64.Length; $i += 64) {
        $lines += $b64.Substring($i, [Math]::Min(64, $b64.Length - $i))
    }
    $lines += $footer
    return ($lines -join "`n") + "`n"
}

$privPem = MakePem "-----BEGIN EC PRIVATE KEY-----" "-----END EC PRIVATE KEY-----" $privB64
$pubPem = MakePem "-----BEGIN PUBLIC KEY-----" "-----END PUBLIC KEY-----" $pubB64
[System.IO.File]::WriteAllText("test\keys\test_private.pem", $privPem)
[System.IO.File]::WriteAllText("test\keys\test_public.pem", $pubPem)
Write-Host "Written private PEM: $($privPem.Length) chars"
Write-Host "Written public PEM: $($pubPem.Length) chars"
Get-Content test\keys\test_private.pem | Select-Object -First 3
Get-Content test\keys\test_public.pem | Select-Object -First 3
