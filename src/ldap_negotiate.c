// SPDX-License-Identifier: AGPL-3.0-only WITH LicenseRef-OWL-Runtime-Exception
#include "ldap_negotiate.h"

#include <stdlib.h>
#include <string.h>
#include <stdio.h>

#include <mbedtls/md.h>
#include <mbedtls/oid.h>
#include <mbedtls/x509_crt.h>

/* ================================================================
 * Channel binding tls-server-end-point (RFC 5929) — partage plateforme.
 * ================================================================ */

#define OWL_CB_PREFIX "tls-server-end-point:"  /* 21 octets (sans le NUL) */

bool owl_ldap_channel_binding_endpoint(const mbedtls_x509_crt* peer_cert,
                                       unsigned char* out, size_t out_cap,
                                       size_t* out_len) {
    if (!peer_cert || !out || !out_len) return false;
    if (peer_cert->raw.p == NULL || peer_cert->raw.len == 0) return false;

    /* RFC 5929 section 4.1 : l'algo de hachage est celui de la signature du
     * certificat, SAUF MD5 et SHA-1 qui sont promus a SHA-256. On lit l'algo
     * depuis le sig_oid PUBLIC (pas de champ prive mbedtls) ; un OID inconnu
     * retombe sur SHA-256 (repli sur). */
    mbedtls_md_type_t md_type = MBEDTLS_MD_SHA256;
    mbedtls_pk_type_t pk_type;
    if (mbedtls_oid_get_sig_alg(&peer_cert->sig_oid, &md_type, &pk_type) != 0) {
        md_type = MBEDTLS_MD_SHA256;
    }
    if (md_type == MBEDTLS_MD_MD5 || md_type == MBEDTLS_MD_SHA1
        || md_type == MBEDTLS_MD_NONE) {
        md_type = MBEDTLS_MD_SHA256;
    }

    const mbedtls_md_info_t* md = mbedtls_md_info_from_type(md_type);
    if (!md) return false;
    size_t hash_len = mbedtls_md_get_size(md);

    const size_t prefix_len = sizeof(OWL_CB_PREFIX) - 1;
    if (out_cap < prefix_len + hash_len) return false;

    memcpy(out, OWL_CB_PREFIX, prefix_len);
    if (mbedtls_md(md, peer_cert->raw.p, peer_cert->raw.len, out + prefix_len) != 0)
        return false;
    *out_len = prefix_len + hash_len;
    return true;
}

static OwlLdapSaslToken sasl_token_fail(void) {
    OwlLdapSaslToken t;
    memset(&t, 0, sizeof t);  /* ok=false, done=false, token=NULL */
    return t;
}

/* ================================================================
 * Backend Windows : SSPI (secur32, paquet "Negotiate").
 * ================================================================ */
#ifdef _WIN32

#define SECURITY_WIN32
#include <windows.h>
#include <sspi.h>
#include <security.h>

/* Filet pour les SDK anciens : le flag existe dans sspi.h a jour. */
#ifndef ISC_REQ_NO_INTEGRITY
#define ISC_REQ_NO_INTEGRITY 0x00800000
#endif

struct OwlLdapNegotiate {
    char           spn[256];
    CredHandle     cred;
    bool           cred_ok;
    CtxtHandle     ctx;
    bool           ctx_ok;
    unsigned char* cb;          /* channel binding (application data) copie */
    size_t         cb_len;
    unsigned char* cbbuf;       /* SEC_CHANNEL_BINDINGS + application data */
    size_t         cbbuf_len;
    unsigned char* token;       /* jeton courant a emettre (possede) */
    size_t         token_len;
    char           last_error[256];
};

/* Construit le buffer SEC_CHANNEL_BINDINGS (entete + application data) a
 * partir de n->cb. Sans channel binding, n->cbbuf reste NULL (pas de buffer
 * CB passe a SSPI). */
static bool sspi_build_cb(OwlLdapNegotiate* n) {
    free(n->cbbuf);
    n->cbbuf = NULL;
    n->cbbuf_len = 0;
    if (!n->cb || n->cb_len == 0) return true;

    size_t total = sizeof(SEC_CHANNEL_BINDINGS) + n->cb_len;
    unsigned char* b = (unsigned char*)malloc(total);
    if (!b) return false;
    SEC_CHANNEL_BINDINGS* scb = (SEC_CHANNEL_BINDINGS*)b;
    memset(scb, 0, sizeof(*scb));
    scb->cbApplicationDataLength = (unsigned long)n->cb_len;
    scb->dwApplicationDataOffset = (unsigned long)sizeof(SEC_CHANNEL_BINDINGS);
    memcpy(b + sizeof(SEC_CHANNEL_BINDINGS), n->cb, n->cb_len);
    n->cbbuf = b;
    n->cbbuf_len = total;
    return true;
}

