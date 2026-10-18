#!/usr/bin/env bash
# SPDX-License-Identifier: AGPL-3.0-only WITH LicenseRef-OWL-Runtime-Exception
# Batterie de controles de la cible `all` du Makefile.
#
# Deux modes :
#   (defaut)      arret au premier echec, exit != 0. Comportement historique,
#                 celui sur lequel s'appuient les hooks et la boucle de dev.
#   --keep-going  execute TOUS les controles independants et rend un
#                 recapitulatif final. Exit != 0 s'il reste un rouge.
#                 Equivalent : OWL_KEEP_GOING=1 dans l'environnement.
#
# Options :
#   --only=id1,id2  ne joue que ces controles ; les autres sont rapportes
#                   FILTRE (non mesures), jamais verts. Sert a rejouer un
#                   rouge seul avant de conclure : un build concurrent
#                   produit regulierement de faux rouges.
#   --skip=id1,id2  filtre inverse : joue tout SAUF ces controles. Sert a la
#                   boucle de dev (`make quick`), qui ecarte les deux
#                   controles longs. Inverse volontaire d'--only : un
#                   controle ajoute demain entre d'office dans la boucle
#                   rapide, au lieu d'y etre oublie en silence.
#   --list          liste les identifiants de controle et leur besoin.
#
# Pourquoi : en fail-fast, un rouge en cache 21 autres. Un rouge
# etranger a un chantier suffisait a rendre toute campagne de mesure
# ininterpretable, sans que rien ne l'indique — on a clos des tickets a tort
# sur cette base.
#
# Dependances entre controles : une seule est reelle, le build. Tout le reste
# est read-only (soit sur les sources, soit sur le binaire fraichement
# construit) et n'ecrit que dans build/. Les controles source-only tournent
# donc MEME si le build echoue ; les autres sont rapportes NON EXECUTE —
# jamais verts. C'est la difference entre un rapport et un piege.

set -u

BUILDDIR="${OWL_BUILDDIR:-build}"
CMAKE_BIN="${OWL_CMAKE:-cmake}"
LOGDIR="$BUILDDIR/logs"
OWL="./$BUILDDIR/owl.exe"

KEEP_GOING=0
ONLY=""
SKIP=""
[ "${OWL_KEEP_GOING:-0}" = "1" ] && KEEP_GOING=1
for arg in "$@"; do
    case "$arg" in
        --keep-going|-k) KEEP_GOING=1 ;;
        --fail-fast)     KEEP_GOING=0 ;;
        --only=*)        ONLY=",${arg#--only=}," ;;
        --skip=*)        SKIP=",${arg#--skip=}," ;;
        --list)          ONLY="__list__" ;;
        *) echo "usage: $0 [--keep-going|--fail-fast] [--only=id1,id2] [--skip=id1,id2] [--list]" >&2; exit 2 ;;
    esac
done

# ----------------------------------------------------------
# Table des controles.
#
# reg <id> <besoin> <soft> <log> <banniere> <indice> <commande>
#
#   besoin : none = source-only (survit a un build casse)
#            bin  = requiert build/owl.exe
#            lsp  = requiert build/owl-lsp-test.exe (optionnel -> SKIP)
#            dap  = requiert build/owl-dap-test.exe (optionnel -> SKIP)
#   soft   : 1 = avertissement non bloquant (n'empeche pas le vert global)
# ----------------------------------------------------------
# `diff` n'est pas fiable sur ce depot (fins de ligne) : comparaison binaire.
check_snapshot() {
    local tmp="$BUILDDIR/registry_snapshot.tmp.json" rc=0
    "$OWL" --vscode-data > "$tmp" 2>/dev/null || rc=1
    cmp -s tools/registry_snapshot.json "$tmp" || rc=1
    rm -f "$tmp"
    return $rc
}

CHK_ID=(); CHK_NEED=(); CHK_SOFT=(); CHK_LOG=(); CHK_BAN=(); CHK_HINT=(); CHK_CMD=()
reg() {
    CHK_ID+=("$1"); CHK_NEED+=("$2"); CHK_SOFT+=("$3"); CHK_LOG+=("$4")
    CHK_BAN+=("$5"); CHK_HINT+=("$6"); CHK_CMD+=("$7")
}

# EN TETE, et source-only, pour deux raisons.
#
# 1. Un hook casse ne casse pas un test : il casse tous les agents a la fois,
#    avec des symptomes qui ne pointent jamais vers lui ( : un defaut
#    de hook chasse des semaines comme un segfault reseau). Son verdict ne doit
#    donc pas etre gate sur un build vert — d'ou `none`.
# 2. Le dernier cas du fichier n'est pas un dry-run : il lance de vrais process
#    owl* depuis build/ et verifie que le hook tue ceux de build/ SEULEMENT.
#    Ici c'est inoffensif : la batterie est lancee par un make, donc le hook a
#    deja tue ces process une seconde plus tot — le cas ne fait que repeter un
#    geste deja pose. Place plus loin, il tuerait le owl-lsp.exe que VSCode a
#    respawn pendant les minutes de build. Les controles sont sequentiels : il
#    ne peut jamais tuer un binaire qu'un autre controle utilise.
reg hook-tests none 0 hook-tests.log \
    "TESTS DES HOOKS echec :" \
    "Un hook de .claude/hooks/ a derive de son contrat (declenchement sur une commande de build, non-declenchement sur un 'make' anodin, attente sur build concurrent, ou cible du kill limitee a build/ de ce depot). Corriger le hook, pas le test : un faux positif de hook fait desarmer le hook, et on repart pour des semaines de faux diagnostics. Le dernier cas manipule de vrais process — s'il est le seul rouge, le rejouer seul avant de conclure." \
    'bash .claude/hooks/tests/stop_owl_before_make_cases.sh'

# Les six autres hooks. Un `reg` par fichier de cas plutot qu'un
# seul bloc : l'indice de remediation n'a rien de commun d'un hook a l'autre, et
# ca donne une cible `--only` par famille. Meme raison que ci-dessus pour
# `none` + cette position : le verdict d'un hook ne doit pas dependre d'un build
# vert. Aucun de ces fichiers ne tue de process ni ne touche ce depot : ils
# n'alimentent les hooks qu'en payloads, et les trois hooks qui interrogent git
# le font depuis un depot jetable monte par le fichier de cas.
INDICE_HOOK="Corriger le hook, pas le test. Les DEUX cotes comptent : un hook qui bloque une commande legitime paralyse tous les agents et finit desarme, ce qui nous ramene pire qu'avant. Sonde de mutation pour verifier qu'un cas mord encore : casser le hook a la main, relancer le fichier de cas, restaurer, verifier avec cmp que le hook est bien revenu a l'identique."

reg hook-tests-add none 0 hook-tests-add.log \
    "TESTS DU HOOK check_git_add echec :" \
    "Le garde contre le stage global a derive. C'est lui qui empeche un \`git add -A\` d'embarquer le travail non commite d'un agent voisin — une perte muette, decouverte bien plus tard, sans lien apparent avec la commande fautive. $INDICE_HOOK" \
    'bash .claude/hooks/tests/check_git_add_cases.sh'

reg hook-tests-destructive none 0 hook-tests-destructive.log \
    "TESTS DU HOOK check_destructive_git echec :" \
    "Le garde contre stash / checkout / restore / reset --hard / clean -f a derive. Meme enjeu que le precedent, en pire : ces commandes effacent le travail non commite des autres agents sans laisser de trace. Verifier aussi la voie d'echappement OWL_ALLOW_DESTRUCTIVE=1 : personne ne s'en sert au quotidien, donc personne ne remarque qu'elle a cesse de fonctionner. $INDICE_HOOK" \
    'bash .claude/hooks/tests/check_destructive_git_cases.sh'

reg hook-tests-diff none 0 hook-tests-diff.log \
    "TESTS DU HOOK check_bare_diff echec :" \
    "Le garde contre le \`diff\` nu a derive. Il n'y a que la forme SANS drapeau a bloquer : un drapeau suffit a rendre l'outil fiable, et le mot 'diff' est partout ailleurs (git diff, --diff, un chemin, un message de commit). $INDICE_HOOK" \
    'bash .claude/hooks/tests/check_bare_diff_cases.sh'

