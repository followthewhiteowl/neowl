// SPDX-License-Identifier: AGPL-3.0-only WITH LicenseRef-OWL-Runtime-Exception
/* stdlib_net.c — Natives d'information reseau portables (famille NetXxx).
 *
 * Couche mince au-dessus de l'API socket standard (gethostname,
 * getaddrinfo/getnameinfo, inet_ntop). Aucune dependance externe, lien
 * statique. Isole des introspections systeme (stdlib_runtime.c) car tire
 * winsock2.h sous Win32 — stdlib_runtime.c evite deliberement windows.h
 * (collision TokenType scanner.h vs winnt.h sous le build GUI).
 *
 * Lot 1 : identite + IP (NetNomMachine, NetAdresseIP, NetListeAdresseIP).
 * Refs canon (reformulees cote user, cf CLAUDE.md regle 7) :
 *   Communication/Autres/NetNomMachine, NetAdresseIP, NetListeAdresseIP.
 * Divergence assumee  : NetListeAdresseIP renvoie un tableau de
 * chaines (le canon renvoie une chaine separee par RC).
 */

#include "owl_stdlib.h"
#include "owl_registry.h"
#include "net_tcp.h"       /* owl_tcp_global_init (WSAStartup idempotent) */
#include "vm.h"
#include "object.h"
#include "value.h"
#include "common.h"

#ifdef _WIN32
  #include <ws2tcpip.h>    /* getaddrinfo/getnameinfo/inet_ntop/NI_MAXHOST */
  #include <iphlpapi.h>    /* GetAdaptersAddresses (enumeration des cartes) */
#else
  #include <sys/types.h>
  #include <sys/socket.h>
  #include <netdb.h>
  #include <arpa/inet.h>
  #include <unistd.h>      /* gethostname */
  #include <ifaddrs.h>     /* getifaddrs */
  #include <net/if.h>      /* IFF_LOOPBACK */
  #if defined(__APPLE__)
    #include <net/if_dl.h>      /* sockaddr_dl / LLADDR (AF_LINK) */
  #else
    #include <netpacket/packet.h>  /* sockaddr_ll (AF_PACKET) */
  #endif
#endif

#include <stdlib.h>
#include <string.h>

/* IPv4 et IPv6 — bitflags combinables (filtre de NetListeAdresseIP). */
#define OWL_NET_IPV4 1
#define OWL_NET_IPV6 2

static Value net_empty_string(void) {
    return OBJ_VAL(obj_copy_string("", 0));
}

/* Resout le nom d'hote local (buf >= 256). Retourne NULL si indeterminable. */
static const char* net_local_host(char* buf, int buflen) {
    if (gethostname(buf, buflen) != 0) return NULL;
    buf[buflen - 1] = '\0';
    return buf;
}

/* ============================================================
 * NetNomMachine([<AdresseIP>]) -> chaine
 *   0 arg : nom de la machine locale (gethostname).
 *   1 arg : resolution DNS inverse de l'adresse IP fournie. Resultat
 *           possiblement vide selon la configuration reseau — jamais une
 *           erreur bloquante. Adresse malformee -> chaine vide.
 * ============================================================ */
static Value fn_net_nom_machine(int argCount, Value* args) {
    if (argCount >= 1 && IS_STRING(args[0]) && AS_STRING(args[0])->length > 0) {
        const char* ip = AS_CSTRING(args[0]);
        struct addrinfo hints;
        memset(&hints, 0, sizeof(hints));
        hints.ai_flags    = AI_NUMERICHOST;  /* parse l'IP sans DNS direct */
        hints.ai_family   = AF_UNSPEC;
        hints.ai_socktype = SOCK_STREAM;
        struct addrinfo* res = NULL;
        if (getaddrinfo(ip, NULL, &hints, &res) != 0 || res == NULL) {
            if (res) freeaddrinfo(res);
            return net_empty_string();  /* adresse malformee */
        }
        char hostname[NI_MAXHOST];
        int rc = getnameinfo(res->ai_addr, (socklen_t)res->ai_addrlen,
                             hostname, sizeof(hostname), NULL, 0, 0);
        Value out = (rc == 0)
            ? OBJ_VAL(obj_copy_string(hostname, (int)strlen(hostname)))
            : net_empty_string();
        freeaddrinfo(res);
        return out;
    }

    char buf[256];
    const char* name = net_local_host(buf, (int)sizeof(buf));
    if (name == NULL) return net_empty_string();
    return OBJ_VAL(obj_copy_string(name, (int)strlen(name)));
}

