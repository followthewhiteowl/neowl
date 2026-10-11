#!/usr/bin/env bash
#  — les events de niveau ServiceREST / OpérationHTTP doivent etre
# dumpes par `owl --vscode-data` (sections events_service / events_operation),
# miroir de events_collection. Sans ca, la regex TextMate
# `support.function.event.owl` (generee depuis ce dump) ne colore pas
# surRequête / surErreur / surAuthentification en bleu. On isole chaque
# section et on liste ses entrees {canonical,nameEN} dans l'ordre du catalogue.
data=$(cat)
section() {
    printf '%s' "$data" | sed -n "/\"$1\": \[/,/^  \],/p" \
        | grep -oE '\{"canonical":[^}]*\}'
}
echo "== events_service =="
section events_service
echo "== events_operation =="
section events_operation
