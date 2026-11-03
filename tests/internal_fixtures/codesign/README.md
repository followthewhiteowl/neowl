# Fixtures de test — signature de code

`throwaway_codesign.pfx.b64` : certificat **jetable, auto-signé, SANS AUCUNE
VALEUR** généré pour les tests de la signature de code. Ce n'est
**pas** un secret — clé RSA 2048 auto-signée, jamais rattachée à une autorité de
confiance, sujet marqué « DO NOT TRUST ». Usage unique : alimenter le round-trip
de construction/vérification PKCS#7 et l'injection Authenticode dans un PE de
test.

Stocké en **base64** (`.pfx.b64`) et non en `.pfx` brut : les `*.pfx` sont
gitignorés (garde anti-leak) et on ne force pas ce garde-fou pour un repo
exporté publiquement. Le test décode à l'exécution (`mbedtls_base64_decode`)
avant de parser le PKCS#12.

- Format : PKCS#12 (`.pfx`)
- Mot de passe : `owltest`
- Usage étendu : signature de code (EKU `1.3.6.1.5.5.7.3.3`)
- Algo : RSA 2048 / SHA-256

Régénérable (Windows) via `New-SelfSignedCertificate -Type CodeSigningCert` +
`Export-PfxCertificate`. Ne jamais remplacer par un vrai certificat.