reg hook-tests-commit none 0 hook-tests-commit.log \
    "TESTS DES HOOKS DE COMMIT echec :" \
    "Un des trois controles joues avant \`git commit\` a derive (perimetre de commit, collision de numero de ticket, sentinelles stress-GC). Attention aux deux echappatoires de check_stressgc_sentinels : \`--no-verify\`, et build/owl.exe absent qui doit rester un WARN NON bloquant — une inversion la rend tout commit sur src/ impossible juste apres un \`make clean\`. $INDICE_HOOK" \
    'bash .claude/hooks/tests/check_commit_hooks_cases.sh'

reg hook-tests-adr none 0 hook-tests-adr.log \
    "TESTS DU HOOK check_adr_unique echec :" \
    "Le garde contre la collision de numero d'ADR a derive. Ce qu'il protege est une reference STABLE : « cf. ADR_0140 » doit designer un seul document pour toujours, et une collision ne se voit pas — elle a dormi 12 jours avant d'etre reperee en lisant le README. Verifier les deux versants, surtout celui qui laisse passer : un garde qui bloque une renumerotation ou un ADR au bon numero sera desarme dans l'heure. $INDICE_HOOK" \
    'bash .claude/hooks/tests/check_adr_unique_cases.sh'

reg hook-tests-ticket-unique none 0 hook-tests-ticket-unique.log \
    "TESTS DU HOOK check_ticket_unique echec :" \
    "Le garde contre la collision de numero de ticket a derive. Piege propre a ce garde (fix/1622) : feat/fix/refactor/chore/docs PARTAGENT une sequence de numeros — la detection doit se faire ACROSS types, sinon feat_1616 et fix_1616 coexistent (angle mort de la version par-type). Le prefixe test_ (scenarios batch) garde en revanche son espace separe. Verifier surtout le cas cross-type et le cas test_ separe. $INDICE_HOOK" \
    'bash .claude/hooks/tests/check_ticket_unique_cases.sh'

reg hook-tests-dispatch none 0 hook-tests-dispatch.log \
    "TESTS DU DISPATCHER pretooluse_bash echec :" \
    "Une branche de routage du dispatcher a derive. Risque propre a ce fichier : une branche perdue DESARME SILENCIEUSEMENT le controle qu'elle dessert — rien ne casse, rien ne parle, le garde ne repond plus. Chaque cas verifie une branche par le verdict du controle en aval. $INDICE_HOOK" \
    'bash .claude/hooks/tests/pretooluse_bash_cases.sh'

reg hook-tests-notes-builder none 0 hook-tests-notes-builder.log \
    "TESTS DU HOOK check_notes_builder echec :" \
    "Le garde qui refuse de clore un ticket 'mode: large' aux Notes builder vides a derive. Ce qu'il protege est la memoire d'un chantier au moment ou le reviewer en a besoin (constat feat_1789 + feat_1792). Verifier surtout le versant qui LAISSE PASSER : un garde qui bloque une cloture legitime (ticket rapide, Notes deja remplies, commande qui n'est pas un git mv vers 99_done) sera desarme dans l'heure. $INDICE_HOOK" \
    'bash .claude/hooks/tests/check_notes_builder_cases.sh'

reg build build 0 build.log \
    "BUILD echec" \
    "Compilation cassee — rien d'aval n'est interpretable." \
    '"$CMAKE_BIN" --build "$BUILDDIR"'

reg registry bin 0 registry-test.log \
    "REGISTRY SELF-TEST violations detectees apres ce build :" \
    "Build OK mais le registre est inconsistant — corriger avant commit." \
    '"$OWL" --self-test-registry'

reg llama-smoke bin 0 llama-smoke.log \
    "LLAMA/GGML SMOKE echec apres ce build :" \
    "Le vendoring llama.cpp/ggml est casse au RUNTIME. Cas principal : le backend CPU ggml n'est plus enregistre (ggml_backend_dev_count()==0) — --whole-archive sur libggml-cpu.a n'a pas survecu a --gc-sections (piege #8166). Verifier le bloc owl_llama de CMakeLists.txt et scripts/build_llama_zig_native.sh. Sans GGUF fourni (OWL_LLAMA_TEST_GGUF), l'inference complete est SKIP — seul l'enregistrement backend + le loader GGUF sont exerces." \
    './"$BUILDDIR"/llamacpp_smoketest.exe'

reg aws-sigv4-smoke bin 0 aws-sigv4-smoke.log \
    "AWS SIGV4 SMOKE echec apres ce build :" \
    "Le coeur de signature AWS Signature V4 (src/stdlib_aws.c) diverge du vecteur officiel AWS. Le smoke test rejoue l'exemple canonique documente par AWS (GET iam / us-east-1) et compare AU BYTE : hash de la canonical request, signature finale, header Authorization complet. Un rouge = soit la construction de la canonical request / du string-to-sign a derive, soit la cascade de derivation de cle HMAC binaire (kDate->kRegion->kService->kSigning) ne se fait plus en binaire. Verifier stdlib_aws.c avant tout." \
    './"$BUILDDIR"/aws_sigv4_smoketest.exe'

reg libxls-smoke bin 0 libxls-smoke.log \
    "LIBXLS SMOKE echec apres ce build :" \
    "Le vendoring libxls 1.6.3 (lecture .xls binaire BIFF8/OLE2) est casse au RUNTIME. Le smoke test lit deux .xls reels (vendor/libxls/testdata/) : une cellule texte + une numerique (mtcars), et surtout les octets UTF-8 EXACTS d'une chaine UTF-16 a diacritiques (vietnamien). Un rouge sur le vietnamien = la conversion UTF-16 -> UTF-8 SANS iconv a derive (locale UTF-8 forcee de vendor/libxls/src/locale.c : _create_locale('.65001') Windows / newlocale 'C.UTF-8' Linux). NE PAS 'reparer' en ajoutant iconv (dependance externe non vendoree, cascade refusee). Verifier vendor/libxls/src/config.h (HAVE_ICONV NON defini) et le bloc owl_libxls de CMakeLists.txt." \
    './"$BUILDDIR"/libxls_smoketest.exe'

reg libssh2-smoke bin 0 libssh2-smoke.log \
    "LIBSSH2 SMOKE echec apres ce build :" \
    "Le vendoring libssh2 (client SSH-2/SFTP, backend mbedTLS, enabler du Lot D FTP sftp://) est casse au RUNTIME. Le smoke test amorce le socle SSH : libssh2_init + session, garde-fou de version (jamais < 1.11.1, le dernier tag VULNERABLE : le pin doit rester un commit master post-1.11.1 patche CVE-2026-55200/55199/CVE-2025-15661), et surtout la preuve que le backend mbedTLS a ENREGISTRE des algos (listes KEX/HOSTKEY non vides, curve25519/ecdh + ecdsa/rsa-sha2). Un rouge sur les listes vides = le backend crypto ne s'accroche plus (verifier LIBSSH2_MBEDTLS dans le bloc owl_libssh2 de CMakeLists.txt et que owl_mbedtls_config.h garde ECDH_C/ECDSA_C/SECP256-384-521/CTR_DRBG_C, sinon mbedtls.h leve un #error). NE PAS 'reparer' en bumpant vers un tag : aucun tag ne contient encore les fixes CVE." \
    './"$BUILDDIR"/libssh2_smoketest.exe'

reg encoding bin 0 encoding-test.log \
    "ENCODAGE SOURCES violations detectees apres ce build :" \
    "Un fichier source viole l'encodage (UTF-8 invalide / BOM / mojibake C3 83) — corriger avant commit." \
    '"$OWL" --self-test-encoding'

reg lsp-tests lsp 0 lsp-test.log \
    "LSP TESTS violations detectees apres ce build :" \
    "Build OK mais l'autocomplete LSP est casse — corriger avant commit." \
    './"$BUILDDIR"/owl-lsp-test.exe'

reg dap-tests dap 0 dap-test.log \
    "DAP TESTS echec apres ce build :" \
    "Build OK mais le Debug Adapter (debug VSCode) est casse — corriger avant commit." \
    'OWL_DAP_EXE=./"$BUILDDIR"/owl-dap.exe OWL_EXE="$OWL" ./"$BUILDDIR"/owl-dap-test.exe'

reg compile-error bin 0 compile-error.log \
    "COMPILE-ERROR SNAPSHOT FIXTURES echec apres ce build :" \
    "Build OK mais un message d'erreur compile-time a derive de son snapshot — verifier et mettre a jour tests/compile-error/*.expected.json." \
    '"$OWL" --self-test-compile-error'

