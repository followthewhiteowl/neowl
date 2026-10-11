#!/usr/bin/env bash
# Harnais boite noire pour `owl serve` en mode projet MULTI-SERVICE
# (, decision coordinator post-verif empirique 2026-09-10).
#
# Quatre montages, quatre fixtures independantes (tests/internal_fixtures/
# cli_serve_multi_service/{collision,union_distinctes,mono,disjoint_urlbase}/) :
#
#   - collision/         : 2 ServiceREST voisins declarent la MEME route
#                          exacte (GET /alpha). Comportement CIBLE : `owl
#                          serve` DOIT echouer au demarrage (exit != 0,
#                          message nommant le conflit) au lieu de servir
#                          silencieusement le premier-declare-gagne.
#                          AUJOURD'HUI : serve demarre quand meme (bug) ->
#                          rouge attendu (timeout = serve tourne, la
#                          collision n'a jamais ete detectee).
#   - union_distinctes/  : 2 ServiceREST voisins, routes DISTINCTES
#                          (GET /alpha, GET /beta). Deja fonctionnel
#                          (union multi-service) : les 2 routes DOIVENT
#                          repondre 200 avec leur corps propre. Mais le
#                          message de demarrage ne nomme QUE le 1er
#                          service -> rouge attendu sur cette 2e
#                          assertion (message honnete =  lot B).
#   - mono/              : 1 seul ServiceREST. Non-regression : route
#                          servie + message inchanges. Vert attendu.
#   - disjoint_urlbase/  : 2 ServiceREST voisins montent le MEME motif de
#                          route ([id]) sous des UrlBase disjointes
#                          (clients vs commandes). Controle positif
#                          : pas de faux-positif de collision
#                          quand le routeur compare le chemin complet
#                          (UrlBase + Chemin), pas seulement le suffixe.
#
# `owl serve` reste bloque en ecoute (jusqu'a Ctrl-C ou kill) : meme
# patron que tests/cli/serve_projet_i18n/run.sh -- lance en tache de
# fond (sauf le cas collision, borne par `timeout` en avant-plan car il
# n'y a rien a curler s'il echoue comme prevu), tue systematiquement le
# process a la fin (jamais de serveur de test qui survit a ce script).
#
# Usage:
#   ./run.sh
#   OWL_BIN=/path/to/owl.exe ./run.sh
#
# Exit code : 0 si tout est VERT (collision detectee + message honnete +
# union/mono servis), 1 si au moins un cas est ROUGE (etat actuel :
# collision non detectee + message incomplet), 2 si owl.exe introuvable.

set -uo pipefail

ROOT="$(cd "$(dirname "$0")" && pwd)"
OWL_ROOT="$(cd "$ROOT/../../.." && pwd)"

OWL="${OWL_BIN:-$OWL_ROOT/build/owl.exe}"
if [ ! -f "$OWL" ]; then
    echo "owl.exe introuvable: $OWL — lance 'make' d'abord"
    exit 2
fi

FIXT_ROOT="$OWL_ROOT/tests/internal_fixtures/cli_serve_multi_service"
FAIL=0

# --- Cas 1 : collision -------------------------------------------------
# `owl serve` DOIT echouer AVANT de servir. Comme il n'y a rien a curler
# si ca marche comme prevu, on lance en AVANT-PLAN sous `timeout` :
#   - exit 124 (timeout atteint)      -> serve a demarre et ecoute encore
#                                         -> collision NON detectee -> ROUGE
#                                         (bonne raison : bug reproduit).
#   - exit != 0 et != 124             -> serve a refuse de demarrer.
#                                         VERT si le message nomme le
#                                         conflit (route /alpha, "collision"
#                                         / "conflit" / "déjà"), ROUGE
#                                         (mauvaise raison) sinon.
#   - exit 0                          -> anomalie (serve ne devrait jamais
#                                         sortir 0 tout seul) -> ROUGE.
cas_collision() {
    local port="${OWL_SERVE_TEST_PORT_COLLISION:-18991}"
    local out; out="$(mktemp)"

    ( cd "$FIXT_ROOT/collision" && timeout 8 "$OWL" serve --port "$port" ) > "$out" 2>&1
    local rc=$?

    local contenu; contenu="$(cat "$out")"
    rm -f "$out"

    if [ "$rc" -eq 124 ]; then
        echo "cli_serve_multi_service_collision: RESULT: RED (bonne raison -- serve a demarre et ecoute encore apres 8s, la collision GET /alpha entre ServiceAlpha et ServiceBeta n'est pas detectee, premier-gagne silencieux)"
        echo "--- sortie ---"
        echo "$contenu"
        return 1
    fi

    if [ "$rc" -eq 0 ]; then
        echo "cli_serve_multi_service_collision: RESULT: RED (MAUVAISE raison -- serve est sorti avec exit 0, ne devrait jamais arriver)"
        echo "--- sortie ---"
        echo "$contenu"
        return 1
    fi

    # exit != 0 et != 124 : serve a refuse de demarrer. Verifier que le
    # message nomme le conflit (pas juste un crash quelconque).
    if echo "$contenu" | grep -qiE "alpha" && echo "$contenu" | grep -qiE "collision|conflit|déjà|deja"; then
        echo "cli_serve_multi_service_collision: RESULT: GREEN (exit=$rc, message nomme le conflit de route /alpha)"
        return 0
    fi

    echo "cli_serve_multi_service_collision: RESULT: RED (MAUVAISE raison -- exit=$rc mais le message ne nomme pas clairement le conflit /alpha)"
    echo "--- sortie ---"
    echo "$contenu"
    return 1
}

