// SPDX-License-Identifier: AGPL-3.0-only
/* Self-test Authenticode PE (, tranche 2).
 *
 * Module sous test : owl_code_sign_pe(), PAS ENCORE IMPLEMENTE au moment ou
 * ce fichier est ecrit (cf. en-tete de ticket). Le prototype est donc
 * FORWARD-DECLARE ci-dessous (pas dans code_sign.h, qui sera complete par
 * l'implementeur) ; ce fichier n'est PAS wire dans CMakeLists tant que la
 * fonction n'existe pas -- l'implementeur wire les deux d'un coup.
 *
 * MBEDTLS_ALLOW_PRIVATE_ACCESS : necessaire pour mbedtls_x509_crt.pk (cle
 * publique) lors de la verification manuelle de signature (meme pattern que
 * code_sign_test.c, tranche 1).
 *
 * ================================================================
 * Pourquoi ce fichier ne peut PAS reutiliser mbedtls_pkcs7_parse_der()
 * ================================================================
 * mbedtls/pkcs7.c (cf. pkcs7_get_signer_info) "assume authenticatedAttributes
 * is nonexistent" -- son parseur attend DIRECTEMENT digestEncryptionAlgorithm
 * apres digestAlgorithm, et rejette toute SignerInfo qui contient le champ
 * optionnel [0] authenticatedAttributes. Or c'est precisement ce que la
 * tranche 2 ajoute (attributs authentifies, requis pour Authenticode). Ce
 * fichier descend donc la structure ASN.1 a la main (mbedtls_asn1_get_tag /
 * get_len), comme le fait deja src/code_sign.c cote ECRITURE pour la
 * tranche 1.
 *
 * ================================================================
 * Piege DER a connaitre : re-tag des attributs authentifies avant hash
 * ================================================================
 * authenticatedAttributes est encode sur le fil comme [0] IMPLICIT (tag
 * 0xA0), mais la signature RSA porte sur le SHA-256 du DER de ces memes
 * attributs reencodes comme un SET OF universel (tag 0x31) -- PAS sur
 * l'encodage [0] tel quel. Seul le premier octet (le tag) differe ; les
 * octets de longueur et le contenu sont identiques bit a bit (la longueur
 * DER ne depend pas de la valeur du tag). cf. hash_auth_attrs_as_der_set().
 * C'est LE piege Authenticode/CMS le plus frequent -- une impl qui hashe
 * directement l'encodage [0] produit une signature qui ne verifie jamais
 * contre aucun verificateur standard (Windows compris).
 */

#define MBEDTLS_ALLOW_PRIVATE_ACCESS

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "code_sign.h"

#include "mbedtls/base64.h"
#include "mbedtls/sha256.h"
#include "mbedtls/asn1.h"
#include "mbedtls/x509_crt.h"
#include "mbedtls/pk.h"

/* ================================================================
 * Contrat tranche 2 (fige par ce ticket, a honorer par l'implementeur) :
 *
 *   OwlCodeSignStatus owl_code_sign_pe(OwlSigner* s,
 *                                      const uint8_t* pe_in, size_t pe_in_len,
 *                                      uint8_t** pe_out, size_t* pe_out_len);
 *
 * Prend un PE non signe, renvoie (via *pe_out, a liberer par free()) un PE
 * signe Authenticode. OWL_CODE_SIGN_OK en succes. *pe_out reste NULL en cas
 * d'echec.
 * ================================================================ */
extern OwlCodeSignStatus owl_code_sign_pe(OwlSigner* s,
                                           const uint8_t* pe_in, size_t pe_in_len,
                                           uint8_t** pe_out, size_t* pe_out_len);

static int g_pass = 0;
static int g_fail = 0;

#define CS_CHECK(expr, label) do { \
    if (expr) { g_pass++; } \
    else { g_fail++; fprintf(stderr, "  FAIL: %s (line %d)\n", label, __LINE__); } \
} while (0)

#define FIXTURE_PATH "tests/internal_fixtures/codesign/throwaway_codesign.pfx.b64"
#define FIXTURE_PASSWORD "owltest"

/* ================================================================
 * Fixture PKCS#12 (identique a code_sign_test.c, tranche 1)
 * ================================================================ */
static int load_fixture_pkcs12_der(uint8_t** out_der, size_t* out_len) {
    FILE* f = fopen(FIXTURE_PATH, "rb");
    if (!f) {
        fprintf(stderr, "  impossible d'ouvrir %s\n", FIXTURE_PATH);
        return 0;
    }
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (sz <= 0) { fclose(f); return 0; }

    unsigned char* b64 = (unsigned char*)malloc((size_t)sz);
    if (!b64) { fclose(f); return 0; }
    size_t rd = fread(b64, 1, (size_t)sz, f);
    fclose(f);
    if (rd != (size_t)sz) { free(b64); return 0; }

    while (rd > 0 && (b64[rd - 1] == '\n' || b64[rd - 1] == '\r')) rd--;

    size_t der_len = 0;
    int rc = mbedtls_base64_decode(NULL, 0, &der_len, b64, rd);
    if (rc != 0 && rc != MBEDTLS_ERR_BASE64_BUFFER_TOO_SMALL) { free(b64); return 0; }
    unsigned char* der = (unsigned char*)malloc(der_len);
    if (!der) { free(b64); return 0; }
    size_t written = 0;
    rc = mbedtls_base64_decode(der, der_len, &written, b64, rd);
    free(b64);
    if (rc != 0) { free(der); return 0; }

    *out_der = der;
    *out_len = written;
    return 1;
}

/* ================================================================
 * PE32+ minimal, hand-crafte. Offsets documentes explicitement : le but
 * n'est pas d'etre chargeable par Windows (la validation "chargeable" se
 * fait a part, via l'oracle PowerShell du coordinator sur un vrai exe), mais
 * d'etre structurellement parsable (DOS header -> PE sig -> COFF -> Optional
 * Header PE32+ avec ses 16 data directories -> 1 section).
 *
 *   0x000 .. 0x03F : DOS header (64o). e_magic="MZ", e_lfanew @ 0x3C = 0x40.
 *   0x040 .. 0x043 : signature "PE\0\0"
 *   0x044 .. 0x057 : COFF File Header (20o)
 *   0x058 .. 0x147 : Optional Header PE32+ (0xF0 = 240o, Magic=0x20B,
 *                    16 data directories). CheckSum @ 0x58+64 = 0x98.
 *                    Security DataDirectory (index 4) @ 0x58+112+4*8 = 0xE8
 *                    (VirtualAddress @ 0xE8, Size @ 0xEC).
 *   0x148 .. 0x16F : 1 Section Header (40o), section ".test"
 *   0x170 .. 0x1FF : padding zero (headers alignes a FileAlignment=0x200)
 *   0x200 .. 0x3FF : donnees de section .test (0x200o, motif deterministe)
 *
 *   Taille totale non signee : 0x400 (1024) octets.
 * ================================================================ */