/* Un tour InitializeSecurityContext : consomme le jeton serveur (NULL au
 * premier tour) + le channel binding, produit le jeton suivant. */
static OwlLdapSaslToken sspi_do_step(OwlLdapNegotiate* n,
                                     const unsigned char* srv, size_t srv_len) {
    SecBuffer inBufs[2];
    unsigned int nIn = 0;
    if (srv && srv_len > 0) {
        inBufs[nIn].BufferType = SECBUFFER_TOKEN;
        inBufs[nIn].cbBuffer   = (unsigned long)srv_len;
        inBufs[nIn].pvBuffer   = (void*)srv;
        nIn++;
    }
    if (n->cbbuf && n->cbbuf_len > 0) {
        inBufs[nIn].BufferType = SECBUFFER_CHANNEL_BINDINGS;
        inBufs[nIn].cbBuffer   = (unsigned long)n->cbbuf_len;
        inBufs[nIn].pvBuffer   = n->cbbuf;
        nIn++;
    }
    SecBufferDesc inDesc;
    inDesc.ulVersion = SECBUFFER_VERSION;
    inDesc.cBuffers  = nIn;
    inDesc.pBuffers  = inBufs;

    SecBuffer outBuf;
    outBuf.BufferType = SECBUFFER_TOKEN;
    outBuf.cbBuffer   = 0;
    outBuf.pvBuffer   = NULL;
    SecBufferDesc outDesc;
    outDesc.ulVersion = SECBUFFER_VERSION;
    outDesc.cBuffers  = 1;
    outDesc.pBuffers  = &outBuf;

    unsigned long ctxAttr = 0;
    TimeStamp     expiry;
    /* Couche de securite SASL = none : le transport TLS chiffre deja tout, et
     * AD refuse un bind sign/seal sur une connexion TLS ("Cannot bind using
     * sign/seal on a connection on which TLS or SSL is in effect", 00002029).
     * Ne PAS demander l'integrite ne suffit pas : le SSP Negotiate/Kerberos de
     * Windows l'assere de lui-meme (ISC_RET_INTEGRITY observe). ISC_REQ_NO_INTEGRITY
     * le supprime explicitement. Mutual auth conserve (le client verifie le
     * serveur, sans wrapping GSS par PDU ; le channel binding tls-server-end-point
     * assure le lien au canal). */
    unsigned long req = ISC_REQ_MUTUAL_AUTH | ISC_REQ_NO_INTEGRITY
                      | ISC_REQ_ALLOCATE_MEMORY;

    SECURITY_STATUS ss = InitializeSecurityContextA(
        &n->cred,
        n->ctx_ok ? &n->ctx : NULL,
        (SEC_CHAR*)n->spn,
        req, 0, SECURITY_NATIVE_DREP,
        (nIn > 0) ? &inDesc : NULL,
        0,
        &n->ctx,
        &outDesc,
        &ctxAttr,
        &expiry);
    n->ctx_ok = true;
    (void)ctxAttr;

    if (ss != SEC_E_OK && ss != SEC_I_CONTINUE_NEEDED) {
        snprintf(n->last_error, sizeof n->last_error,
                 "SSPI InitializeSecurityContext a echoue (0x%08lx)",
                 (unsigned long)ss);
        if (outBuf.pvBuffer) FreeContextBuffer(outBuf.pvBuffer);
        return sasl_token_fail();
    }

    free(n->token);
    n->token = NULL;
    n->token_len = 0;
    if (outBuf.cbBuffer > 0 && outBuf.pvBuffer) {
        n->token = (unsigned char*)malloc(outBuf.cbBuffer);
        if (!n->token) {
            snprintf(n->last_error, sizeof n->last_error,
                     "memoire epuisee (jeton SSPI)");
            FreeContextBuffer(outBuf.pvBuffer);
            return sasl_token_fail();
        }
        memcpy(n->token, outBuf.pvBuffer, outBuf.cbBuffer);
        n->token_len = outBuf.cbBuffer;
    }
    if (outBuf.pvBuffer) FreeContextBuffer(outBuf.pvBuffer);

    OwlLdapSaslToken out;
    out.ok        = true;
    out.done      = (ss == SEC_E_OK);
    out.token     = n->token;
    out.token_len = n->token_len;
    return out;
}