reg doc-phantom-lint bin 0 doc-phantom-lint.log \
    "DOC PHANTOM LINT (calibration) echec apres ce build :" \
    "Le lint anti-natives-fantomes diverge de son oracle. Le detecteur partage (Regle 63 du registry self-test = surface production) est calibre sur tests/doc-lint-fixtures/ (oracle EXPECTED.json). Un rouge = soit le detecteur flag un token legitime des fixtures ignore/ (faux positif : propriete .., type, SQL, config, id demo, bloc de code fence, native reelle), soit il rate un fantome des fixtures catch/. Les 3 heuristiques et la matrice de calibrage sont dans tests/doc-lint-fixtures/CALIBRATION.md. La Regle 63 sur le corpus reel docs/public_documentation/ est verifiee par --self-test-registry (surface production, meme detecteur)." \
    '"$OWL" --self-test-doc-phantom-lint'

reg ce-workdir-isolation bin 0 ce-workdir-isolation.log \
    "CONVENTION DU POINT : un harnais redevient visible pour le walk du depot." \
    "Ce controle garde la regle qui empeche les harnais de se detruire mutuellement : tout dossier ou fichier de travail pose sous build/ (ou a la racine) pendant qu'un harnais tourne porte un nom commencant par un POINT, donc invisible pour les walkers du depot. Sans elle, une campagne \`owl test\` concurrente charge ces fichiers comme compagnons et abandonne quand leur producteur les remplace — un abort transitoire qui ne pointe jamais vers sa cause. Un rouge sur A/A2/A3 nomme le producteur fautif : lui rendre son point initial, JAMAIS ajouter une exclusion walker par walker (c'est la dette que fix/1317 a soldee). Les noms attendus sont LUS dans la source du producteur : « base de travail introuvable » veut dire que l'extraction ne reconnait plus la variable, pas que la convention tient. Un rouge sur un CONTROLE (temoin sans point non decouvert) dit l'inverse : le walk ne descend plus dans build/, la sonde est devenue muette et c'est elle qu'il faut revoir. Un rouge sur B touche la retention des workdirs \`_KEPT\` (src/internal_tests/compile_error_test.c) : ce sont les traces post-mortem d'un self-test compile-error en echec, les perdre coute le diagnostic. Un rouge sur C seul est une course : le rejouer seul avant de conclure. ~30 s." \
    'bash scripts/test_ce_workdir_isolation.sh'

reg error-centralization none 0 error-centralization.log \
    "CENTRALISATION ERREURS violee : message d'erreur ad-hoc detecte." \
    "Utilise owl_emit_*_error (cf ADR_0020). Build refuse." \
    'bash scripts/check_error_centralization.sh src'

reg error-template-args none 0 error-template-args.log \
    "MARQUEUR DE GABARIT BRUT : un message d'erreur sortirait avec un '{marqueur}' litteral." \
    "Un gabarit d'owl_errors.def qui reclame {arg}, {expected}, {actual}, {path}... n'efface PAS le marqueur quand le site d'emission ne l'alimente pas : il le rend tel quel a l'utilisateur (\`HeureVersChaine: argument '{arg}' attendu ...\`). Le log donne fichier:ligne + les marqueurs orphelins. La correction est TOUJOURS au site d'emission (ajouter la paire \"cle\", valeur) — retirer le marqueur du gabarit ferait taire le symptome en appauvrissant un diagnostic correct. Le marqueur {native} n'est pas controle : il est lie d'office au parametre positionnel de owl_emit_native_error." \
    'bash scripts/check_error_template_args.sh src'

reg capacity-saturation none 0 capacity-saturation.log \
    "GARDE DE SATURATION MUET : une limite annoncee peut etre atteinte sans le dire." \
    "Un site qui sature doit passer par error_capacity_exceeded() — le seul chemin qui nomme la limite, donne sa valeur et renvoie vers 'owl explain'. Un \`return\` muet ne fait pas que taire le refus : il ETEINT le controle qui s'appuyait sur la table (une classe qui n'implementait que 512 des 513 methodes de son interface compilait au vert). Le log nomme le fichier et la ligne. Ce controle ne voit PAS les murs empruntes : c'est le role de limits-probe." \
    'bash scripts/check_capacity_saturation.sh src'

# Apres le build : la sonde compile de vrais sources avec le binaire courant.
reg limits-probe bin 0 limits-probe.log \
    "LIMITES ANNONCEES : le mur annonce n'est pas le mur reel." \
    "Pour chaque limite sondee, N-1 doit compiler et N+1 doit echouer en nommant CETTE limite. Un N-1 refuse signale un mur emprunte : une autre limite, ou une table dimensionnee sur le nom d'une autre, frappe avant — c'est le cas que le controle statique ne peut pas voir. Un N+1 accepte signale une saturation muette. ~40 s. Les sondes ecartees et leur raison sont listees en tete de tests/limits/run.sh." \
    'OWL_BIN="$OWL" bash tests/limits/run.sh'

reg lint-gc none 0 lint-gc.log \
    "LINT GC CANON violee : table_set direct hors canon." \
    "Utilise inst_field_set_* / obj_globals_set (cf ADR_0057+0058+0059). Build refuse." \
    'bash scripts/lint_gc_canon.sh'

# lint-color pointe tools/lint_color_reads.sh, outil interne NON publie
# (hors TOOLS_INCLUDE de l'export). Sur un clone public le script est absent :
# on saute l'enregistrement du check plutot que de casser la batterie. Repo
# prive = script present = check inchange.
if [ -f tools/lint_color_reads.sh ]; then
reg lint-color none 0 lint-color.log \
    "LINT COULEUR violee : lecture couleur alpha-aveugle (instance_get_int)." \
    "Utilise instance_get_color (bgr+alpha) ou marque /* color-int-ok */ (cf fix/1085). Build refuse." \
    'bash tools/lint_color_reads.sh'
fi

reg lint-enum none 0 lint-enum.log \
    "LINT ENUM violee : enum defini mais absent de son tableau de registration." \
    "Invisible pour --self-test-registry (qui itere le registre, pas le source) — corriger avant commit." \
    'bash scripts/lint_enum_registration.sh'

reg doc-type-fields bin 0 doc-type-fields.log \
    "DOC <-> TEMPLATE : la table de champs publiee ne decrit plus le type reel." \
    "Un champ documente mais absent du template est une promesse que le compilateur refuse (\`cnx..TimeoutCnx = 30\` -> « Membre inexistant ») ; un champ present mais non documente est indecouvrable. Les deux dorment des mois : le self-test registry itere le registre, pas la doc, et la suite n'ecrit jamais un champ qu'elle ignore. La source de verite est le template C, pas la doc — corriger la doc, sauf si c'est bien le champ qui manque. Portee DERIVEE (fix/1592) : toute table \`| Field FR | Field EN |\` de docs/public_documentation/ sous sa section \`### \`<type>\`\` est couverte d'office — aucune liste a tenir. Un message « aucune ligne de table trouvee » ou « table sans section englobante » dit que la sonde est devenue muette, pas qu'elle passe." \
    'OWL_BIN="$OWL" bash scripts/check_doc_type_fields.sh'

reg tests-layout none 0 tests-layout.log \
    "TAXONOMIE tests/ violee : un dossier top-level inattendu est apparu sous tests/." \
    "Les familles cli_/formatter_/translate_ sont regroupees sous tests/cli, tests/formatter, tests/translate ; les buckets top-level autorises sont figes dans une allowlist. Un nouveau dossier tests/<truc>/ (feature qui se cree son propre repertoire au lieu de se ranger) fait rougir ce verrou : le ranger sous un bucket existant, ou — si c'est un bucket legitime — l'ajouter a ALLOW dans scripts/check_tests_layout.sh avec justification (fix/2221)." \
    'bash scripts/check_tests_layout.sh'

reg cli-doc-lang bin 0 cli-doc-lang.log \
    "DIGUE LANGUE CLI DOC violee : fuite FR dans owl explain/list/examples --lang en." \
    "Tout label/header/desc FR non gate sur --lang en doit passer par dl()/type_to_lang (cf fix/1128). Build refuse." \
    'OWL_BIN="$OWL" bash tests/cli/doc_lang/run.sh'

reg cli-openapi bin 0 cli-openapi.log \
    "GENERATION OPENAPI violee : owl openapi ne produit pas un document OpenAPI 3 conforme au DSL." \
    "Le document OpenAPI 3 est DERIVE de l'AST ServiceREST/OperationHTTP (jamais une 2e source) : chemins UrlBase+Chemin (params -> {id}), methodes, parametres path/query (required derive de la presence d'un defaut sur le param), requestBody (multipart inclus), responses (Reponses + Problem Details application/problem+json + 500 implicite), securityScheme derive de Authentification. Expose via owl openapi <service> ET la route GET /openapi.json. Un rouge ici pointe le generateur src/http_openapi.{c,h} ou son cablage CLI/route, pas le compilateur (feat/1840)." \
    'OWL_BIN="$OWL" bash tests/cli/openapi/run.sh'