#define PE_DOS_SIZE          0x40u
#define PE_SIG_OFFSET        0x40u
#define PE_COFF_OFFSET       0x44u
#define PE_COFF_SIZE         20u
#define PE_OPT_OFFSET        0x58u   /* PE_COFF_OFFSET + PE_COFF_SIZE */
#define PE_OPT_SIZE          0xF0u   /* PE32+ : 112 champs fixes + 16*8 data dirs */
#define PE_CHECKSUM_REL      64u    /* offset du champ CheckSum DANS l'optional header */
#define PE_DATADIR_REL       112u   /* offset du tableau DataDirectory DANS l'optional header */
#define PE_SECURITY_DIR_IDX  4u
#define PE_SECTION_HDR_OFFSET (PE_OPT_OFFSET + PE_OPT_SIZE)  /* 0x148 */
#define PE_SECTION_HDR_SIZE  40u
#define PE_NUM_SECTIONS      1u
#define PE_HEADERS_RAW_END   (PE_SECTION_HDR_OFFSET + PE_SECTION_HDR_SIZE * PE_NUM_SECTIONS) /* 0x170 */
#define PE_SIZEOFHEADERS     0x200u  /* padding jusqu'a FileAlignment */
#define PE_SECTION_RAW_OFFSET 0x200u
#define PE_SECTION_RAW_SIZE   0x200u
#define PE_TOTAL_LEN         (PE_SECTION_RAW_OFFSET + PE_SECTION_RAW_SIZE) /* 0x400 */

static void put_u16(unsigned char* b, uint16_t v) { b[0] = (unsigned char)(v & 0xFF); b[1] = (unsigned char)((v >> 8) & 0xFF); }
static void put_u32(unsigned char* b, uint32_t v) {
    b[0] = (unsigned char)(v & 0xFF); b[1] = (unsigned char)((v >> 8) & 0xFF);
    b[2] = (unsigned char)((v >> 16) & 0xFF); b[3] = (unsigned char)((v >> 24) & 0xFF);
}
static void put_u64(unsigned char* b, uint64_t v) { put_u32(b, (uint32_t)(v & 0xFFFFFFFFu)); put_u32(b + 4, (uint32_t)(v >> 32)); }
static uint16_t get_u16(const unsigned char* b) { return (uint16_t)(b[0] | (b[1] << 8)); }
static uint32_t get_u32(const unsigned char* b) {
    return (uint32_t)b[0] | ((uint32_t)b[1] << 8) | ((uint32_t)b[2] << 16) | ((uint32_t)b[3] << 24);
}

/* Construit un PE32+ minimal non signe. *out est alloue par malloc(), a
 * liberer par l'appelant. Deterministe : memes octets a chaque appel. */
static void build_minimal_pe(uint8_t** out, size_t* out_len) {
    unsigned char* buf = (unsigned char*)calloc(1, PE_TOTAL_LEN);

    /* DOS header */
    buf[0] = 'M'; buf[1] = 'Z';
    put_u32(buf + 0x3C, PE_SIG_OFFSET); /* e_lfanew */

    /* PE signature */
    memcpy(buf + PE_SIG_OFFSET, "PE\0\0", 4);

    /* COFF File Header */
    unsigned char* coff = buf + PE_COFF_OFFSET;
    put_u16(coff + 0, 0x8664);               /* Machine = AMD64 */
    put_u16(coff + 2, (uint16_t)PE_NUM_SECTIONS);
    put_u32(coff + 4, 0);                    /* TimeDateStamp */
    put_u32(coff + 8, 0);                    /* PointerToSymbolTable */
    put_u32(coff + 12, 0);                   /* NumberOfSymbols */
    put_u16(coff + 16, (uint16_t)PE_OPT_SIZE); /* SizeOfOptionalHeader */
    put_u16(coff + 18, 0x0022);              /* Characteristics */

    /* Optional Header PE32+ */
    unsigned char* opt = buf + PE_OPT_OFFSET;
    put_u16(opt + 0, 0x020B);                /* Magic = PE32+ */
    opt[2] = 14; opt[3] = 0;                 /* linker version */
    put_u32(opt + 4, PE_SECTION_RAW_SIZE);   /* SizeOfCode */
    put_u32(opt + 8, 0);                     /* SizeOfInitializedData */
    put_u32(opt + 12, 0);                    /* SizeOfUninitializedData */
    put_u32(opt + 16, 0x1000);               /* AddressOfEntryPoint */
    put_u32(opt + 20, 0x1000);               /* BaseOfCode */
    put_u64(opt + 24, 0x0000000140000000ULL);/* ImageBase */
    put_u32(opt + 32, 0x1000);               /* SectionAlignment */
    put_u32(opt + 36, 0x200);                /* FileAlignment */
    put_u16(opt + 40, 6); put_u16(opt + 42, 0); /* OS version */
    put_u16(opt + 44, 0); put_u16(opt + 46, 0); /* Image version */
    put_u16(opt + 48, 6); put_u16(opt + 50, 0); /* Subsystem version */
    put_u32(opt + 52, 0);                    /* Win32VersionValue */
    put_u32(opt + 56, 0x2000);               /* SizeOfImage */
    put_u32(opt + 60, PE_SIZEOFHEADERS);     /* SizeOfHeaders */
    put_u32(opt + PE_CHECKSUM_REL, 0);       /* CheckSum : 0, le signeur le recalcule */
    put_u16(opt + 68, 3);                    /* Subsystem = CUI */
    put_u16(opt + 70, 0);                    /* DllCharacteristics */
    put_u64(opt + 72, 0x100000);             /* SizeOfStackReserve */
    put_u64(opt + 80, 0x1000);               /* SizeOfStackCommit */
    put_u64(opt + 88, 0x100000);             /* SizeOfHeapReserve */
    put_u64(opt + 96, 0x1000);               /* SizeOfHeapCommit */
    put_u32(opt + 104, 0);                   /* LoaderFlags */
    put_u32(opt + 108, 16);                  /* NumberOfRvaAndSizes */
    /* DataDirectory[16] : tout a 0/0 par calloc, y compris Security (index 4) */

    /* Section Header ".test" */
    unsigned char* sh = buf + PE_SECTION_HDR_OFFSET;
    memcpy(sh, ".test\0\0\0", 8);
    put_u32(sh + 8, PE_SECTION_RAW_SIZE);    /* VirtualSize */
    put_u32(sh + 12, 0x1000);                /* VirtualAddress */
    put_u32(sh + 16, PE_SECTION_RAW_SIZE);   /* SizeOfRawData */
    put_u32(sh + 20, PE_SECTION_RAW_OFFSET); /* PointerToRawData */
    put_u32(sh + 24, 0); put_u32(sh + 28, 0);/* relocations / linenumbers */
    put_u16(sh + 32, 0); put_u16(sh + 34, 0);
    put_u32(sh + 36, 0x40000040);            /* CNT_INITIALIZED_DATA | MEM_READ */

    /* Donnees de section : motif deterministe, non nul (le test TAMPER a
     * besoin d'octets dont le flip est detectable sans ambiguite). */
    for (size_t i = 0; i < PE_SECTION_RAW_SIZE; i++) {
        buf[PE_SECTION_RAW_OFFSET + i] = (unsigned char)((i * 7 + 3) & 0xFF);
    }

    *out = buf;
    *out_len = PE_TOTAL_LEN;
}