static OwlLdapSaslToken sspi_init(void* self,
                                  const unsigned char* cb, size_t cb_len) {
    OwlLdapNegotiate* n = (OwlLdapNegotiate*)self;
    free(n->cb);
    n->cb = NULL;
    n->cb_len = 0;
    if (cb && cb_len > 0) {
        n->cb = (unsigned char*)malloc(cb_len);
        if (n->cb) { memcpy(n->cb, cb, cb_len); n->cb_len = cb_len; }
    }
    if (!sspi_build_cb(n)) {
        snprintf(n->last_error, sizeof n->last_error,
                 "memoire epuisee (channel binding)");
        return sasl_token_fail();
    }
    return sspi_do_step(n, NULL, 0);
}

static OwlLdapSaslToken sspi_step(void* self,
                                  const unsigned char* srv, size_t srv_len) {
    return sspi_do_step((OwlLdapNegotiate*)self, srv, srv_len);
}

OwlLdapNegotiate* owl_ldap_negotiate_create(const char* spn,
                                            char* err, size_t err_len) {
    OwlLdapNegotiate* n = (OwlLdapNegotiate*)calloc(1, sizeof *n);
    if (!n) { if (err) snprintf(err, err_len, "memoire epuisee"); return NULL; }
    snprintf(n->spn, sizeof n->spn, "%s", spn ? spn : "");

    TimeStamp expiry;
    SECURITY_STATUS ss = AcquireCredentialsHandleA(
        NULL, (SEC_CHAR*)"Negotiate", SECPKG_CRED_OUTBOUND,
        NULL, NULL, NULL, NULL, &n->cred, &expiry);
    if (ss != SEC_E_OK) {
        if (err) snprintf(err, err_len,
            "SSPI AcquireCredentialsHandle (Negotiate) a echoue (0x%08lx)",
            (unsigned long)ss);
        free(n);
        return NULL;
    }
    n->cred_ok = true;
    return n;
}

OwlLdapTokenProvider owl_ldap_negotiate_provider(OwlLdapNegotiate* n) {
    OwlLdapTokenProvider p;
    p.init = sspi_init;
    p.step = sspi_step;
    p.self = n;
    return p;
}

const char* owl_ldap_negotiate_last_error(const OwlLdapNegotiate* n) {
    return n ? n->last_error : "";
}

void owl_ldap_negotiate_destroy(OwlLdapNegotiate* n) {
    if (!n) return;
    if (n->ctx_ok)  DeleteSecurityContext(&n->ctx);
    if (n->cred_ok) FreeCredentialsHandle(&n->cred);
    free(n->cb);
    free(n->cbbuf);
    free(n->token);
    free(n);
}

/* ================================================================
 * Backend POSIX : GSSAPI (mecanisme SPNEGO), bibliotheque systeme.
 *
 * Aucune dependance de lien ni de header : l'ABI GSSAPI (RFC 2744) est
 * figee depuis 25 ans, on en declare le sous-ensemble utilise et on charge
 * la bibliotheque a l'execution (dlopen). Consequence : le binaire reste
 * chargeable sans krb5 installe ; owl_ldap_negotiate_create echoue
 * proprement si libgssapi est absente (hote non joint au domaine). Non
 * autonome par nature (exige krb5.conf + ticket cache sur l'hote).
 * ================================================================ */
#else

#include <dlfcn.h>
#include <stdint.h>

typedef uint32_t OM_uint32;
typedef struct gss_name_struct*    gss_name_t;
typedef struct gss_ctx_id_struct*  gss_ctx_id_t;
typedef struct gss_cred_id_struct* gss_cred_id_t;

typedef struct gss_OID_desc_struct {
    OM_uint32 length;
    void*     elements;
} gss_OID_desc, *gss_OID;

typedef struct gss_buffer_desc_struct {
    size_t length;
    void*  value;
} gss_buffer_desc, *gss_buffer_t;

typedef struct gss_channel_bindings_struct {
    OM_uint32       initiator_addrtype;
    gss_buffer_desc initiator_address;
    OM_uint32       acceptor_addrtype;
    gss_buffer_desc acceptor_address;
    gss_buffer_desc application_data;
}* gss_channel_bindings_t;

#define GSS_C_NO_CONTEXT          ((gss_ctx_id_t)0)
#define GSS_C_NO_CREDENTIAL       ((gss_cred_id_t)0)
#define GSS_C_NO_BUFFER           ((gss_buffer_t)0)
#define GSS_C_NO_CHANNEL_BINDINGS ((gss_channel_bindings_t)0)

#define GSS_C_MUTUAL_FLAG  2
#define GSS_C_INTEG_FLAG   32