/* ============================================================
 * NetAdresseIP([<NomPoste>] [, <Indice>]) -> chaine
 *   IPv4 uniquement. Accesseur indexe (0 = premiere adresse IPv4 du poste).
 *   NomPoste vide ou absent : machine locale. Indice hors liste, indice
 *   negatif, ou poste introuvable -> chaine vide.
 * ============================================================ */
static Value fn_net_adresse_ip(int argCount, Value* args) {
    const char* host;
    char buf[256];
    if (argCount >= 1 && IS_STRING(args[0]) && AS_STRING(args[0])->length > 0) {
        host = AS_CSTRING(args[0]);
    } else {
        host = net_local_host(buf, (int)sizeof(buf));
        if (host == NULL) return net_empty_string();
    }

    int index = 0;
    if (argCount >= 2 && IS_INTEGER(args[1])) index = (int)AS_INTEGER(args[1]);
    if (index < 0) return net_empty_string();  /* pas une position valide */

    struct addrinfo hints;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family   = AF_INET;       /* IPv4 seul */
    hints.ai_socktype = SOCK_STREAM;   /* deduplique les familles socket */
    struct addrinfo* res = NULL;
    if (getaddrinfo(host, NULL, &hints, &res) != 0) return net_empty_string();

    Value out = net_empty_string();
    int i = 0;
    for (struct addrinfo* p = res; p != NULL; p = p->ai_next) {
        if (p->ai_family != AF_INET) continue;
        if (i == index) {
            char ipbuf[INET_ADDRSTRLEN];
            struct sockaddr_in* sa = (struct sockaddr_in*)p->ai_addr;
            if (inet_ntop(AF_INET, &sa->sin_addr, ipbuf, sizeof(ipbuf)) != NULL) {
                out = OBJ_VAL(obj_copy_string(ipbuf, (int)strlen(ipbuf)));
            }
            break;
        }
        i++;
    }
    freeaddrinfo(res);
    return out;
}

/* ============================================================
 * NetListeAdresseIP([<Machine>] [, <Options>]) -> tableau de chaines
 *   Divergence assumee : renvoie un tableau de chaines.
 *   Machine vide ou absente : machine locale. Options = enum combinable
 *   IPv4/IPv6, absent -> les deux. Machine introuvable -> tableau vide.
 * ============================================================ */
static Value fn_net_liste_adresse_ip(int argCount, Value* args) {
    ObjArray* arr = obj_new_array();
    vm_push(OBJ_VAL(arr));  /* root pendant la construction (strings allouees) */

    const char* host;
    char buf[256];
    if (argCount >= 1 && IS_STRING(args[0]) && AS_STRING(args[0])->length > 0) {
        host = AS_CSTRING(args[0]);
    } else {
        host = net_local_host(buf, (int)sizeof(buf));
        if (host == NULL) { vm_pop(); return OBJ_VAL(arr); }
    }

    int options = OWL_NET_IPV4 | OWL_NET_IPV6;  /* defaut : les deux */
    if (argCount >= 2 && IS_INTEGER(args[1])) {
        options = (int)AS_INTEGER(args[1]);
        if (options == 0) options = OWL_NET_IPV4 | OWL_NET_IPV6;
    }
    bool wantV4 = (options & OWL_NET_IPV4) != 0;
    bool wantV6 = (options & OWL_NET_IPV6) != 0;
    int family = AF_UNSPEC;
    if (wantV4 && !wantV6)      family = AF_INET;
    else if (wantV6 && !wantV4) family = AF_INET6;

    struct addrinfo hints;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family   = family;
    hints.ai_socktype = SOCK_STREAM;   /* deduplique les familles socket */
    struct addrinfo* res = NULL;
    if (getaddrinfo(host, NULL, &hints, &res) == 0) {
        for (struct addrinfo* p = res; p != NULL; p = p->ai_next) {
            char ipbuf[INET6_ADDRSTRLEN];
            const char* r = NULL;
            if (p->ai_family == AF_INET) {
                struct sockaddr_in* sa = (struct sockaddr_in*)p->ai_addr;
                r = inet_ntop(AF_INET, &sa->sin_addr, ipbuf, sizeof(ipbuf));
            } else if (p->ai_family == AF_INET6) {
                struct sockaddr_in6* sa = (struct sockaddr_in6*)p->ai_addr;
                r = inet_ntop(AF_INET6, &sa->sin6_addr, ipbuf, sizeof(ipbuf));
            }
            if (r != NULL) {
                obj_array_push(arr, OBJ_VAL(obj_copy_string(ipbuf, (int)strlen(ipbuf))));
            }
        }
        freeaddrinfo(res);
    }

    vm_pop();
    return OBJ_VAL(arr);
}