/* ================================================================
 * Hash Authenticode (SHA-256), independant de toute implementation.
 * Fonctionne sur n'importe quel buffer PE32+ valide (signe ou non) : lit
 * ses propres en-tetes pour localiser CheckSum / Security DataDirectory /
 * SizeOfHeaders / sections, exactement comme le decrit la spec Authenticode
 * (saute CheckSum, saute l'entree Security de la data directory, saute la
 * table de certificats elle-meme en fin de fichier).
 * ================================================================ */
static int compute_authenticode_hash(const unsigned char* pe, size_t pe_len,
                                      unsigned char out_digest[32],
                                      size_t* out_checksum_off,
                                      size_t* out_sec_va, size_t* out_sec_size) {
    if (pe_len < 0x40) return -1;
    uint32_t e_lfanew = get_u32(pe + 0x3C);
    if ((size_t)e_lfanew + 24 > pe_len) return -2;
    const unsigned char* nt = pe + e_lfanew;
    if (memcmp(nt, "PE\0\0", 4) != 0) return -3;
    const unsigned char* coff = nt + 4;
    uint16_t num_sections = get_u16(coff + 2);
    uint16_t size_opt = get_u16(coff + 16);
    const unsigned char* opt = coff + PE_COFF_SIZE;
    uint16_t magic = get_u16(opt);
    if (magic != 0x020B) return -4; /* PE32+ uniquement : suffisant pour ce test */

    size_t checksum_off = (size_t)(opt - pe) + PE_CHECKSUM_REL;
    size_t secdir_off = (size_t)(opt - pe) + PE_DATADIR_REL + PE_SECURITY_DIR_IDX * 8u;
    uint32_t sizeof_headers = get_u32(opt + 60);
    uint32_t sec_va = get_u32(pe + secdir_off);
    uint32_t sec_size = get_u32(pe + secdir_off + 4);
    size_t sections_off = (size_t)(opt - pe) + size_opt;

    mbedtls_sha256_context ctx;
    mbedtls_sha256_init(&ctx);
    mbedtls_sha256_starts(&ctx, 0);

    mbedtls_sha256_update(&ctx, pe, checksum_off);                                   /* debut -> CheckSum (exclu) */
    mbedtls_sha256_update(&ctx, pe + checksum_off + 4, secdir_off - (checksum_off + 4)); /* -> Security dir (exclu) */
    mbedtls_sha256_update(&ctx, pe + secdir_off + 8, (size_t)sizeof_headers - (secdir_off + 8)); /* -> fin des headers */

    size_t last_end = sizeof_headers;
    for (uint16_t i = 0; i < num_sections; i++) {
        const unsigned char* shp = pe + sections_off + (size_t)i * PE_SECTION_HDR_SIZE;
        uint32_t raw_size = get_u32(shp + 16);
        uint32_t raw_ptr = get_u32(shp + 20);
        if (raw_size == 0 || raw_ptr == 0) continue;
        mbedtls_sha256_update(&ctx, pe + raw_ptr, raw_size);
        size_t section_end = (size_t)raw_ptr + raw_size;
        if (section_end > last_end) last_end = section_end;
    }

    size_t tail_end = (sec_va != 0) ? (size_t)sec_va : pe_len;
    if (tail_end > last_end) {
        mbedtls_sha256_update(&ctx, pe + last_end, tail_end - last_end);
    }

    mbedtls_sha256_finish(&ctx, out_digest);
    mbedtls_sha256_free(&ctx);

    if (out_checksum_off) *out_checksum_off = checksum_off;
    if (out_sec_va) *out_sec_va = sec_va;
    if (out_sec_size) *out_sec_size = sec_size;
    return 0;
}

/* Algorithme de checksum PE canonique (somme de mots 16 bits avec repli de
 * retenue + taille fichier). Contrairement au hash Authenticode, couvre TOUT
 * le fichier y compris la table de certificats -- seul le champ CheckSum
 * lui-meme (4 octets = 2 mots) est traite comme nul pendant le calcul. */
static uint32_t pe_checksum(const unsigned char* buf, size_t len, size_t checksum_off) {
    uint64_t sum = 0;
    size_t i = 0;
    for (; i + 1 < len; i += 2) {
        uint32_t word;
        if (i == checksum_off || i == checksum_off + 2) {
            word = 0;
        } else {
            word = (uint32_t)buf[i] | ((uint32_t)buf[i + 1] << 8);
        }
        sum += word;
        if (sum > 0xFFFFu) sum = (sum & 0xFFFFu) + (sum >> 16);
    }
    if (i < len) {
        sum += buf[i];
        if (sum > 0xFFFFu) sum = (sum & 0xFFFFu) + (sum >> 16);
    }
    sum = (sum & 0xFFFFu) + (sum >> 16);
    sum = (sum & 0xFFFFu) + (sum >> 16);
    sum += (uint32_t)len;
    return (uint32_t)sum;
}

/* ================================================================
 * Parsing ASN.1 a la main du PKCS#7 Authenticode (SignedData AVEC
 * attributs authentifies -- mbedtls_pkcs7_parse_der ne le supporte pas,
 * cf. commentaire en tete de fichier).
 * ================================================================ */
static const unsigned char OID_PKCS7_SIGNED_DATA[]   = { 0x2A, 0x86, 0x48, 0x86, 0xF7, 0x0D, 0x01, 0x07, 0x02 };
static const unsigned char OID_SPC_INDIRECT_DATA[]   = { 0x2B, 0x06, 0x01, 0x04, 0x01, 0x82, 0x37, 0x02, 0x01, 0x04 };
static const unsigned char OID_ATTR_MESSAGE_DIGEST[] = { 0x2A, 0x86, 0x48, 0x86, 0xF7, 0x0D, 0x01, 0x09, 0x04 };

typedef struct {
    const unsigned char* outer_content_type; size_t outer_content_type_len;
    const unsigned char* inner_content_type; size_t inner_content_type_len;
    const unsigned char* spc_tlv;         size_t spc_tlv_len;         /* SpcIndirectDataContent, tag inclus */
    const unsigned char* embedded_digest; size_t embedded_digest_len; /* DigestInfo.digest */
    const unsigned char* cert_tlv;        size_t cert_tlv_len;        /* Certificate X.509, tag inclus */
    unsigned char* auth_attrs_tag_start;  size_t auth_attrs_total_len; /* region [0] complete, tag inclus */
    unsigned char* auth_attrs_content;    size_t auth_attrs_content_len;
    const unsigned char* attr_message_digest; size_t attr_message_digest_len;
    const unsigned char* signature;       size_t signature_len;       /* encryptedDigest */
} AuthenticodeParsed;

