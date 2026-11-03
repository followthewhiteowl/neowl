# WHATWG Encoding — index snapshot vendore

Source : https://encoding.spec.whatwg.org/
Script : `scripts/refresh_codepage_tables.sh`
Generateur : `tools/gen_codepage_tables.c` -> `src/generated/codepage_tables.inc`
Refresh recommande : manuel, tous les 12-24 mois (spec tres stable).

Licence des donnees : **CC-BY** (faits de correspondance octet<->codepoint du
WHATWG Encoding Standard). Voir THIRD-PARTY-NOTICES.

## Etat du snapshot

- **Date refresh** : 2026-10-31T15:48:32+0100
- **Format des lignes** : `<pointer>\t0x<CODEPOINT_HEX>\t<glyphe> (<NOM>)`
  (le fichier `index-gb18030-ranges.txt` n'a que `<pointer>\t0x<HEX>`).

## Fichiers vendorises

| Fichier | Taille | SHA256 | Usage |
|---|---:|---|---|
| `index-jis0208.txt` | 269.3 KB | `341dcde7e8b984e9c7bbf5ed75c8da7c6087d47083a1a2b3ed558bfd5bef9468` | Shift-JIS + EUC-JP (table partagee) |
| `index-jis0212.txt` | 205.7 KB | `6ac698a473822b282ff4f4d4f17873b970612ef9bd232e03941854c09b67c7ee` | EUC-JP sequences 0x8F |
| `index-euc-kr.txt` | 655.8 KB | `89af20dd867c84cefb710b1790229786cfef2bf11916361a210d81b90381e267` | EUC-KR |
| `index-big5.txt` | 672.7 KB | `08e24270c8e95d998c994c03f907e972480dc01f58743e078654cc466203c8ff` | Big5 |
| `index-gb18030.txt` | 824.1 KB | `746b3c55f1a8ec4b90b451f384437a17fd37cd51cd668456a28071a758d10784` | GB18030 (2 octets) |
| `index-gb18030-ranges.txt` | 3.0 KB | `874c6b6f6f74cf7d427ad228d5b41ddd9354fffd92a2259bf429f86e6baa7a1e` | GB18030 (plages 4 octets) |

## Regeneration

```bash
./scripts/refresh_codepage_tables.sh
```

Idempotent : skip les fichiers dont le SHA256 n'a pas change. Le generateur
auto-verifie des vecteurs de reference (あ/漢, 가, 中, U+0080, U+10FFFF...)
AVANT d'ecrire le .inc : un seul vecteur KO et aucune table n'est publiee.