reg cli-serve bin 0 cli-serve.log \
    "OWL SERVE FRONTIERE PROJET violee : owl serve ne compile qu'un fichier isole au lieu de la frontiere projet." \
    "\`owl serve api.owl\` doit resoudre la frontiere projet (meme moteur que \`owl <fichier>\` / \`owl .\`) et compiler TOUS les fichiers voisins (compagnons defs+corps AVANT l'entree), pour qu'un ServiceREST reference un ETAT_X ou une procedure declaree dans un fichier voisin. Le harnais monte \`owl serve api.owl\` sur un projet {projet.owl, etat_facture.owl, api.owl} : avant le fix, la compilation echoue sur 'ETAT_Facture inconnue'. Un rouge ici pointe handle_serve_command (src/main.c), owl_serve_prepare_projet (src/owl_serve.c) ou la voie projet de l'executeur (src/http_exec.c / project_run_sources_no_teardown), pas le compilateur (fix/1982)." \
    'OWL_BIN="$PWD/$BUILDDIR/owl.exe" bash tests/cli/serve/run.sh'

reg cli-openapi-projet bin 0 cli-openapi-projet.log \
    "OWL OPENAPI FRONTIERE PROJET violee : owl openapi ne compile qu'un fichier isole au lieu de la frontiere projet." \
    "\`owl openapi api.owl\` doit resoudre la frontiere projet (meme moteur que \`owl serve\`) et compiler les fichiers voisins (compagnons defs+corps AVANT l'entree), pour qu'un ServiceREST reference un ETAT_X declare dans un voisin ; le document OpenAPI genere reste ENTRY-SPECIFIC (le service declare dans le fichier cible). Le harnais monte \`owl openapi api.owl\` sur {projet.owl, etat_facture.owl, api.owl} : avant le fix, la compilation echoue sur 'ETAT_Facture inconnue'. Un rouge ici pointe handle_openapi_command (src/main.c) ou owl_serve_compile_projet (src/owl_serve.c), pas le compilateur (fix/1990)." \
    'OWL_BIN="$PWD/$BUILDDIR/owl.exe" bash tests/cli/openapi_projet/run.sh'

reg cli-prescan-servicerest bin 0 cli-prescan-servicerest.log \
    "PARTICIPATION PRESCAN SERVICEREST violee : un fichier ServiceREST-only n'est pas reconnu comme compagnon en mode projet." \
    "\`prescan_after_est_is_dsl_block\` (src/prescan.c:477-491) doit reconnaitre \`ServiceREST\` au meme titre que Classe/Fenetre/Etat/Projet/Theme/CollectionDeProcedures : sinon \`project_companion_in_compile_scope\` (src/project_run.c) exclut de la compilation tout fichier qui ne declare RIEN d'autre qu'un ServiceREST sous un \`projet.owl\` (mode projet) -- une operation HTTP dupliquee dans ce service (erreur normalement fatale) reste alors muette sous \`owl .\`. Le harnais compare aussi deux non-regressions (ServiceREST-only valide, projet sans ServiceREST) qui doivent rester propres avant ET apres le fix. Un rouge sur le 1er cas pointe la whitelist de prescan.c, pas le compilateur (fix/1986)." \
    'OWL_BIN="$PWD/$BUILDDIR/owl.exe" bash tests/cli/prescan_servicerest/run.sh'

reg cli-serve-projet-i18n bin 0 cli-serve-projet-i18n.log \
    "OWL SERVE MODE PROJET FIDELE violee : owl serve ne boote pas l'i18n de projet.owl (contournement ADR_0203)." \
    "\`handle_serve_command\` (src/main.c) force \`projet.mode_projet = 0\` (mode script), donc \`project_run_ex\` (src/project_run.c) ne boote jamais l'i18n depuis \`projet.owl\` (\`owl_boot_project_i18n\` gate sur \`isProjectMode\`). Le harnais monte un ServiceREST (fichier ET dossier) sous un \`projet.owl\` declarant \`Langues = [langueAnglais]\` et interroge une route qui renvoie \`LangueSource()\` : attendu 'en-US' apres fix/1986 (vrai mode projet, ADR_0203 amende), 'fr-FR' (repli par defaut) aujourd'hui. Un rouge ici pointe handle_serve_command/owl_serve.c/http_exec.c, pas le compilateur (fix/1986)." \
    'OWL_BIN="$PWD/$BUILDDIR/owl.exe" bash tests/cli/serve_projet_i18n/run.sh'

reg cli-serve-multi-service bin 0 cli-serve-multi-service.log \
    "OWL SERVE COLLISION DE ROUTES violee : deux ServiceREST voisins declarant la meme route servent en silence (premier-gagne), et le message de demarrage ne nomme que le 1er service." \
    "\`owl_serve_prepare_projet\` (src/owl_serve.c) doit detecter, a travers TOUS les ServiceREST de la frontiere projet, deux OperationHTTP declarant le meme couple (methode, chemin resolu) et refuser de demarrer avec un message nommant le conflit (fail-loud, jamais de premier-declare-gagne silencieux) ; le message de demarrage doit aussi nommer TOUS les services servis (ou un compte), pas seulement \`owl_service_registry_at(0)\`. Le harnais monte 3 fixtures (collision/, union_distinctes/, mono/) : collision doit refuser de demarrer (aujourd'hui il demarre quand meme -> rouge), union_distinctes doit servir les 2 routes ET nommer les 2 services dans le message (aujourd'hui seul le 1er est nomme -> rouge sur le message), mono est une non-regression (vert). Recouvrement wildcard hors scope (fix/1971 parque), se limiter a la collision exacte. Un rouge ici pointe owl_serve_prepare_projet/compagnons_service_count (src/owl_serve.c), pas le compilateur (feat/1911)." \
    'OWL_BIN="$PWD/$BUILDDIR/owl.exe" bash tests/cli/serve_multi_service/run.sh'

reg cli-pdf-console-parity bin 0 cli-pdf-console-parity.log \
    "PARITE PDF CONSOLE/GUI violee : owl-cli --check (registre console, sans Qt) et owl.exe --check rendent des verdicts differents sur les natives PDF." \
    "owl-build compile le C dans un process CONSOLE (owl_runtime_console, sans OWL_HAS_GUI) : son registre doit annoncer les signatures des 42 natives Etats/PDF pour que le portail de compile recale/accepte un projet PDF EXACTEMENT comme l'exe GUI qu'il produit (parite du verdict). Les donnees de registre vivent dans src/stdlib_pdf_defs.c (Qt-free), enregistrees inconditionnellement par owl_stdlib.c ; les impls Qt (stdlib_pdf.c) sont injectees en GUI seulement via stdlib_pdf_impls_wire(). Le harnais compare owl.exe --check vs owl-cli.exe --check sur 4 fixtures (valid_fr, valid_en, arity, enum) : un rouge pointe le split stdlib_pdf_defs.c, le guard-relax impl=NULL d'owl_registry_register_module, ou l'ordre d'init owl_stdlib.c (fix/2224), pas le compilateur. SKIP gracieux si owl-cli.exe absent (build sans OWL_DEV_TOOLS)." \
    'if [ -x "$PWD/$BUILDDIR/owl-cli.exe" ]; then OWL_BIN="$PWD/$BUILDDIR/owl.exe" OWL_CLI_BIN="$PWD/$BUILDDIR/owl-cli.exe" bash tests/cli/pdf_console_parity/run.sh; else echo "owl-cli.exe absent -> SKIP (build OWL_DEV_TOOLS=ON pour exercer la parite)"; fi'