# --- Cas 2 : union_distinctes -------------------------------------------
# Lance en tache de fond, attend le montage, curl /alpha ET /beta, verifie
# le message de demarrage (doit nommer les 2 services, ou au moins donner
# un compte >= 2), tue le process.
cas_union_distinctes() {
    local port="${OWL_SERVE_TEST_PORT_UNION:-18992}"
    local out; out="$(mktemp)"

    ( cd "$FIXT_ROOT/union_distinctes" && "$OWL" serve --port "$port" ) > "$out" 2>&1 &
    local pid=$!

    local deadline=$((SECONDS + 10))
    local state="timeout"
    while [ $SECONDS -lt $deadline ]; do
        if grep -qE "en écoute|en ecoute" "$out" 2>/dev/null; then
            state="monte"
            break
        fi
        if ! kill -0 "$pid" 2>/dev/null; then
            state="sorti"
            break
        fi
        sleep 0.2
    done

    local rc=1
    if [ "$state" = "monte" ]; then
        local corps_alpha corps_beta code_alpha code_beta
        code_alpha="$(curl -s -o /tmp/owl_union_alpha.$$ -w '%{http_code}' "http://127.0.0.1:$port/alpha" 2>/dev/null)"
        corps_alpha="$(cat /tmp/owl_union_alpha.$$ 2>/dev/null)"
        code_beta="$(curl -s -o /tmp/owl_union_beta.$$ -w '%{http_code}' "http://127.0.0.1:$port/beta" 2>/dev/null)"
        corps_beta="$(cat /tmp/owl_union_beta.$$ 2>/dev/null)"
        rm -f /tmp/owl_union_alpha.$$ /tmp/owl_union_beta.$$

        local contenu; contenu="$(cat "$out")"

        if kill -0 "$pid" 2>/dev/null; then
            kill "$pid" 2>/dev/null
            wait "$pid" 2>/dev/null
        fi

        local routes_ok=1
        if [ "$code_alpha" != "200" ] || [ "$corps_alpha" != "reponse-alpha" ]; then routes_ok=0; fi
        if [ "$code_beta" != "200" ] || [ "$corps_beta" != "reponse-beta" ]; then routes_ok=0; fi

        if [ "$routes_ok" -eq 0 ]; then
            echo "cli_serve_multi_service_union_routes: RESULT: RED (MAUVAISE raison -- /alpha=[$code_alpha,$corps_alpha] /beta=[$code_beta,$corps_beta], attendu 200+reponse-alpha et 200+reponse-beta)"
            echo "--- sortie ---"
            echo "$contenu"
            rc=1
        else
            echo "cli_serve_multi_service_union_routes: RESULT: GREEN (les 2 routes /alpha et /beta repondent 200 avec leur corps propre, union deja fonctionnelle)"
            rc=0
        fi

        # message de demarrage honnete : doit nommer les 2 services (ou
        # donner un compte >= 2), pas seulement le 1er.
        if echo "$contenu" | grep -qi "ServiceAlpha" && echo "$contenu" | grep -qi "ServiceBeta"; then
            echo "cli_serve_multi_service_union_message: RESULT: GREEN (le message de demarrage nomme les 2 services)"
        elif echo "$contenu" | grep -qE '[2-9][0-9]* service|service[s]?.*[2-9]'; then
            echo "cli_serve_multi_service_union_message: RESULT: GREEN (le message de demarrage donne un compte >= 2)"
        else
            echo "cli_serve_multi_service_union_message: RESULT: RED (bonne raison -- le message de demarrage ne nomme que le 1er service, trompeur des que 2+ services)"
            echo "--- message ---"
            echo "$contenu"
            rc=1
        fi
    else
        local contenu; contenu="$(cat "$out")"
        rm -f "$out"
        if kill -0 "$pid" 2>/dev/null; then
            kill "$pid" 2>/dev/null
            wait "$pid" 2>/dev/null
        fi
        echo "cli_serve_multi_service_union: RESULT: RED (MAUVAISE raison -- etat=$state, ni montage ni sortie propre detectes)"
        echo "--- sortie ---"
        echo "$contenu"
        rc=1
    fi

    rm -f "$out"
    return $rc
}

