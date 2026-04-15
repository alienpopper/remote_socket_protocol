try {
    $ecdsa = [System.Security.Cryptography.ECDsaCng]::new(256)
    Write-Host "Created ECDsaCng"
    $ecdsa.KeySize
    $params = $ecdsa.ExportParameters($true)
    Write-Host "D length: $($params.D.Length)"
    Write-Host "X length: $($params.Q.X.Length)"
    Write-Host "Y length: $($params.Q.Y.Length)"
    
    # Export PKCS8 private key
    $privBytes = $ecdsa.ExportPkcs8PrivateKey()
    $pubBytes = $ecdsa.ExportSubjectPublicKeyInfo()
    Write-Host "Private PKCS8 bytes: $($privBytes.Length)"
    Write-Host "Public SPKI bytes: $($pubBytes.Length)"
    
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
    
    $privPem = MakePem "-----BEGIN PRIVATE KEY-----" "-----END PRIVATE KEY-----" $privB64
    $pubPem = MakePem "-----BEGIN PUBLIC KEY-----" "-----END PUBLIC KEY-----" $pubB64
    [System.IO.File]::WriteAllText("$PWD\test\keys\test_private.pem", $privPem)
    [System.IO.File]::WriteAllText("$PWD\test\keys\test_public.pem", $pubPem)
    Write-Host "Written private PEM: $($privPem.Length) chars"
    Write-Host "Written public PEM: $($pubPem.Length) chars"
    Get-Content "$PWD\test\keys\test_private.pem"
    Write-Host "---"
    Get-Content "$PWD\test\keys\test_public.pem"
} catch {
    Write-Host "ERROR: $_"
    Write-Host $_.ScriptStackTrace
}