reg cli-ui-console-parity bin 0 cli-ui-console-parity.log \
    "PARITE UI CONSOLE/GUI violee : owl-cli --check (registre console, sans Qt) et owl.exe --check rendent des verdicts differents sur les natives UI." \
    "owl-build compile le C dans un process CONSOLE (owl_runtime_console, sans OWL_HAS_GUI) : son registre doit annoncer les signatures des 108 natives UI (+ leurs enums touche/fsel/modeTaille/recherche) pour que le portail de compile recale/accepte un projet UI EXACTEMENT comme l'exe GUI qu'il produit (parite du verdict). Les donnees de registre vivent dans src/stdlib_ui_defs.c (Qt-free), enregistrees inconditionnellement par owl_stdlib.c ; les impls Qt (stdlib_ui.c) sont injectees en GUI seulement via stdlib_ui_impls_wire(). Le harnais compare owl.exe --check vs owl-cli.exe --check sur 4 fixtures (valid_fr, valid_en, arity, enum) : un rouge pointe le split stdlib_ui_defs.c, la migration des enums UI en inconditionnel, ou l'ordre d'init owl_stdlib.c (fix/2226), pas le compilateur. SKIP gracieux si owl-cli.exe absent (build sans OWL_DEV_TOOLS)." \
    'if [ -x "$PWD/$BUILDDIR/owl-cli.exe" ]; then OWL_BIN="$PWD/$BUILDDIR/owl.exe" OWL_CLI_BIN="$PWD/$BUILDDIR/owl-cli.exe" bash tests/cli/ui_console_parity/run.sh; else echo "owl-cli.exe absent -> SKIP (build OWL_DEV_TOOLS=ON pour exercer la parite)"; fi'

reg real-to-str-locale none 0 real-to-str-locale.log \
    "LOCALE VIOLEE : owl_real_to_str fuit le separateur decimal de LC_NUMERIC." \
    "owl_real_to_str (src/value.c) doit toujours serialiser un reel avec un point decimal, quel que soit le LC_NUMERIC actif du processus — c'est la fonction canon utilisee par TOUTE serialisation reelle (JSON, OOXML, console...). Le harnais force une locale a separateur virgule (fr_FR/de_DE) DANS SON PROPRE PROCESSUS puis appelle la fonction extraite EN DIRECT depuis src/value.c (pas de copie). Si aucune locale virgule n'est disponible sur la machine, le check est un SKIP explicite (exit 0 logge), jamais un vert silencieux qui ne prouve rien (cf fix/1805)." \
    'bash tests/runtime_c/real_to_str_locale/run.sh'

reg translate-iteration bin 0 translate-iteration.log \
    "TRADUCTION DU MOT D'ITERATION violee : owl translate laisse du francais dans une sortie EN (ou l'inverse)." \
    "Les sept formes d'iteration (TOUT/TOUTE/TOUS/CHAQUE, EACH/EVERY/ALL) forment un groupe de synonymes a UN canon par langue : TOUT en FR, EACH en EN (fix/1531, ADR_0101 amende). Toutes restent acceptees en entree — le harnais le verifie aussi — mais la sortie de translate doit etre monolingue et canonique. Le collapse ne se declenche qu'au changement de langue (une cible egale a la langue du fichier laisse la forme ecrite, pour ne pas casser l'accord grammatical FR). Le pairing vit dans fmt_keywords[] (colonne partner, formatter.c) : un rouge ici pointe cette table ou fmt_kw_emit_form(), pas le compilateur." \
    'OWL_BIN="$OWL" bash tests/translate/iteration/run.sh'

reg translate-property-families bin 0 translate-property-families.log \
    "FAMILLES DE TRADUCTION violees : owl translate laisse du francais dans une sortie EN (ou l'inverse) sur une des familles surveillees." \
    "Quatre familles echappent au chemin mot-cle/native standard et se traduisent silencieusement de travers : cles DSL du bloc \`est un Projet\` (Langues<->Languages, FenetreDemarrage<->StartupWindow), acces propriete en cascade \`..Prop\` (..Valeur<->..Value, ..Libelle<->..Caption), methodes fluentes accentuees du Tenseur (.Retropropage<->.Backpropagate, .EntropieCroisee<->.CrossEntropy, .DescenteGradient<->.GradientDescent) et l'elision \`d'\` devant voyelle (\`tableau d'entiers\`<->\`array of integer\`). Le harnais verrouille chaque face separement (sens casse ET sens deja correct comme garde de non-regression), plus un round-trip composite. L'elision EN->FR est translate-only : le formatter n'est PAS touche (il tolere toujours \`de entiers\`). Un rouge ici pointe le moteur translate (owl_translate) et fmt_keywords[]/fmt_coll_elem_fr_starts_vowel dans formatter.c, pas le compilateur." \
    'OWL_BIN="$OWL" bash tests/translate/property_families/run.sh'

reg formatter-accented-keywords bin 0 formatter-accented-keywords.log \
    "MOTS-CLES ACCENTUES DU FORMATTER violes : À / _À_ / ÉLÉMENT / SÉPARÉE ne sont plus canonises, ou le PAR d'iteration fuit." \
    "Ces quatre formes ont une entree dans fmt_keywords[] (formatter.c) qui restaure accent + casse ; sans elle, la forme survit par absence de traitement (le meme angle mort que fix/1349 sur État). Le PAR d'iteration (POUR TOUT ... SÉPARÉE PAR) passe en MAJUSCULE par un cas special seenPourTout symetrique du DE (fix/0660), tandis que le par de dimensions/tri reste minuscule. Un rouge ici pointe fmt_keywords[] ou le cas special PAR, pas le compilateur." \
    'OWL_BIN="$OWL" bash tests/formatter/accented_keywords/run.sh'

reg formatter-string-prefix bin 0 formatter-string-prefix.log \
    "PREFIXE CHAINE INTERPOLEE : owl fmt corrompt la forme prefixe-pourcent-guillemet en inserant un espace parasite (pourcent colle au guillemet -> pourcent espace guillemet)." \
    "Le prefixe pourcent colle au guillemet ouvre une chaine interpolee (scanner scan_string_interp : au pourcent, si le caractere suivant est un guillemet, c'est une chaine, pas l'operateur modulo). Un espace injecte entre le pourcent et le guillemet fait redevenir le pourcent l'operateur modulo -> code valide corrompu au save (violation invariant 4, le formatter est gatekeeper canonique). Le harnais verrouille le round-trip byte-identique de la chaine prefixee et de sa variante interpolee, plus la non-regression de l'operateur modulo a pourcent b et des 3 autres formes de chaine. Un rouge ici pointe l'emission du token pourcent dans formatter.c, pas le compilateur." \
    'OWL_BIN="$OWL" bash tests/formatter/string_prefix/run.sh'

reg formatter-type-name-en-passthrough bin 0 formatter-type-name-en-passthrough.log \
    "PASSTHROUGH NOM DE TYPE EN : owl fmt FR-ifie le nom d'un type compose ecrit en forme EN (ou inversement) au lieu de le laisser verbatim." \
    "Le chemin type du formatter (formatter.c) ne matche que l'ASCII -> canon FR ; il n'a aucun chemin de preservation EN pour les types purs, donc un token de type en forme EN est laisse verbatim (passthrough). Depuis que la resolution de type reconnait nameEN (fix/2253), une declaration 'x est un <TypeEN>' compile : le formatter ne doit surtout pas la reecrire vers la forme FR au save (violation invariant 3 bilingue + invariant 4 gatekeeper : un code EN se ferait FR-ifier sous l'utilisateur). Le harnais verrouille le passthrough EN (resolue ou non), la non-regression de la forme FR canon, et l'idempotence. Un rouge ici pointe le chemin type de formatter.c, pas le compilateur." \
    'OWL_BIN="$OWL" bash tests/formatter/type_name_en_passthrough/run.sh'

reg ui-min-floors bin 0 ui-min-floors.log \
    "PLANCHERS DE TAILLE : ce qui est publie a l'editeur n'est plus ce que le moteur applique." \
    "La section 'ui_min_floors' de owl --vscode-data donne, par famille de champ, la taille en dessous de laquelle un layout ne descend jamais. L'apercu du designer s'en sert pour dessiner la taille REELLE d'une fenetre — celle qui s'ouvrira, pas celle qui est ecrite. Un plancher publie qui diverge de celui applique redonne exactement le defaut que ce chantier ferme : un nombre qui ment sans que rien ne le dise. Le log nomme la famille et l'axe fautifs." \
    'OWL_BIN="$OWL" bash tests/ui_min_floors/run.sh'

reg compiler-tests bin 0 compiler-tests.log \
    "COMPILER SNAPSHOT TESTS echec apres ce build :" \
    "catalog / fmt-context-aware ont derive — verifier tests/compiler/*/*.expected.*" \
    'bash scripts/run_compiler_tests.sh "$OWL"'

reg segfault-unwind bin 0 segfault-unwind-test.log \
    "SEGFAULT UNWIND NON-REGRESSION echec apres ce build :" \
    "fix/0236 regression — erreur runtime levee depuis native re-entrante segfault au teardown." \
    'bash scripts/run_segfault_unwind_test.sh "$OWL"'