/* Saute un TLV complet (tag + longueur + contenu), quel que soit le tag. */
static int asn1_skip_tlv(unsigned char** p, unsigned char* end) {
    if (*p >= end) return -1;
    int tag = **p;
    size_t len = 0;
    if (mbedtls_asn1_get_tag(p, end, &len, tag) != 0) return -1;
    *p += len;
    return 0;
}

/* Parse le ContentInfo exterieur + SignedData + contenu SPC + certificat +
 * SignerInfo unique avec attributs authentifies. Renvoie 0 si OK. */
static int parse_authenticode_pkcs7(unsigned char* p7_der, size_t p7_len, AuthenticodeParsed* out) {
    memset(out, 0, sizeof(*out));
    unsigned char* p = p7_der;
    unsigned char* end = p7_der + p7_len;
    size_t len = 0;

    /* ContentInfo ::= SEQUENCE { contentType OID, content [0] EXPLICIT SignedData } */
    if (mbedtls_asn1_get_tag(&p, end, &len, MBEDTLS_ASN1_CONSTRUCTED | MBEDTLS_ASN1_SEQUENCE) != 0) return -1;
    unsigned char* ci_end = p + len;
    if (mbedtls_asn1_get_tag(&p, ci_end, &len, MBEDTLS_ASN1_OID) != 0) return -2;
    out->outer_content_type = p; out->outer_content_type_len = len;
    p += len;
    if (mbedtls_asn1_get_tag(&p, ci_end, &len, MBEDTLS_ASN1_CONSTRUCTED | MBEDTLS_ASN1_CONTEXT_SPECIFIC) != 0) return -3;
    unsigned char* sd_wrap_end = p + len;

    /* SignedData ::= SEQUENCE { version, digestAlgorithms, contentInfo, certificates, signerInfos } */
    if (mbedtls_asn1_get_tag(&p, sd_wrap_end, &len, MBEDTLS_ASN1_CONSTRUCTED | MBEDTLS_ASN1_SEQUENCE) != 0) return -4;
    unsigned char* sd_end = p + len;

    if (mbedtls_asn1_get_tag(&p, sd_end, &len, MBEDTLS_ASN1_INTEGER) != 0) return -5; /* version */
    p += len;
    if (mbedtls_asn1_get_tag(&p, sd_end, &len, MBEDTLS_ASN1_CONSTRUCTED | MBEDTLS_ASN1_SET) != 0) return -6; /* digestAlgorithms */
    p += len;

    /* contentInfo (interne) ::= SEQUENCE { contentType OID(SPC_INDIRECT_DATA), content [0] EXPLICIT SpcIndirectDataContent } */
    if (mbedtls_asn1_get_tag(&p, sd_end, &len, MBEDTLS_ASN1_CONSTRUCTED | MBEDTLS_ASN1_SEQUENCE) != 0) return -7;
    unsigned char* inner_ci_end = p + len;
    if (mbedtls_asn1_get_tag(&p, inner_ci_end, &len, MBEDTLS_ASN1_OID) != 0) return -8;
    out->inner_content_type = p; out->inner_content_type_len = len;
    p += len;
    if (mbedtls_asn1_get_tag(&p, inner_ci_end, &len, MBEDTLS_ASN1_CONSTRUCTED | MBEDTLS_ASN1_CONTEXT_SPECIFIC) != 0) return -9;
    unsigned char* spc_wrap_end = p + len;
    unsigned char* spc_start = p; /* debut du TLV SpcIndirectDataContent, tag SEQUENCE inclus */
    if (mbedtls_asn1_get_tag(&p, spc_wrap_end, &len, MBEDTLS_ASN1_CONSTRUCTED | MBEDTLS_ASN1_SEQUENCE) != 0) return -10;
    unsigned char* spc_content_end = p + len;
    out->spc_tlv = spc_start; out->spc_tlv_len = (size_t)(spc_content_end - spc_start);

    if (asn1_skip_tlv(&p, spc_content_end) != 0) return -11; /* data SpcAttributeTypeAndOptionalValue : peu importe son contenu */

    /* messageDigest DigestInfo ::= SEQUENCE { AlgorithmIdentifier, OCTET STRING } */
    if (mbedtls_asn1_get_tag(&p, spc_content_end, &len, MBEDTLS_ASN1_CONSTRUCTED | MBEDTLS_ASN1_SEQUENCE) != 0) return -12;
    unsigned char* digestinfo_end = p + len;
    if (asn1_skip_tlv(&p, digestinfo_end) != 0) return -13; /* digestAlgorithm */
    if (mbedtls_asn1_get_tag(&p, digestinfo_end, &len, MBEDTLS_ASN1_OCTET_STRING) != 0) return -14;
    out->embedded_digest = p; out->embedded_digest_len = len;

    p = inner_ci_end; /* fin du contentInfo interne */

    /* certificates ::= [0] IMPLICIT SET OF Certificate (exactement 1 ici) */
    if (mbedtls_asn1_get_tag(&p, sd_end, &len, MBEDTLS_ASN1_CONSTRUCTED | MBEDTLS_ASN1_CONTEXT_SPECIFIC) != 0) return -15;
    out->cert_tlv = p; out->cert_tlv_len = len; /* len = TLV complet du Certificate (meme convention que mbedtls pkcs7_get_certificates) */
    p += len;

    /* signerInfos ::= SET OF SignerInfo (1 signataire) */
    if (mbedtls_asn1_get_tag(&p, sd_end, &len, MBEDTLS_ASN1_CONSTRUCTED | MBEDTLS_ASN1_SET) != 0) return -16;
    unsigned char* signers_end = p + len;

    if (mbedtls_asn1_get_tag(&p, signers_end, &len, MBEDTLS_ASN1_CONSTRUCTED | MBEDTLS_ASN1_SEQUENCE) != 0) return -17;
    unsigned char* si_end = p + len;
    if (p + len != signers_end) return -18; /* exactement 1 SignerInfo attendu */

    if (mbedtls_asn1_get_tag(&p, si_end, &len, MBEDTLS_ASN1_INTEGER) != 0) return -19; /* version */
    p += len;
    if (asn1_skip_tlv(&p, si_end) != 0) return -20; /* issuerAndSerialNumber */
    if (asn1_skip_tlv(&p, si_end) != 0) return -21; /* digestAlgorithm */

    /* authenticatedAttributes ::= [0] IMPLICIT SET OF Attribute -- OBLIGATOIRE tranche 2 */
    unsigned char* aa_tag_start = p;
    if (mbedtls_asn1_get_tag(&p, si_end, &len, MBEDTLS_ASN1_CONSTRUCTED | MBEDTLS_ASN1_CONTEXT_SPECIFIC) != 0) return -22;
    out->auth_attrs_tag_start = aa_tag_start;
    out->auth_attrs_content = p;
    out->auth_attrs_content_len = len;
    out->auth_attrs_total_len = (size_t)((p + len) - aa_tag_start);
    unsigned char* aa_end = p + len;

    /* Parcours des Attribute ::= SEQUENCE { type OID, values SET OF AttributeValue } */
    unsigned char* ap = p;
    while (ap < aa_end) {
        if (mbedtls_asn1_get_tag(&ap, aa_end, &len, MBEDTLS_ASN1_CONSTRUCTED | MBEDTLS_ASN1_SEQUENCE) != 0) return -23;
        unsigned char* attr_end = ap + len;
        if (mbedtls_asn1_get_tag(&ap, attr_end, &len, MBEDTLS_ASN1_OID) != 0) return -24;
        const unsigned char* oid_p = ap; size_t oid_len = len;
        ap += len;
        if (mbedtls_asn1_get_tag(&ap, attr_end, &len, MBEDTLS_ASN1_CONSTRUCTED | MBEDTLS_ASN1_SET) != 0) return -25;
        unsigned char* val_end = ap + len;
        if (oid_len == sizeof(OID_ATTR_MESSAGE_DIGEST) && memcmp(oid_p, OID_ATTR_MESSAGE_DIGEST, oid_len) == 0) {
            unsigned char* vp = ap; size_t vlen = 0;
            if (mbedtls_asn1_get_tag(&vp, val_end, &vlen, MBEDTLS_ASN1_OCTET_STRING) != 0) return -26;
            out->attr_message_digest = vp; out->attr_message_digest_len = vlen;
        }
        ap = attr_end;
    }
    p = aa_end;

    if (asn1_skip_tlv(&p, si_end) != 0) return -27; /* digestEncryptionAlgorithm */
    if (mbedtls_asn1_get_tag(&p, si_end, &len, MBEDTLS_ASN1_OCTET_STRING) != 0) return -28; /* encryptedDigest */
    out->signature = p; out->signature_len = len;

    return 0;
}