#define GSS_C_CALLING_ERROR_OFFSET 24
#define GSS_C_ROUTINE_ERROR_OFFSET 16
#define GSS_S_CONTINUE_NEEDED (1u << 0)
#define GSS_ERROR(x) ((x) & ((0xffu << GSS_C_CALLING_ERROR_OFFSET) \
                          | (0xffu << GSS_C_ROUTINE_ERROR_OFFSET)))

typedef OM_uint32 (*fn_import_name)(OM_uint32*, const gss_buffer_t,
                                    const gss_OID, gss_name_t*);
typedef OM_uint32 (*fn_init_sec_context)(OM_uint32*, const gss_cred_id_t,
                                         gss_ctx_id_t*, const gss_name_t,
                                         const gss_OID, OM_uint32, OM_uint32,
                                         const gss_channel_bindings_t,
                                         const gss_buffer_t, gss_OID*,
                                         gss_buffer_t, OM_uint32*, OM_uint32*);
typedef OM_uint32 (*fn_release_buffer)(OM_uint32*, gss_buffer_t);
typedef OM_uint32 (*fn_release_name)(OM_uint32*, gss_name_t*);
typedef OM_uint32 (*fn_delete_sec_context)(OM_uint32*, gss_ctx_id_t*,
                                           gss_buffer_t);

struct OwlLdapNegotiate {
    void*                 lib;
    fn_import_name        p_import_name;
    fn_init_sec_context   p_init_sec_context;
    fn_release_buffer     p_release_buffer;
    fn_release_name       p_release_name;
    fn_delete_sec_context p_delete_sec_context;
    gss_name_t            target;
    gss_ctx_id_t          ctx;
    unsigned char*        cb;
    size_t                cb_len;
    unsigned char*        token;
    size_t                token_len;
    char                  last_error[256];
};

/* OID du mecanisme SPNEGO : 1.3.6.1.5.5.2. */
static unsigned char gss_spnego_oid[]   = {0x2b,0x06,0x01,0x05,0x05,0x02};
/* GSS_C_NT_HOSTBASED_SERVICE : 1.2.840.113554.1.2.1.4. */
static unsigned char gss_hostbased_oid[] =
    {0x2a,0x86,0x48,0x86,0xf7,0x12,0x01,0x02,0x01,0x04};

static OwlLdapSaslToken gss_do_step(OwlLdapNegotiate* n,
                                    const unsigned char* srv, size_t srv_len) {
    gss_OID_desc mech;
    mech.length   = sizeof gss_spnego_oid;
    mech.elements = gss_spnego_oid;

    gss_buffer_desc in;
    in.length = (srv && srv_len > 0) ? srv_len : 0;
    in.value  = (void*)srv;

    gss_buffer_desc output;
    output.length = 0;
    output.value  = NULL;

    struct gss_channel_bindings_struct cbs;
    gss_channel_bindings_t pcb = GSS_C_NO_CHANNEL_BINDINGS;
    if (n->cb && n->cb_len > 0) {
        memset(&cbs, 0, sizeof cbs);
        cbs.application_data.length = n->cb_len;
        cbs.application_data.value  = n->cb;
        pcb = &cbs;
    }

    /* Couche de securite SASL = none (cf. backend SSPI) : mutual auth seul, NI
     * integrite NI confidentialite — AD refuse sign/seal par-dessus TLS. */
    OM_uint32 minor = 0, ret_flags = 0;
    OM_uint32 maj = n->p_init_sec_context(
        &minor, GSS_C_NO_CREDENTIAL, &n->ctx, n->target, &mech,
        GSS_C_MUTUAL_FLAG, 0, pcb,
        (srv && srv_len > 0) ? &in : GSS_C_NO_BUFFER,
        NULL, &output, &ret_flags, NULL);

    if (GSS_ERROR(maj)) {
        snprintf(n->last_error, sizeof n->last_error,
                 "GSSAPI gss_init_sec_context a echoue (major=0x%08x minor=0x%08x)",
                 (unsigned)maj, (unsigned)minor);
        if (output.value) n->p_release_buffer(&minor, &output);
        return sasl_token_fail();
    }

    free(n->token);
    n->token = NULL;
    n->token_len = 0;
    if (output.length > 0 && output.value) {
        n->token = (unsigned char*)malloc(output.length);
        if (!n->token) {
            snprintf(n->last_error, sizeof n->last_error,
                     "memoire epuisee (jeton GSSAPI)");
            n->p_release_buffer(&minor, &output);
            return sasl_token_fail();
        }
        memcpy(n->token, output.value, output.length);
        n->token_len = output.length;
    }
    if (output.value) n->p_release_buffer(&minor, &output);

    OwlLdapSaslToken out;
    out.ok        = true;
    out.done      = ((maj & GSS_S_CONTINUE_NEEDED) == 0);
    out.token     = n->token;
    out.token_len = n->token_len;
    return out;
}