reg stdout-survives-crash bin 0 stdout-survives-crash-test.log \
    "SORTIE PERDUE SUR ARRET BRUTAL echec apres ce build :" \
    "fix/1444 regression — stdout redirige ne survit plus a un crash (retour au line-buffering, alias du full-buffering hors terminal) : le dernier test affiche avant le plantage devient invisible. Ou fix/1467 regression — le dump de pile (owl_crash.txt dans %TEMP%) n'a plus d'en-tete ou plus une seule frame : fichier rouvert en mode texte, retamponne, ou symbolisation dbghelp repassee avant le deroulement brut." \
    'bash scripts/run_stdout_survives_crash_test.sh "$OWL"'

reg reentrant-terminal-diag bin 0 reentrant-terminal-diag-test.log \
    "DOUBLE DIAGNOSTIC / RATTRAPAGE INCOMPLET (cas TERMINAL reentrant) echec apres ce build :" \
    "fix/1716 regression — une erreur runtime USER levee dans un callback reentrant (comparateur TableauTrie ttFonction) SANS aucun QUAND EXCEPTION doit imprimer le diagnostic « Erreur d'execution » EXACTEMENT 1x (pas 2x : frontiere reentrante + top-level) et arreter la native au 1er echec (le comparateur ne doit etre rappele qu'1x, pas relance apres avoir deja plante). L'exit code non-nul doit rester preserve : ce n'est pas un rattrapage silencieux, juste un diagnostic qui ne doit pas fuir en double. Amendement d'ADR_0175 (pendant « cas terminal » du contrat pose par fix/1655, qui ne couvrait que le cas AVEC handler ancetre)." \
    'bash scripts/run_reentrant_terminal_diag_test.sh "$OWL"'

reg fmt-stability bin 0 fmt-stability-test.log \
    "FMT-STABILITY SELF-TESTS echec apres ce build :" \
    "Le formatter veut modifier un .expected.owl — regression indent/casse, verifier tests/compiler/fmt-stability/." \
    '"$OWL" --self-test-fmt-stability'

reg fmt-stability-catalog lsp 0 fmt-stability-catalog.log \
    "FMT-STABILITY PARITE CATALOGUE echec : owl-lsp formate differemment d'owl.exe !" \
    "Le backend catalogue (owl-lsp) diverge du backend registry (owl.exe) — verifier dump_lsp_catalog / formatter_canon_catalog.c." \
    './"$BUILDDIR"/owl-lsp-test.exe --self-test-fmt-stability'

reg fmt-contract bin 0 fmt-contract-test.log \
    "FMT-CONTRACT echec : le formatter ne tient plus son contrat !" \
    "Soit il n'est plus idempotent (le fichier derive a chaque sauvegarde), soit il CASSE LA COMPILATION d'un fichier qui compilait avant formatage. Le formatter tourne au save dans l'editeur : le second cas detruit le fichier de l'utilisateur pendant qu'il travaille. Le log nomme le fichier fautif." \
    '"$OWL" --self-test-fmt-contract'

reg fmt-canon bin 0 fmt-canon-test.log \
    "FMT-CANON echec : le formatter et la doc n'ont pas le meme canon !" \
    "Un mot est affiche par 'owl explain'/le hover sous une forme que 'owl fmt' reecrit autrement (accents ou casse). Source de verite : fmt_keywords[] (formatter.c) — c'est elle qui decide de ce qui atterrit dans le fichier. Corriger token_type_name (scanner.c) / canon_display (owl_keyword_catalog.c) / CONTROL_KINDS, sauf si c'est le formatter qui a tort." \
    '"$OWL" --self-test-fmt-canon'

reg errors bin 0 errors-test.log \
    "REGISTRY ERREURS SELF-TESTS echecs detectes apres ce build :" \
    "Build OK mais le registre d'erreurs (codes, alias numerique EnnnnN, rendu bilingue des templates, helpers compile/runtime/native-aware) a derive — corriger src/owl_errors.* avant commit." \
    '"$OWL" --self-test-errors'

reg signature bin 0 signature-test.log \
    "SIGNATURE SNAPSHOT FIXTURES echec apres ce build :" \
    "Un diagnostic de signature/arite a derive de son snapshot — verifier tests/compiler/signature/*.expected.json." \
    '"$OWL" --self-test-signature'

reg net bin 0 net-test.log \
    "INTERNAL NET/SQL TESTS echecs detectes apres ce build :" \
    "Build OK mais les briques reseau/SQL internes sont cassees — corriger avant commit." \
    '"$OWL" --self-test-net'

reg http-dsl-service bin 0 http-dsl-service-test.log \
    "DSL SERVICE (bout-en-bout) echecs detectes apres ce build :" \
    "Build OK mais le runtime du DSL de service (compilation surRequete + registre + invocation) est casse — corriger avant commit." \
    '"$OWL" --self-test-http-dsl-service'

reg http-service-flux bin 0 http-service-flux-test.log \
    "DSL SERVICE FLUX (3e mode d'execution, streaming E2E) echecs detectes apres ce build :" \
    "Build OK mais le 3e mode d'execution flux du DSL de service (execute_service_job : ouverture paresseuse au 1er FluxEcrit, statut fige, anti-croisement, anti-famine) est casse — corriger avant commit." \
    '"$OWL" --self-test-http-service-flux'

reg http-serve bin 0 http-serve-test.log \
    "OWL SERVE (montage compile->executeur->listener->boucle) echecs detectes apres ce build :" \
    "Rouge attendu tant que src/owl_serve.{c,h} n'existe pas et que src/internal_tests/http_serve_test.c n'est pas cable dans CMakeLists.txt + le dispatch --self-test-http-serve de main.c (feat/1900). Une fois cable : port reel binde + vrai client socket doivent servir GET /ping (200) et GET /inconnu (404), respecter le defaut 127.0.0.1, et refuser proprement un fichier sans ServiceREST ou un port deja pris." \
    '"$OWL" --self-test-http-serve'

reg cli bin 0 cli-test.log \
    "CLI DOC SELF-TESTS echecs detectes apres ce build :" \
    "Build OK mais les sous-commandes doc CLI sont cassees — corriger avant commit." \
    '"$OWL" --self-test-cli'

reg cli-ops bin 0 cli-ops-test.log \
    "CLI OPS SELF-TESTS echecs detectes apres ce build :" \
    "Build OK mais le dispatch des commandes CLI ops (fmt/check/list) est casse — corriger avant commit." \
    '"$OWL" --self-test-cli-ops'

reg translate bin 0 translate-test.log \
    "TRANSLATE SELF-TESTS echecs detectes apres ce build :" \
    "Build OK mais le moteur de traduction FR<->EN (owl translate) est casse — corriger avant commit." \
    '"$OWL" --self-test-translate'

reg similarity bin 0 similarity-test.log \
    "SIMILARITE TEXTE SELF-TESTS echecs detectes apres ce build :" \
    "Build OK mais la famille SimilariteTexte est cassee — corriger avant commit." \
    '"$OWL" --self-test-similarity'

reg temporal-parity bin 0 temporal-parity-test.log \
    "TEMPORAL PARITY SELF-TESTS echecs detectes apres ce build :" \
    "Build OK mais l'invariant single-parser temporal (Date/Heure/DateHeure/Duree) a derive — corriger src/stdlib_date_parsers.c avant commit." \
    '"$OWL" --self-test-temporal-parity'

reg mcp bin 0 mcp-test.log \
    "MCP SELF-TESTS echecs detectes apres ce build :" \
    "Build OK mais le serveur MCP est casse — corriger avant commit." \
    '"$OWL" --self-test-mcp'

reg multi-instance bin 0 multi-instance-test.log \
    "MULTI-INSTANCE SELF-TESTS echecs detectes apres ce build :" \
    "Build OK mais les structures multi-instance (ObjWindowInstance/stack VM/GC) sont cassees — corriger avant commit." \
    '"$OWL" --self-test-multi-instance'

reg vm-dispatch bin 0 vm-dispatch-test.log \
    "VM DISPATCH SELF-TESTS echecs detectes apres ce build :" \
    "Build OK mais le dispatch VM / l'appel reentrant (fix/0658) a derive — corriger src/vm.c avant commit." \
    '"$OWL" --self-test-vm-dispatch'

reg compile-split bin 0 compile-split-test.log \
    "COMPILE SPLIT SELF-TESTS echecs detectes apres ce build :" \
    "Build OK mais le decoupage deux phases (compile_in_project_split, fix/0941/ADR_0122) a derive — corriger src/compiler.c avant commit." \
    '"$OWL" --self-test-compile-split'