/* Re-encode authenticatedAttributes comme SET universel (0x31) au lieu de
 * [0] IMPLICIT (0xA0) et renvoie SHA-256 du resultat. C'est CE digest (pas
 * le hash du PE) que la signature RSA du SignerInfo doit verifier. */
static int hash_auth_attrs_as_der_set(const AuthenticodeParsed* pa, unsigned char out_digest[32]) {
    size_t header_len = (size_t)(pa->auth_attrs_content - pa->auth_attrs_tag_start);
    size_t total_len = header_len + pa->auth_attrs_content_len;
    unsigned char* buf = (unsigned char*)malloc(total_len);
    if (!buf) return -1;
    memcpy(buf, pa->auth_attrs_tag_start, total_len);
    buf[0] = (unsigned char)(MBEDTLS_ASN1_CONSTRUCTED | MBEDTLS_ASN1_SET); /* 0xA0 -> 0x31 */
    int rc = mbedtls_sha256(buf, total_len, out_digest, 0);
    free(buf);
    return rc;
}

/* Localise et parse le blob PKCS#7 dans la table de certificats du PE signe.
 * Renvoie 0 si OK, remplit *sec_va / *sec_size / *win_cert_dw_length. */
static int locate_wincert(const unsigned char* pe_out, size_t pe_out_len,
                           size_t* sec_va, size_t* sec_size,
                           uint32_t* dw_length, uint16_t* w_revision, uint16_t* w_cert_type,
                           unsigned char** p7_der, size_t* p7_der_maxlen) {
    uint32_t e_lfanew = get_u32(pe_out + 0x3C);
    const unsigned char* opt = pe_out + e_lfanew + 4 + PE_COFF_SIZE;
    size_t secdir_off = (size_t)(opt - pe_out) + PE_DATADIR_REL + PE_SECURITY_DIR_IDX * 8u;
    uint32_t va = get_u32(pe_out + secdir_off);
    uint32_t size = get_u32(pe_out + secdir_off + 4);
    if (va == 0 || size == 0 || (size_t)va + 8 > pe_out_len) return -1;

    const unsigned char* wc = pe_out + va;
    *dw_length = get_u32(wc + 0);
    *w_revision = get_u16(wc + 4);
    *w_cert_type = get_u16(wc + 6);
    *sec_va = va;
    *sec_size = size;
    *p7_der = (unsigned char*)(wc + 8);
    *p7_der_maxlen = (*dw_length > 8) ? (*dw_length - 8) : 0;
    return 0;
}

/* ================================================================
 * Cas 1 : signature OK, buffer de sortie plus grand (table de certifs ajoutee).
 * ================================================================ */
static void test_pe_sign_ok(void) {
    uint8_t* pfx = NULL; size_t pfx_len = 0;
    if (!load_fixture_pkcs12_der(&pfx, &pfx_len)) { CS_CHECK(0, "fixture chargee (prerequis sign_ok)"); return; }
    OwlSigner* s = NULL;
    OwlCodeSignStatus st = owl_code_sign_load_pkcs12(pfx, pfx_len, FIXTURE_PASSWORD, &s);
    free(pfx);
    CS_CHECK(st == OWL_CODE_SIGN_OK && s != NULL, "load_pkcs12 OK (prerequis sign_ok)");
    if (!s) return;

    uint8_t* pe_in = NULL; size_t pe_in_len = 0;
    build_minimal_pe(&pe_in, &pe_in_len);

    uint8_t* pe_out = NULL; size_t pe_out_len = 0;
    st = owl_code_sign_pe(s, pe_in, pe_in_len, &pe_out, &pe_out_len);
    CS_CHECK(st == OWL_CODE_SIGN_OK, "owl_code_sign_pe sur PE minimal -> OK");
    CS_CHECK(pe_out != NULL, "owl_code_sign_pe -> buffer de sortie non NULL");
    CS_CHECK(pe_out_len > pe_in_len, "owl_code_sign_pe -> PE signe plus grand que l'entree (table de certifs ajoutee)");

    free(pe_in);
    free(pe_out);
    owl_code_sign_free(s);
}

/* ================================================================
 * Cas 2 : structure PE signe -- Security DataDirectory + entete WIN_CERTIFICATE.
 * ================================================================ */