# --- Cas 3 : mono --------------------------------------------------------
# Non-regression : 1 service, 1 route servie, message inchange.
cas_mono() {
    local port="${OWL_SERVE_TEST_PORT_MONO:-18993}"
    local out; out="$(mktemp)"

    ( cd "$FIXT_ROOT/mono" && "$OWL" serve --port "$port" ) > "$out" 2>&1 &
    local pid=$!

    local deadline=$((SECONDS + 10))
    local state="timeout"
    while [ $SECONDS -lt $deadline ]; do
        if grep -qE "en écoute|en ecoute" "$out" 2>/dev/null; then
            state="monte"
            break
        fi
        if ! kill -0 "$pid" 2>/dev/null; then
            state="sorti"
            break
        fi
        sleep 0.2
    done

    local rc=1
    local corps="" code=""
    if [ "$state" = "monte" ]; then
        code="$(curl -s -o /tmp/owl_mono_alpha.$$ -w '%{http_code}' "http://127.0.0.1:$port/alpha" 2>/dev/null)"
        corps="$(cat /tmp/owl_mono_alpha.$$ 2>/dev/null)"
        rm -f /tmp/owl_mono_alpha.$$
    fi

    local contenu; contenu="$(cat "$out")"
    rm -f "$out"

    if kill -0 "$pid" 2>/dev/null; then
        kill "$pid" 2>/dev/null
        wait "$pid" 2>/dev/null
    fi

    if [ "$state" = "monte" ] && [ "$code" = "200" ] && [ "$corps" = "reponse-alpha" ]; then
        echo "cli_serve_multi_service_mono: RESULT: GREEN (mono-service inchange, /alpha repond 200+reponse-alpha)"
        rc=0
    else
        echo "cli_serve_multi_service_mono: RESULT: RED (MAUVAISE raison -- etat=$state code=$code corps=$corps)"
        echo "--- sortie ---"
        echo "$contenu"
        rc=1
    fi
    return $rc
}

# --- Cas 4 : disjoint_urlbase --------------------------------------------
# Controle positif : meme motif [id] sous 2 UrlBase disjointes (clients,
# commandes). Doit demarrer et servir les 2 routes sans faux-positif de
# collision.
cas_disjoint_urlbase() {
    local port="${OWL_SERVE_TEST_PORT_DISJOINT:-18994}"
    local out; out="$(mktemp)"

    ( cd "$FIXT_ROOT/disjoint_urlbase" && "$OWL" serve --port "$port" ) > "$out" 2>&1 &
    local pid=$!

    local deadline=$((SECONDS + 10))
    local state="timeout"
    while [ $SECONDS -lt $deadline ]; do
        if grep -qE "en écoute|en ecoute" "$out" 2>/dev/null; then
            state="monte"
            break
        fi
        if ! kill -0 "$pid" 2>/dev/null; then
            state="sorti"
            break
        fi
        sleep 0.2
    done

    local rc=1
    if [ "$state" = "monte" ]; then
        local corps_clients corps_commandes code_clients code_commandes
        code_clients="$(curl -s -o /tmp/owl_disjoint_clients.$$ -w '%{http_code}' "http://127.0.0.1:$port/clients/42" 2>/dev/null)"
        corps_clients="$(cat /tmp/owl_disjoint_clients.$$ 2>/dev/null)"
        code_commandes="$(curl -s -o /tmp/owl_disjoint_commandes.$$ -w '%{http_code}' "http://127.0.0.1:$port/commandes/7" 2>/dev/null)"
        corps_commandes="$(cat /tmp/owl_disjoint_commandes.$$ 2>/dev/null)"
        rm -f /tmp/owl_disjoint_clients.$$ /tmp/owl_disjoint_commandes.$$

        local contenu; contenu="$(cat "$out")"

        if kill -0 "$pid" 2>/dev/null; then
            kill "$pid" 2>/dev/null
            wait "$pid" 2>/dev/null
        fi

        if [ "$code_clients" = "200" ] && [ "$corps_clients" = "client-42" ] \
            && [ "$code_commandes" = "200" ] && [ "$corps_commandes" = "commande-7" ]; then
            echo "cli_serve_multi_service_disjoint_urlbase: RESULT: GREEN (les 2 routes [id] sous UrlBase disjointes repondent 200 sans collision)"
            rc=0
        else
            echo "cli_serve_multi_service_disjoint_urlbase: RESULT: RED (MAUVAISE raison -- /clients/42=[$code_clients,$corps_clients] /commandes/7=[$code_commandes,$corps_commandes], attendu 200+client-42 et 200+commande-7)"
            echo "--- sortie ---"
            echo "$contenu"
            rc=1
        fi
    else
        local contenu; contenu="$(cat "$out")"
        rm -f "$out"
        if kill -0 "$pid" 2>/dev/null; then
            kill "$pid" 2>/dev/null
            wait "$pid" 2>/dev/null
        fi
        echo "cli_serve_multi_service_disjoint_urlbase: RESULT: RED (MAUVAISE raison -- etat=$state, ni montage ni sortie propre detectes -- possible faux-positif de collision entre /clients/{id} et /commandes/{id})"
        echo "--- sortie ---"
        echo "$contenu"
        rc=1
    fi

    rm -f "$out"
    return $rc
}

cas_collision || FAIL=1
cas_union_distinctes || FAIL=1
cas_mono || FAIL=1
cas_disjoint_urlbase || FAIL=1

exit $FAIL