reg format-projet-leak bin 0 format-projet-leak-test.log \
    "FORMAT PROJET LEAK SELF-TESTS echecs detectes apres ce build :" \
    "Build OK mais l'etat format projet compile-time (FormatDate/Heure/Duree/Nombre/Monetaire) fuit entre projets d'un meme process : soit compiler_projet_boot_langue_reset() n'est plus exhaustif, soit une frontiere projet (test runner src/testing.c, serveur check LSP src/main.c) ne l'appelle plus (fix/2157). Corriger avant commit." \
    '"$OWL" --self-test-format-projet-leak'

reg const-index bin 0 const-index-test.log \
    "CONST INDEX SELF-TESTS echecs detectes apres ce build :" \
    "Build OK mais l'encodage 16 bits de l'index de constante (fix/1504) a derive — la table chunk_opcode_shape et les sites d'emission ne s'accordent plus. Corriger src/chunk.c / src/compiler.c avant commit." \
    '"$OWL" --self-test-const-index'

reg go-local bin 0 go-local-test.log \
    "GO LOCAL SELF-TESTS echecs detectes apres ce build :" \
    "Build OK mais la detection fichier-fenetre pur (feat/0488) a derive — corriger src/main.c avant commit." \
    '"$OWL" --self-test-go-local'

reg repl-completion bin 0 repl-completion-test.log \
    "REPL COMPLETION SELF-TESTS echecs detectes apres ce build :" \
    "Build OK mais la complétion Tab du REPL (fix/1785) a derive : elle doit refleter le registre vivant (natives/types/consts/enums, formes FR+ASCII+EN, natives internes masquees) et non une liste figee — corriger repl_completion_enumerate dans src/main.c avant commit." \
    '"$OWL" --self-test-repl-completion'

reg prescan-depth bin 0 prescan-depth-test.log \
    "PRESCAN DEPTH SELF-TESTS echecs detectes apres ce build :" \
    "Build OK mais le depth tracker prescan (fix/0492 Lot C) a derive — corriger src/prescan.c avant commit." \
    '"$OWL" --self-test-prescan-depth'

reg block-openers bin 0 block-openers-test.log \
    "BLOCK OPENER GUARD echecs detectes apres ce build :" \
    "Build OK mais le discriminant « ce mot-cle ouvre-t-il un bloc ferme par FIN ? » a derive entre la regle partagee (prescan_depth_delta) et ses consommateurs (pre-passe d'element, detecteur GO) : une construction sans FIN comptee comme ouvrante fait sortir du niveau attendu tout ce qui la suit (fonction inconnue sur un source legal, refus d'auto-ouverture). Corriger le site fautif avant commit — cf. fix/1441." \
    '"$OWL" --self-test-block-openers'

reg collection-prescan-guard bin 0 collection-prescan-guard-test.log \
    "COLLECTION PRESCAN GUARD SELF-TESTS echecs detectes apres ce build :" \
    "Build OK mais le garde de coherence pre-scan/parse des membres de collection (fix/1406) a derive : soit il ne detecte plus un membre invisible des autres fichiers, soit il accuse du code legitime — corriger src/compiler.c avant commit." \
    '"$OWL" --self-test-collection-prescan-guard'

reg catalog-bilingual bin 0 catalog-bilingual-test.log \
    "CATALOGUE BILINGUE violations detectees apres ce build :" \
    "Un catalogue genere (--vscode-data ou le dump LSP) exporte un symbole sans sa forme EN alors que le registre en porte une (invariant bilingue n°3, cf. fix/1289+1341+1342) : le symbole ne sera colore/complete qu'en francais — emettre le nameEN dans l'export fautif (src/main.c) avant commit." \
    '"$OWL" --self-test-catalog-bilingual'

reg ui-bindings bin 0 ui-bindings-test.log \
    "UI BINDINGS SELF-TESTS echecs detectes apres ce build :" \
    "Build OK mais le contrat de la table de bindings GUI sous Ouvre imbrique (fix/0522) a derive — corriger src/stdlib_ui.c avant commit." \
    '"$OWL" --self-test-ui-bindings'

reg ui-position bin 0 ui-position-test.log \
    "UI POSITION SELF-TESTS echecs detectes apres ce build :" \
    "Build OK mais le centrage + clamp de position de fenetre (fix/2160) a derive : une fenetre centree plus grande que l'ecran doit rester clampee au coin de la zone disponible (barre de titre visible), le centrage d'une fenetre normale doit rester inchange. Corriger src/ui_position.c avant commit." \
    '"$OWL" --self-test-ui-position'

reg presentation bin 0 presentation-test.log \
    "PRESENTATION SELF-TESTS echecs detectes apres ce build :" \
    "Build OK mais la couche 2 (intention de presentation designer, feat/0801) a derive : une prop Saisie designer-exposee n'a pas d'entree couche 2, ou une entree pointe une prop inexistante — corriger src/ui_presentation.c avant commit." \
    '"$OWL" --self-test-presentation'

reg debug-locals bin 0 debug-locals-test.log \
    "DEBUG LOCALS SELF-TESTS echecs detectes apres ce build :" \
    "Build OK mais la resolution des locales par nom dans le debogueur (feat/0523) a derive — corriger src/debug_api.c / table localNames d'ObjFunction avant commit." \
    '"$OWL" --self-test-debug-locals'

reg multilingue-print bin 0 multilingue-print-test.log \
    "MULTILINGUE PRINT SELF-TESTS echecs detectes apres ce build :" \
    "Build OK mais l'affichage d'une chaineMultilingue (object_print case OBJ_MULTILINGUE) ne resout plus la langue courante (fix/0640) — corriger src/object.c avant commit." \
    '"$OWL" --self-test-multilingue-print'

reg case-accent bin 0 case-accent-test.log \
    "CASE/ACCENT SELF-TESTS echecs detectes apres ce build :" \
    "Build OK mais la resolution casse-insensible/accent-insensible des identifiants au runtime (invariant 1) a derive sur un des chemins native/procedure/locale/globale/mot-cle/double type-native — corriger scanner.c/compiler.c/vm.c avant commit." \
    '"$OWL" --self-test-case-accent'

reg xls-styles bin 0 xls-styles-test.log \
    "XLS STYLES SELF-TESTS echecs detectes apres ce build :" \
    "Build OK mais la generation OOXML des styles de cellule (feat/1795 : styles.xml, attributs s=, cols/rows, ordre couleur BGR->ARGB) a derive — corriger src/stdlib_xls.c avant commit." \
    '"$OWL" --self-test-xls-styles'

reg xls-preservation bin 0 xls-preservation-test.log \
    "XLS PRESERVATION SELF-TESTS echecs detectes apres ce build :" \
    "Build OK mais la preservation ZIP-level des parts non modelisees d'un classeur ouvert (feat/1792 : macros/graphiques/medias reinjectes verbatim a xlsSauve) a derive — corriger src/stdlib_xls.c avant commit." \
    '"$OWL" --self-test-xls-preservation'

reg xls-table-export bin 0 xls-table-export-test.log \
    "XLS TABLE EXPORT SELF-TESTS echecs detectes apres ce build :" \
    "Build OK mais l'export « table -> .xlsx » du menu contextuel de table (feat/2146 : owl_ui_table_export_xlsx) a derive — corriger src/stdlib_ui.c avant commit." \
    '"$OWL" --self-test-xls-table-export'

# Non bloquant : le snapshot versionne peut legitimement etre en retard d'un
# commit. `make snapshot-check` reste dur pour la CI.
reg snapshot-registry bin 1 snapshot-registry.log \
    "snapshot registry obsolete — lancer 'make snapshot' et committer le diff." \
    "(avertissement non-bloquant ; 'make snapshot-check' reste dur pour la CI)" \
    'check_snapshot'