static void test_pe_structure(void) {
    uint8_t* pfx = NULL; size_t pfx_len = 0;
    if (!load_fixture_pkcs12_der(&pfx, &pfx_len)) { CS_CHECK(0, "fixture chargee (prerequis structure)"); return; }
    OwlSigner* s = NULL;
    OwlCodeSignStatus st = owl_code_sign_load_pkcs12(pfx, pfx_len, FIXTURE_PASSWORD, &s);
    free(pfx);
    if (!s) { CS_CHECK(0, "load_pkcs12 OK (prerequis structure)"); return; }

    uint8_t* pe_in = NULL; size_t pe_in_len = 0;
    build_minimal_pe(&pe_in, &pe_in_len);
    uint8_t* pe_out = NULL; size_t pe_out_len = 0;
    st = owl_code_sign_pe(s, pe_in, pe_in_len, &pe_out, &pe_out_len);
    if (st != OWL_CODE_SIGN_OK || !pe_out) { CS_CHECK(0, "owl_code_sign_pe OK (prerequis structure)"); free(pe_in); owl_code_sign_free(s); return; }

    size_t sec_va = 0, sec_size = 0; uint32_t dw_length = 0; uint16_t w_revision = 0, w_cert_type = 0;
    unsigned char* p7 = NULL; size_t p7_maxlen = 0;
    int rc = locate_wincert(pe_out, pe_out_len, &sec_va, &sec_size, &dw_length, &w_revision, &w_cert_type, &p7, &p7_maxlen);
    CS_CHECK(rc == 0, "Security DataDirectory (index 4) renseignee et exploitable");

    CS_CHECK(sec_va == pe_in_len, "Security..VirtualAddress == taille du PE non signe (table appondue juste apres)");
    CS_CHECK((sec_va % 8) == 0, "offset de la table de certificats aligne 8 octets");
    CS_CHECK((dw_length % 8) == 0, "WIN_CERTIFICATE..dwLength (padding inclus) aligne 8 octets");
    CS_CHECK(w_revision == 0x0200, "WIN_CERTIFICATE..wRevision == WIN_CERT_REVISION_2_0 (0x0200)");
    CS_CHECK(w_cert_type == 0x0002, "WIN_CERTIFICATE..wCertificateType == WIN_CERT_TYPE_PKCS_SIGNED_DATA (0x0002)");
    CS_CHECK(sec_size == dw_length, "Security..Size == dwLength (une seule entree dans la table)");
    CS_CHECK(pe_out_len == sec_va + dw_length, "le PE signe se termine exactement a la fin de la table de certificats");

    free(pe_in);
    free(pe_out);
    owl_code_sign_free(s);
}

/* ================================================================
 * Cas 3 : hash Authenticode recalcule independamment == hash embarque dans
 * le SpcIndirectDataContent. Coeur du test.
 * ================================================================ */
static void test_pe_authenticode_hash(void) {
    uint8_t* pfx = NULL; size_t pfx_len = 0;
    if (!load_fixture_pkcs12_der(&pfx, &pfx_len)) { CS_CHECK(0, "fixture chargee (prerequis hash)"); return; }
    OwlSigner* s = NULL;
    OwlCodeSignStatus st = owl_code_sign_load_pkcs12(pfx, pfx_len, FIXTURE_PASSWORD, &s);
    free(pfx);
    if (!s) { CS_CHECK(0, "load_pkcs12 OK (prerequis hash)"); return; }

    uint8_t* pe_in = NULL; size_t pe_in_len = 0;
    build_minimal_pe(&pe_in, &pe_in_len);
    uint8_t* pe_out = NULL; size_t pe_out_len = 0;
    st = owl_code_sign_pe(s, pe_in, pe_in_len, &pe_out, &pe_out_len);
    if (st != OWL_CODE_SIGN_OK || !pe_out) { CS_CHECK(0, "owl_code_sign_pe OK (prerequis hash)"); free(pe_in); owl_code_sign_free(s); return; }

    unsigned char hash_unsigned[32], hash_signed[32];
    CS_CHECK(compute_authenticode_hash(pe_in, pe_in_len, hash_unsigned, NULL, NULL, NULL) == 0,
             "hash Authenticode calculable sur le PE non signe (sanity)");
    CS_CHECK(compute_authenticode_hash(pe_out, pe_out_len, hash_signed, NULL, NULL, NULL) == 0,
             "hash Authenticode calculable sur le PE signe");
    CS_CHECK(memcmp(hash_unsigned, hash_signed, 32) == 0,
             "hash Authenticode identique avant/apres signature (CheckSum + Security dir + table de certifs exclus du calcul)");

    size_t sec_va = 0, sec_size = 0; uint32_t dw_length = 0; uint16_t w_rev = 0, w_type = 0;
    unsigned char* p7 = NULL; size_t p7_maxlen = 0;
    int rc = locate_wincert(pe_out, pe_out_len, &sec_va, &sec_size, &dw_length, &w_rev, &w_type, &p7, &p7_maxlen);
    CS_CHECK(rc == 0, "WIN_CERTIFICATE localisable (prerequis hash embarque)");
    if (rc == 0) {
        AuthenticodeParsed pa;
        int prc = parse_authenticode_pkcs7(p7, p7_maxlen, &pa);
        CS_CHECK(prc == 0, "parse ASN.1 a la main du PKCS#7 Authenticode -> OK");
        if (prc == 0) {
            CS_CHECK(pa.inner_content_type_len == sizeof(OID_SPC_INDIRECT_DATA) &&
                     memcmp(pa.inner_content_type, OID_SPC_INDIRECT_DATA, sizeof(OID_SPC_INDIRECT_DATA)) == 0,
                     "SignedData..contentInfo..contentType == SPC_INDIRECT_DATA (1.3.6.1.4.1.311.2.1.4)");
            CS_CHECK(pa.outer_content_type_len == sizeof(OID_PKCS7_SIGNED_DATA) &&
                     memcmp(pa.outer_content_type, OID_PKCS7_SIGNED_DATA, sizeof(OID_PKCS7_SIGNED_DATA)) == 0,
                     "ContentInfo..contentType == pkcs7-signedData (1.2.840.113549.1.7.2)");
            CS_CHECK(pa.embedded_digest_len == 32, "DigestInfo..digest embarque fait 32 octets (SHA-256)");
            CS_CHECK(pa.embedded_digest_len == 32 && memcmp(pa.embedded_digest, hash_signed, 32) == 0,
                     "hash Authenticode recalcule independamment == hash embarque dans le PKCS#7");
        }
    }

    free(pe_in);
    free(pe_out);
    owl_code_sign_free(s);
}

/* ================================================================
 * Cas 4 : signature RSA valide sur SHA-256(DER des attributs authentifies
 * re-tagges SET), et attribut messageDigest == SHA-256(contenu du
 * SpcIndirectDataContent, en-tete SEQUENCE exclu).
 * ================================================================ */
