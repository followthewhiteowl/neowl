# Fixture horodatage RFC 3161

`rfc3161_digicert_response.tsr.b64` : reponse TimeStampResp REELLE (DER, base64)
d'une autorite de temps publique (DigiCert), capturee le 2026-10-29. Status
= Granted, algorithme SHA-256. Ce n'est pas un secret (horodatage public).

Le `messageImprint` de cette reponse = SHA-256 du contenu horodate :
`77aaa32305d33e6127a8ac2f375e6c8d8db9029d0945129c0879cd984fcdc676`
(contenu = les octets ASCII `owl-timestamp-fixture-content`).

Usage : test deterministe OFFLINE du parse TimeStampResp + extraction du
timeStampToken, sans dependance reseau. La requete reelle a une TSA vit dans
la tranche HTTP (test conformance reseau optionnel).