/* Formate six octets en "aa:bb:cc:dd:ee:ff" (minuscules). out >= 18. */
static void net_fmt_mac(const unsigned char* m, char* out) {
    static const char hx[] = "0123456789abcdef";
    int j = 0;
    for (int i = 0; i < 6; i++) {
        out[j++] = hx[(m[i] >> 4) & 0xF];
        out[j++] = hx[m[i] & 0xF];
        if (i < 5) out[j++] = ':';
    }
    out[j] = '\0';
}

/* Ecrit dans out (>= 18) la MAC de la carte physique a l'indice `index`
 * (0 = premiere). Les cartes loopback et sans adresse (MAC != 6 octets ou
 * tout a zero) sont ignorees et ne comptent pas dans l'indexation.
 * Retourne true si une carte a ete trouvee a cet indice. */
static bool net_mac_at_index(int index, char* out) {
#ifdef _WIN32
    ULONG flags = GAA_FLAG_SKIP_UNICAST | GAA_FLAG_SKIP_ANYCAST |
                  GAA_FLAG_SKIP_MULTICAST | GAA_FLAG_SKIP_DNS_SERVER;
    ULONG size = 15000;  /* taille indicative recommandee par l'API */
    IP_ADAPTER_ADDRESSES* buf = (IP_ADAPTER_ADDRESSES*)malloc(size);
    if (buf == NULL) return false;
    ULONG r = GetAdaptersAddresses(AF_UNSPEC, flags, NULL, buf, &size);
    if (r == ERROR_BUFFER_OVERFLOW) {
        free(buf);
        buf = (IP_ADAPTER_ADDRESSES*)malloc(size);
        if (buf == NULL) return false;
        r = GetAdaptersAddresses(AF_UNSPEC, flags, NULL, buf, &size);
    }
    if (r != NO_ERROR) { free(buf); return false; }

    int i = 0;
    bool found = false;
    for (IP_ADAPTER_ADDRESSES* a = buf; a != NULL; a = a->Next) {
        if (a->IfType == IF_TYPE_SOFTWARE_LOOPBACK) continue;
        if (a->PhysicalAddressLength != 6) continue;
        const unsigned char* m = a->PhysicalAddress;
        if ((m[0]|m[1]|m[2]|m[3]|m[4]|m[5]) == 0) continue;  /* MAC nulle */
        if (i == index) { net_fmt_mac(m, out); found = true; break; }
        i++;
    }
    free(buf);
    return found;
#else
    struct ifaddrs* ifaddr = NULL;
    if (getifaddrs(&ifaddr) != 0) return false;

    int i = 0;
    bool found = false;
    for (struct ifaddrs* ifa = ifaddr; ifa != NULL; ifa = ifa->ifa_next) {
        if (ifa->ifa_addr == NULL) continue;
        if (ifa->ifa_flags & IFF_LOOPBACK) continue;
        unsigned char mac[6];
  #if defined(__APPLE__)
        if (ifa->ifa_addr->sa_family != AF_LINK) continue;
        struct sockaddr_dl* sdl = (struct sockaddr_dl*)ifa->ifa_addr;
        if (sdl->sdl_alen != 6) continue;
        memcpy(mac, LLADDR(sdl), 6);
  #else
        if (ifa->ifa_addr->sa_family != AF_PACKET) continue;
        struct sockaddr_ll* sll = (struct sockaddr_ll*)ifa->ifa_addr;
        if (sll->sll_halen != 6) continue;
        memcpy(mac, sll->sll_addr, 6);
  #endif
        if ((mac[0]|mac[1]|mac[2]|mac[3]|mac[4]|mac[5]) == 0) continue;
        if (i == index) { net_fmt_mac(mac, out); found = true; break; }
        i++;
    }
    freeifaddrs(ifaddr);
    return found;
#endif
}

/* ============================================================
 * NetAdresseMAC([<RangCarte>]) -> chaine
 *   0 arg : adresse MAC de la premiere carte reseau physique.
 *   1 arg : carte a l'indice donne (0 = premiere). Rang negatif -> premiere
 *           carte. Rang hors liste ou aucune carte -> chaine vide.
 *   Format : "aa:bb:cc:dd:ee:ff" (minuscules, ':' separateur).
 *   Ref canon (reformulee cote user, regle 7) :
 *     Communication/Autres/NetAdresseMAC.
 * ============================================================ */