NB=${#CHK_ID[@]}

if [ "$ONLY" = "__list__" ]; then
    for ((i = 0; i < NB; i++)); do
        printf '%2d  %-24s %s\n' "$((i + 1))" "${CHK_ID[$i]}" "${CHK_NEED[$i]}"
    done
    exit 0
fi

# ----------------------------------------------------------
# Auto-heal : un .ninja_log tronque (build concurrent dans le meme
# build/ ou tue par un lock owl.exe/owl-lsp) fait croire a ninja que tout est
# a refaire -> rebuild complet a CHAQUE invocation, et le log ne se repare
# jamais seul (ninja appende a un fichier casse). On le detecte par un dry-run
# et on purge le log : ninja en recree un sain au build qui suit.
# ----------------------------------------------------------
if "$CMAKE_BIN" --build "$BUILDDIR" -- -n 2>&1 | grep -q "premature end of file"; then
    echo "[auto-heal] $BUILDDIR/.ninja_log corrompu detecte -> purge"
    rm -f "$BUILDDIR/.ninja_log"
fi
mkdir -p "$LOGDIR"

# Identite de la passe, pour l'historique cumulatif (cf. la ligne d'ecriture
# dans la boucle). Le mode se deduit des filtres : c'est lui qui rend les
# durees comparables entre elles — un `quick` et un `all` ne mesurent pas la
# meme chose. HISTFILE vit dans $BUILDDIR, donc gitignore.
RUN_TS=$(date -u +%Y-%m-%dT%H:%M:%SZ)
RUN_ID="$$"
if   [ -n "$ONLY" ]; then RUN_MODE="only"
elif [ -n "$SKIP" ]; then RUN_MODE="quick"
else                      RUN_MODE="all"; fi
CHK_HIST="$BUILDDIR/checks_history.tsv"

# ----------------------------------------------------------
# Execution
# ----------------------------------------------------------
STATUS=(); ELAPSED=()
build_ok=1
n_fail=0; n_warn=0; n_skip=0; n_noexec=0; n_ok=0

for ((i = 0; i < NB; i++)); do
    id="${CHK_ID[$i]}"; need="${CHK_NEED[$i]}"; log="$LOGDIR/${CHK_LOG[$i]}"
    st=""; t0=$SECONDS

    # --only : les controles hors filtre sont IGNORE, jamais verts. Sert a
    # rejouer un seul controle seul (diagnostic de course entre deux builds
    # concurrents, qui produit de faux rouges).
    if [ -n "$ONLY" ] && [ "${ONLY#*,$id,}" = "$ONLY" ]; then
        st="FILTRE"
    fi
    # --skip : filtre inverse, meme statut FILTRE (donc jamais vert non plus).
    if [ -n "$SKIP" ] && [ "${SKIP#*,$id,}" != "$SKIP" ]; then
        st="FILTRE"
    fi

    case "$need" in
        bin) [ -z "$st" ] && { [ "$build_ok" = "1" ] || st="NOEXEC"; } ;;
        lsp) if [ -n "$st" ]; then :
             elif [ "$build_ok" != "1" ]; then st="NOEXEC"
             elif [ ! -x "./$BUILDDIR/owl-lsp-test.exe" ]; then st="SKIP"; fi ;;
        dap) if [ -n "$st" ]; then :
             elif [ "$build_ok" != "1" ]; then st="NOEXEC"
             elif [ ! -x "./$BUILDDIR/owl-dap-test.exe" ]; then st="SKIP"; fi ;;
    esac

    # Portabilite : l'arbre public n'embarque pas .claude/ (systeme d'agents
    # interne). Les hook-tests gardent des hooks de ce depot prive -> SKIP (pas
    # FAIL) quand .claude/hooks est absent. En depot prive, .claude/hooks est
    # toujours la : comportement inchange.
    if [ -z "$st" ] && [ "${id#hook-tests}" != "$id" ] && [ ! -d ".claude/hooks" ]; then
        st="SKIP"
    fi

    if [ -z "$st" ]; then
        if eval "${CHK_CMD[$i]}" > "$log" 2>&1; then
            st="OK"
        else
            st=$([ "${CHK_SOFT[$i]}" = "1" ] && echo "WARN" || echo "FAIL")
        fi
    fi

    STATUS+=("$st"); ELAPSED+=($((SECONDS - t0)))
    # Historique cumulatif : $LOGDIR est ECRASE a chaque passe, donc l'etat des
    # passes precedentes est perdu et on ne peut pas voir une derive s'installer.
    # Une ligne par controle et par passe, ecrite au fil de l'eau (survit au
    # fail-fast, qui coupe la boucle avant tout recapitulatif final).
    printf '%s\t%s\t%s\t%s\t%s\t%s\n' \
        "$RUN_TS" "$RUN_ID" "$RUN_MODE" "$id" "$st" "$((SECONDS - t0))" >> "$CHK_HIST"
    case "$st" in
        OK)     n_ok=$((n_ok + 1)) ;;
        WARN)   n_warn=$((n_warn + 1)) ;;
        SKIP|FILTRE) n_skip=$((n_skip + 1)) ;;
        NOEXEC) n_noexec=$((n_noexec + 1)) ;;
        FAIL)   n_fail=$((n_fail + 1)); [ "$id" = "build" ] && build_ok=0 ;;
    esac

    if [ "$st" = "FAIL" ] && [ "$KEEP_GOING" = "0" ]; then
        echo ""
        echo "/!\\  ${CHK_BAN[$i]}"
        cat "$log"
        echo "/!\\  ${CHK_HINT[$i]}"
        exit 1
    fi
    if [ "$st" = "WARN" ] && [ "$KEEP_GOING" = "0" ]; then
        echo ""
        echo "/!\\  ${CHK_BAN[$i]}"
        echo "    ${CHK_HINT[$i]}"
    fi

    if [ "$KEEP_GOING" = "1" ]; then
        printf '[%2d/%2d] %-24s %-6s %3ds\n' "$((i + 1))" "$NB" "$id" "$st" "${ELAPSED[$i]}"
    fi
done

# Pre-chauffe antivirus : owl.exe et owl-dap.exe sont deja exerces
# par les self-tests ci-dessus (donc scannes par Defender pendant le build),
# mais build/owl-lsp.exe n'est jamais relance — les tests LSP tournent sur
# owl-lsp-test.exe, un binaire distinct. Resultat : le 1er spawn du LSP par
# VSCode apres un build encaissait le scan Defender (~1,5 s). On le front-load
# ici. Non bloquant (EOF stdin => sortie propre ; timeout de secours).
if [ "$build_ok" = "1" ] && [ -x "./$BUILDDIR/owl-lsp.exe" ]; then
    timeout 30 "./$BUILDDIR/owl-lsp.exe" < /dev/null > /dev/null 2>&1 || true
fi

if [ "$KEEP_GOING" = "0" ]; then
    # Bilan une-ligne du mode fail-fast (make all). On n'arrive ici QUE si
    # aucun controle n'a FAIL (le chemin FAIL a deja exit 1 dans la boucle) :
    # le silence d'un succes complet se lisait comme un echec. Meme
    # vocabulaire que le recapitulatif check-all (OK / AVERTISSEMENT / IGNORE)
    # pour etre reconnaissable, prefixe distinct pour lever l'ambiguite de mode.
    echo ""
    printf ' Batterie make all : %d OK / %d AVERTISSEMENT / %d IGNORE\n' \
        "$n_ok" "$n_warn" "$n_skip"
    exit 0
fi

# ----------------------------------------------------------
# Recapitulatif
# ----------------------------------------------------------
echo ""
echo "================================================================================"
echo " RECAPITULATIF — $NB controles (make check-all)"
echo "================================================================================"
reds=""
for ((i = 0; i < NB; i++)); do
    st="${STATUS[$i]}"
    case "$st" in
        OK)     mark="  OK    " ; note="" ;;
        WARN)   mark="  WARN  " ; note="${CHK_HINT[$i]}" ;;
        SKIP)   mark="  SKIP  " ; note="binaire optionnel absent" ;;
        FILTRE) mark=" FILTRE " ; note="hors filtre --only : non mesure" ;;
        NOEXEC) mark=" NOEXEC " ; note="non interpretable : le build amont a echoue" ;;
        FAIL)   mark=" ECHEC  " ; note="$LOGDIR/${CHK_LOG[$i]}"; reds="$reds ${CHK_ID[$i]}" ;;
    esac
    printf ' %2d %s %-24s %4ds  %s\n' "$((i + 1))" "$mark" "${CHK_ID[$i]}" "${ELAPSED[$i]}" "$note"
done
echo "--------------------------------------------------------------------------------"
printf ' %d OK / %d ECHEC / %d NON EXECUTE / %d IGNORE / %d AVERTISSEMENT\n' \
    "$n_ok" "$n_fail" "$n_noexec" "$n_skip" "$n_warn"
if [ "$n_fail" -gt 0 ]; then
    echo " Rouges :$reds"
fi
if [ "$n_noexec" -gt 0 ]; then
    echo " NON EXECUTE = non mesure. Ne pas lire comme un vert."
fi
echo "================================================================================"

[ "$n_fail" -eq 0 ]