static void test_pe_signature_valid(void) {
    uint8_t* pfx = NULL; size_t pfx_len = 0;
    if (!load_fixture_pkcs12_der(&pfx, &pfx_len)) { CS_CHECK(0, "fixture chargee (prerequis signature)"); return; }
    OwlSigner* s = NULL;
    OwlCodeSignStatus st = owl_code_sign_load_pkcs12(pfx, pfx_len, FIXTURE_PASSWORD, &s);
    free(pfx);
    if (!s) { CS_CHECK(0, "load_pkcs12 OK (prerequis signature)"); return; }

    uint8_t* pe_in = NULL; size_t pe_in_len = 0;
    build_minimal_pe(&pe_in, &pe_in_len);
    uint8_t* pe_out = NULL; size_t pe_out_len = 0;
    st = owl_code_sign_pe(s, pe_in, pe_in_len, &pe_out, &pe_out_len);
    if (st != OWL_CODE_SIGN_OK || !pe_out) { CS_CHECK(0, "owl_code_sign_pe OK (prerequis signature)"); free(pe_in); owl_code_sign_free(s); return; }

    size_t sec_va = 0, sec_size = 0; uint32_t dw_length = 0; uint16_t w_rev = 0, w_type = 0;
    unsigned char* p7 = NULL; size_t p7_maxlen = 0;
    int rc = locate_wincert(pe_out, pe_out_len, &sec_va, &sec_size, &dw_length, &w_rev, &w_type, &p7, &p7_maxlen);
    CS_CHECK(rc == 0, "WIN_CERTIFICATE localisable (prerequis signature)");
    if (rc == 0) {
        AuthenticodeParsed pa;
        int prc = parse_authenticode_pkcs7(p7, p7_maxlen, &pa);
        CS_CHECK(prc == 0, "parse ASN.1 a la main du PKCS#7 (prerequis signature)");
        if (prc == 0) {
            CS_CHECK(pa.attr_message_digest != NULL, "attribut messageDigest present dans authenticatedAttributes");

            /* messageDigest = SHA-256 du CONTENU de SpcIndirectDataContent,
             * sans son en-tete SEQUENCE englobant : Authenticode hashe la
             * concatenation des membres (data + DigestInfo), pas le wrapper.
             * Hasher la SEQUENCE entiere fait rejeter la signature par Windows
             * (cf. build_authenticode_pkcs7 dans code_sign.c). */
            unsigned char expected_md[32];
            unsigned char* spc_content = (unsigned char*)pa.spc_tlv;
            size_t spc_content_len = 0;
            int tag_rc = mbedtls_asn1_get_tag(&spc_content, spc_content + pa.spc_tlv_len,
                                              &spc_content_len,
                                              MBEDTLS_ASN1_CONSTRUCTED | MBEDTLS_ASN1_SEQUENCE);
            int src = (tag_rc == 0) ? mbedtls_sha256(spc_content, spc_content_len, expected_md, 0) : -1;
            CS_CHECK(src == 0, "SHA-256(contenu de SpcIndirectDataContent) calculable");
            CS_CHECK(pa.attr_message_digest != NULL && pa.attr_message_digest_len == 32 &&
                     memcmp(pa.attr_message_digest, expected_md, 32) == 0,
                     "attribut messageDigest == SHA-256(contenu de SpcIndirectDataContent)");

            unsigned char attrs_digest[32];
            int hrc = hash_auth_attrs_as_der_set(&pa, attrs_digest);
            CS_CHECK(hrc == 0, "SHA-256(authenticatedAttributes re-tagges SET) calculable");

            mbedtls_x509_crt cert;
            mbedtls_x509_crt_init(&cert);
            int crc = mbedtls_x509_crt_parse_der(&cert, pa.cert_tlv, pa.cert_tlv_len);
            CS_CHECK(crc == 0, "certificat signataire embarque reparsable (X.509 DER)");
            if (crc == 0) {
                int vrc = mbedtls_pk_verify(&cert.pk, MBEDTLS_MD_SHA256,
                                            attrs_digest, sizeof(attrs_digest),
                                            pa.signature, pa.signature_len);
                CS_CHECK(vrc == 0, "mbedtls_pk_verify : signature RSA valide sur SHA-256(attributs authentifies)");
            }
            mbedtls_x509_crt_free(&cert);
        }
    }

    free(pe_in);
    free(pe_out);
    owl_code_sign_free(s);
}

/* ================================================================
 * Cas 5 : CheckSum PE recalcule, non nul et correct.
 * ================================================================ */
static void test_pe_checksum(void) {
    uint8_t* pfx = NULL; size_t pfx_len = 0;
    if (!load_fixture_pkcs12_der(&pfx, &pfx_len)) { CS_CHECK(0, "fixture chargee (prerequis checksum)"); return; }
    OwlSigner* s = NULL;
    OwlCodeSignStatus st = owl_code_sign_load_pkcs12(pfx, pfx_len, FIXTURE_PASSWORD, &s);
    free(pfx);
    if (!s) { CS_CHECK(0, "load_pkcs12 OK (prerequis checksum)"); return; }

    uint8_t* pe_in = NULL; size_t pe_in_len = 0;
    build_minimal_pe(&pe_in, &pe_in_len);
    uint8_t* pe_out = NULL; size_t pe_out_len = 0;
    st = owl_code_sign_pe(s, pe_in, pe_in_len, &pe_out, &pe_out_len);
    if (st != OWL_CODE_SIGN_OK || !pe_out) { CS_CHECK(0, "owl_code_sign_pe OK (prerequis checksum)"); free(pe_in); owl_code_sign_free(s); return; }

    size_t checksum_off = 0;
    CS_CHECK(compute_authenticode_hash(pe_out, pe_out_len, (unsigned char[32]){0}, &checksum_off, NULL, NULL) == 0,
             "localisation du champ CheckSum (prerequis checksum)");

    uint32_t stored = get_u32(pe_out + checksum_off);
    uint32_t expected = pe_checksum(pe_out, pe_out_len, checksum_off);

    CS_CHECK(stored != 0, "OptionalHeader..CheckSum non nul apres signature");
    CS_CHECK(stored == expected, "OptionalHeader..CheckSum == checksum PE recalcule independamment (fichier complet, table de certifs incluse)");

    free(pe_in);
    free(pe_out);
    owl_code_sign_free(s);
}

/* ================================================================
 * Cas 6 : TAMPER anti-tautologie -- 1 octet modifie dans les donnees de
 * section APRES signature -> le hash recalcule diverge du hash embarque.
 * ================================================================ */