static Value fn_net_adresse_mac(int argCount, Value* args) {
    int index = 0;
    if (argCount >= 1 && IS_INTEGER(args[0])) index = (int)AS_INTEGER(args[0]);
    if (index < 0) index = 0;  /* rang negatif = premiere carte */

    char mac[18];
    if (net_mac_at_index(index, mac)) {
        return OBJ_VAL(obj_copy_string(mac, (int)strlen(mac)));
    }
    return net_empty_string();
}

/* ============================================================
 * Enum combinable IPv4/IPv6 (filtre de NetListeAdresseIP)
 * ============================================================ */
static const char* const E_netVersionIP_values[] = {
    "IPv4",
    "IPv6",
    NULL
};
const OwlEnum E_netVersionIP = {
    .name="netVersionIP", .nameAscii="netVersionIP", .nameEN="netIPVersion",
    .category="R\xc3\xa9seau",
    .descFR="Familles d'adresses IP a inclure (bitflags combinables : IPv4, IPv6).",
    .descEN="IP address families to include (combinable bitflags: IPv4, IPv6).",
    .values=E_netVersionIP_values, .combinable=true
};

static const OwlEnum* const NET_ENUMS[] = { &E_netVersionIP };

static const OwlConstDef NET_CONSTS[] = {
    { "IPv4", "R\xc3\xa9seau", INTEGER_VAL(OWL_NET_IPV4),
      "Adresses IPv4 (quatre octets, ex. 192.168.0.1).",
      "IPv4 addresses (four octets, e.g. 192.168.0.1).", "IPv4" },
    { "IPv6", "R\xc3\xa9seau", INTEGER_VAL(OWL_NET_IPV6),
      "Adresses IPv6 (huit groupes hexadecimaux, ex. fe80::1).",
      "IPv6 addresses (eight hexadecimal groups, e.g. fe80::1).", "IPv6" },
};

/* ============================================================
 * Registry
 * ============================================================ */
static const OwlParam p_none[] = {{0}};

static const OwlParam p_net_nom_machine[] = {
    {"adresseIP", "cha\xc3\xaene", "adresse IP a resoudre en nom (DNS inverse)"},
    {0}
};
static const OwlSignature sig_net_nom_machine[] = {
    {0, p_none,            "cha\xc3\xaene", "nom de la machine locale"},
    {1, p_net_nom_machine, "cha\xc3\xaene", "nom resolu depuis l'adresse (vide si indeterminable)"},
    {0}
};

static const OwlParam p_net_adresse_ip_host[] = {
    {"nomPoste", "cha\xc3\xaene", "nom du poste (vide = machine locale)"},
    {0}
};
static const OwlParam p_net_adresse_ip_host_idx[] = {
    {"nomPoste", "cha\xc3\xaene", "nom du poste (vide = machine locale)"},
    {"indice",   "entier",       "indice 0-base de l'adresse IPv4 (0 = premiere)"},
    {0}
};
static const OwlSignature sig_net_adresse_ip[] = {
    {0, p_none,                  "cha\xc3\xaene", "premiere adresse IPv4 de la machine locale"},
    {1, p_net_adresse_ip_host,   "cha\xc3\xaene", "premiere adresse IPv4 du poste"},
    {2, p_net_adresse_ip_host_idx,"cha\xc3\xaene","adresse IPv4 a l'indice donne (vide si hors liste)"},
    {0}
};

static const OwlParam p_net_liste_machine[] = {
    {"machine", "cha\xc3\xaene", "nom de la machine (vide = machine locale)"},
    {0}
};
static const OwlParam p_net_liste_machine_opt[] = {
    {"machine", "cha\xc3\xaene", "nom de la machine (vide = machine locale)"},
    {"options", "entier",       "familles a inclure (IPv4, IPv6, combinables)", .allowedBase=&E_netVersionIP},
    {0}
};
static const OwlSignature sig_net_liste_adresse_ip[] = {
    {0, p_none,                   "tableau de cha\xc3\xaenes", "toutes les adresses IPv4 et IPv6 de la machine locale"},
    {1, p_net_liste_machine,      "tableau de cha\xc3\xaenes", "adresses de la machine donnee"},
    {2, p_net_liste_machine_opt,  "tableau de cha\xc3\xaenes", "adresses filtrees par famille"},
    {0}
};