static OwlLdapSaslToken gss_init(void* self,
                                 const unsigned char* cb, size_t cb_len) {
    OwlLdapNegotiate* n = (OwlLdapNegotiate*)self;
    free(n->cb);
    n->cb = NULL;
    n->cb_len = 0;
    if (cb && cb_len > 0) {
        n->cb = (unsigned char*)malloc(cb_len);
        if (n->cb) { memcpy(n->cb, cb, cb_len); n->cb_len = cb_len; }
    }
    return gss_do_step(n, NULL, 0);
}

static OwlLdapSaslToken gss_step(void* self,
                                 const unsigned char* srv, size_t srv_len) {
    return gss_do_step((OwlLdapNegotiate*)self, srv, srv_len);
}

OwlLdapNegotiate* owl_ldap_negotiate_create(const char* spn,
                                            char* err, size_t err_len) {
    OwlLdapNegotiate* n = (OwlLdapNegotiate*)calloc(1, sizeof *n);
    if (!n) { if (err) snprintf(err, err_len, "memoire epuisee"); return NULL; }

    const char* libs[] = { "libgssapi_krb5.so.2", "libgssapi_krb5.so",
                           "libgssapi.so.3", "libgssapi.so", NULL };
    for (int i = 0; libs[i] && !n->lib; i++) {
        n->lib = dlopen(libs[i], RTLD_NOW | RTLD_GLOBAL);
    }
    if (!n->lib) {
        if (err) snprintf(err, err_len,
            "GSSAPI indisponible : libgssapi introuvable (hote non joint au domaine ?)");
        free(n);
        return NULL;
    }

    n->p_import_name        = (fn_import_name)        dlsym(n->lib, "gss_import_name");
    n->p_init_sec_context   = (fn_init_sec_context)   dlsym(n->lib, "gss_init_sec_context");
    n->p_release_buffer     = (fn_release_buffer)     dlsym(n->lib, "gss_release_buffer");
    n->p_release_name       = (fn_release_name)       dlsym(n->lib, "gss_release_name");
    n->p_delete_sec_context = (fn_delete_sec_context) dlsym(n->lib, "gss_delete_sec_context");
    if (!n->p_import_name || !n->p_init_sec_context || !n->p_release_buffer
        || !n->p_release_name || !n->p_delete_sec_context) {
        if (err) snprintf(err, err_len, "GSSAPI : symboles gss_* introuvables");
        dlclose(n->lib);
        free(n);
        return NULL;
    }

    /* "ldap/host" -> "ldap@host" (nom de service hostbased GSSAPI). */
    char svc[256];
    snprintf(svc, sizeof svc, "%s", spn ? spn : "");
    for (char* s = svc; *s; s++) { if (*s == '/') { *s = '@'; break; } }

    gss_OID_desc nt;
    nt.length   = sizeof gss_hostbased_oid;
    nt.elements = gss_hostbased_oid;
    gss_buffer_desc namebuf;
    namebuf.length = strlen(svc);
    namebuf.value  = svc;

    OM_uint32 minor = 0;
    OM_uint32 maj = n->p_import_name(&minor, &namebuf, &nt, &n->target);
    if (GSS_ERROR(maj)) {
        if (err) snprintf(err, err_len,
            "GSSAPI gss_import_name a echoue (0x%08x)", (unsigned)maj);
        dlclose(n->lib);
        free(n);
        return NULL;
    }
    n->ctx = GSS_C_NO_CONTEXT;
    return n;
}

OwlLdapTokenProvider owl_ldap_negotiate_provider(OwlLdapNegotiate* n) {
    OwlLdapTokenProvider p;
    p.init = gss_init;
    p.step = gss_step;
    p.self = n;
    return p;
}

const char* owl_ldap_negotiate_last_error(const OwlLdapNegotiate* n) {
    return n ? n->last_error : "";
}

void owl_ldap_negotiate_destroy(OwlLdapNegotiate* n) {
    if (!n) return;
    OM_uint32 minor = 0;
    if (n->ctx != GSS_C_NO_CONTEXT)
        n->p_delete_sec_context(&minor, &n->ctx, GSS_C_NO_BUFFER);
    if (n->target)
        n->p_release_name(&minor, &n->target);
    if (n->lib) dlclose(n->lib);
    free(n->cb);
    free(n->token);
    free(n);
}

#endif /* _WIN32 */