static void test_pe_tamper_detected(void) {
    uint8_t* pfx = NULL; size_t pfx_len = 0;
    if (!load_fixture_pkcs12_der(&pfx, &pfx_len)) { CS_CHECK(0, "fixture chargee (prerequis tamper)"); return; }
    OwlSigner* s = NULL;
    OwlCodeSignStatus st = owl_code_sign_load_pkcs12(pfx, pfx_len, FIXTURE_PASSWORD, &s);
    free(pfx);
    if (!s) { CS_CHECK(0, "load_pkcs12 OK (prerequis tamper)"); return; }

    uint8_t* pe_in = NULL; size_t pe_in_len = 0;
    build_minimal_pe(&pe_in, &pe_in_len);
    uint8_t* pe_out = NULL; size_t pe_out_len = 0;
    st = owl_code_sign_pe(s, pe_in, pe_in_len, &pe_out, &pe_out_len);
    if (st != OWL_CODE_SIGN_OK || !pe_out) { CS_CHECK(0, "owl_code_sign_pe OK (prerequis tamper)"); free(pe_in); owl_code_sign_free(s); return; }

    size_t sec_va = 0, sec_size = 0; uint32_t dw_length = 0; uint16_t w_rev = 0, w_type = 0;
    unsigned char* p7 = NULL; size_t p7_maxlen = 0;
    int rc = locate_wincert(pe_out, pe_out_len, &sec_va, &sec_size, &dw_length, &w_rev, &w_type, &p7, &p7_maxlen);
    CS_CHECK(rc == 0, "WIN_CERTIFICATE localisable (prerequis tamper)");
    if (rc != 0) { free(pe_in); free(pe_out); owl_code_sign_free(s); return; }

    AuthenticodeParsed pa;
    int prc = parse_authenticode_pkcs7(p7, p7_maxlen, &pa);
    CS_CHECK(prc == 0, "parse ASN.1 a la main du PKCS#7 (prerequis tamper)");
    if (prc != 0) { free(pe_in); free(pe_out); owl_code_sign_free(s); return; }

    unsigned char embedded_digest[32];
    memcpy(embedded_digest, pa.embedded_digest, pa.embedded_digest_len < 32 ? pa.embedded_digest_len : 32);

    /* Buffer tampon independant : flip d'1 octet a l'interieur des donnees
     * de section .test, bien avant la table de certificats. */
    unsigned char* tampered = (unsigned char*)malloc(pe_out_len);
    memcpy(tampered, pe_out, pe_out_len);
    tampered[PE_SECTION_RAW_OFFSET + 10] ^= 0xFF;

    unsigned char hash_tampered[32];
    int hrc = compute_authenticode_hash(tampered, pe_out_len, hash_tampered, NULL, NULL, NULL);
    CS_CHECK(hrc == 0, "hash Authenticode calculable sur le buffer altere");
    CS_CHECK(hrc == 0 && memcmp(hash_tampered, embedded_digest, 32) != 0,
             "hash Authenticode diverge apres alteration d'1 octet de section (anti-tautologie)");

    free(tampered);
    free(pe_in);
    free(pe_out);
    owl_code_sign_free(s);
}

/* ================================================================
 * Cas 7 : durcissement PE malforme (tranche 4a). owl_code_sign_pe doit
 * REJETER proprement (OWL_CODE_SIGN_EBADARG) un PE dont la structure est
 * incoherente, au lieu de produire un digest silencieusement faux ou de
 * lire hors des bornes. Un vrai signataire est charge pour que le rejet
 * vienne bien du parsing PE et PAS du garde `!s` en tete de fonction.
 * ================================================================ */
static void test_pe_hardening_malformed(void) {
    uint8_t* pfx = NULL; size_t pfx_len = 0;
    if (!load_fixture_pkcs12_der(&pfx, &pfx_len)) { CS_CHECK(0, "fixture chargee (prerequis hardening)"); return; }
    OwlSigner* s = NULL;
    OwlCodeSignStatus st = owl_code_sign_load_pkcs12(pfx, pfx_len, FIXTURE_PASSWORD, &s);
    free(pfx);
    if (st != OWL_CODE_SIGN_OK || !s) { CS_CHECK(0, "load_pkcs12 OK (prerequis hardening)"); return; }

    /* Sanity : le PE nominal non modifie signe bien avec ce signataire
     * (sinon les rejets ci-dessous seraient des faux positifs). */
    {
        uint8_t* pe = NULL; size_t pe_len = 0;
        build_minimal_pe(&pe, &pe_len);
        uint8_t* out = NULL; size_t out_len = 0;
        OwlCodeSignStatus rc = owl_code_sign_pe(s, pe, pe_len, &out, &out_len);
        CS_CHECK(rc == OWL_CODE_SIGN_OK && out != NULL, "PE nominal signe (sanity hardening)");
        free(pe); free(out);
    }

    /* 7a : NumberOfRvaAndSizes < 5 -> l'entree Security (index 4) de la data
     * directory n'existe pas. Doit etre rejete AVANT toute lecture de cette
     * entree, EBADARG. Champ a opt+108 (PE32+ : datadir_rel 112 - 4). */
    {
        uint8_t* pe = NULL; size_t pe_len = 0;
        build_minimal_pe(&pe, &pe_len);
        put_u32(pe + PE_OPT_OFFSET + 108, 4); /* < 5 */
        uint8_t* out = NULL; size_t out_len = 0;
        OwlCodeSignStatus rc = owl_code_sign_pe(s, pe, pe_len, &out, &out_len);
        CS_CHECK(rc == OWL_CODE_SIGN_EBADARG, "NumberOfRvaAndSizes < 5 -> EBADARG (pas d'entree Security)");
        CS_CHECK(out == NULL, "NumberOfRvaAndSizes < 5 -> aucun buffer de sortie");
        free(pe); free(out);
    }

    /* 7b : PE tronque -- longueur insuffisante pour couvrir l'entree Security
     * de la data directory (sec_dir + 8 depasse pe_len). Rejet propre EBADARG,
     * pas de lecture hors bornes. */
    {
        uint8_t* pe = NULL; size_t pe_len = 0;
        build_minimal_pe(&pe, &pe_len);
        /* 0x80 : au-dela du DOS header + signature + debut d'optional header,
         * mais bien avant la Security dir (@ 0xE8). %8 == 0 pour ne pas court-
         * circuiter sur l'alignement. */
        uint8_t* out = NULL; size_t out_len = 0;
        OwlCodeSignStatus rc = owl_code_sign_pe(s, pe, 0x80u, &out, &out_len);
        CS_CHECK(rc == OWL_CODE_SIGN_EBADARG, "PE tronque (Security dir hors bornes) -> EBADARG");
        CS_CHECK(out == NULL, "PE tronque -> aucun buffer de sortie");
        free(pe); free(out);
    }

    /* 7c : NumberOfSections > 96 -- au-dela de la capacite du tableau de
     * ranges interne. Doit etre rejete EBADARG, pas tronque en silence vers
     * un digest faux. */
    {
        uint8_t* pe = NULL; size_t pe_len = 0;
        build_minimal_pe(&pe, &pe_len);
        put_u16(pe + PE_COFF_OFFSET + 2, 100); /* NumberOfSections = 100 > 96 */
        uint8_t* out = NULL; size_t out_len = 0;
        OwlCodeSignStatus rc = owl_code_sign_pe(s, pe, pe_len, &out, &out_len);
        CS_CHECK(rc == OWL_CODE_SIGN_EBADARG, "NumberOfSections > 96 -> EBADARG (pas de digest tronque)");
        CS_CHECK(out == NULL, "NumberOfSections > 96 -> aucun buffer de sortie");
        free(pe); free(out);
    }

    owl_code_sign_free(s);
}

int code_sign_pe_self_test_run(void) {
    g_pass = 0;
    g_fail = 0;

    test_pe_sign_ok();
    test_pe_structure();
    test_pe_authenticode_hash();
    test_pe_signature_valid();
    test_pe_checksum();
    test_pe_tamper_detected();
    test_pe_hardening_malformed();

    printf("code_sign_pe self-test: %d pass, %d fail\n", g_pass, g_fail);
    return g_fail;
}