static const OwlParam p_net_adresse_mac[] = {
    {"rangCarte", "entier", "indice 0-base de la carte reseau (0 = premiere ; negatif = premiere)"},
    {0}
};
static const OwlSignature sig_net_adresse_mac[] = {
    {0, p_none,             "cha\xc3\xaene", "adresse MAC de la premiere carte reseau"},
    {1, p_net_adresse_mac,  "cha\xc3\xaene", "adresse MAC de la carte au rang donne (vide si hors liste)"},
    {0}
};

static const OwlNativeDef DEFS[] = {
    { .canonical="NetNomMachine", .nameEN="NetMachineName", .category="R\xc3\xa9seau",
      .descFR="Renvoie le nom reseau d'une machine. Sans argument, le nom de la machine locale. Avec une adresse IP, le nom resolu par DNS inverse (chaine vide si l'adresse ne resout vers aucun nom ou si elle est mal formee). N'echoue jamais : une resolution impossible renvoie une chaine vide.",
      .descEN="Returns the network name of a machine. With no argument, the local machine name. With an IP address, the name resolved by reverse DNS (empty string if the address resolves to no name or is malformed). Never fails: an impossible resolution returns an empty string.",
      .signatures=sig_net_nom_machine, .impl=fn_net_nom_machine },
    { .canonical="NetAdresseIP", .nameEN="NetIPAddress", .category="R\xc3\xa9seau",
      .descFR="Renvoie une adresse IPv4 d'un poste, accedee par indice (0 = premiere adresse). Sans nom de poste, la machine locale est utilisee. Renvoie une chaine vide si l'indice depasse le nombre d'adresses disponibles, si l'indice est negatif, ou si le poste est introuvable.",
      .descEN="Returns an IPv4 address of a host, accessed by index (0 = first address). With no host name, the local machine is used. Returns an empty string if the index exceeds the number of available addresses, if the index is negative, or if the host cannot be found.",
      .signatures=sig_net_adresse_ip, .impl=fn_net_adresse_ip },
    { .canonical="NetListeAdresseIP", .nameEN="NetListIPAddress", .category="R\xc3\xa9seau",
      .descFR="Renvoie la liste des adresses IP d'une machine sous forme de tableau de chaines. Sans argument, toutes les adresses IPv4 et IPv6 de la machine locale. Avec un nom de machine, les adresses de cette machine (tableau vide si elle est introuvable). Les options IPv4 / IPv6 (combinables) filtrent les familles renvoyees ; en leur absence, les deux familles sont incluses.",
      .descEN="Returns the list of IP addresses of a machine as an array of strings. With no argument, all IPv4 and IPv6 addresses of the local machine. With a machine name, the addresses of that machine (empty array if it cannot be found). The IPv4 / IPv6 options (combinable) filter the returned families; when omitted, both families are included.",
      .signatures=sig_net_liste_adresse_ip, .impl=fn_net_liste_adresse_ip },
    { .canonical="NetAdresseMAC", .nameEN="NetMACAddress", .category="R\xc3\xa9seau",
      .descFR="Renvoie l'adresse physique (MAC) d'une carte reseau locale, au format aa:bb:cc:dd:ee:ff. Sans argument, l'adresse de la premiere carte reseau physique. Avec un rang de carte (0 = premiere), la carte correspondante sur une machine a plusieurs cartes ; un rang negatif designe la premiere carte. Les interfaces de boucle locale et sans adresse physique sont ignorees. Renvoie une chaine vide si aucune carte n'est disponible ou si le rang depasse le nombre de cartes.",
      .descEN="Returns the physical (MAC) address of a local network adapter, formatted as aa:bb:cc:dd:ee:ff. With no argument, the address of the first physical network adapter. With a card index (0 = first), the matching adapter on a multi-adapter machine; a negative index selects the first adapter. Loopback and address-less interfaces are skipped. Returns an empty string if no adapter is available or if the index exceeds the number of adapters.",
      .signatures=sig_net_adresse_mac, .impl=fn_net_adresse_mac },
};

void stdlib_net_init(void) {
    /* WSAStartup sous Win32 (idempotent, refcompte avec la couche TCP).
     * No-op renvoyant true sous POSIX. Pour la vie du process. */
    owl_tcp_global_init();
    owl_registry_register_module(DEFS, (int)(sizeof(DEFS)/sizeof(DEFS[0])));
    owl_registry_register_enums (NET_ENUMS, (int)(sizeof(NET_ENUMS)/sizeof(NET_ENUMS[0])));
    owl_registry_register_consts(NET_CONSTS,(int)(sizeof(NET_CONSTS)/sizeof(NET_CONSTS[0])));
}
