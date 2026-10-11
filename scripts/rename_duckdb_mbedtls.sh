#!/usr/bin/env bash
# SPDX-License-Identifier: AGPL-3.0-only WITH LicenseRef-OWL-Runtime-Exception
# Post-build : renomme les symboles mbedtls_* de libowl_duckdb.a en
# duckdb_mbedtls_* pour eviter le conflit avec libowl_mbedtls.a au link.
#
# DuckDB amalgamation embarque sa propre copie de mbedtls 3.x (utilisee
# par ses extensions httpfs/encryption). OWL utilise mbedtls 3.x cote
# reseau (PG/MySQL TLS). Les deux copies se collisionnent au link :
#   lld-link: error: duplicate symbol: mbedtls_internal_sha256_process
#
# Solution : objcopy --redefine-syms renomme les ~229 symboles mbedtls_*
# dans libowl_duckdb.a (definitions ET references). DuckDB devient
# auto-suffisant avec sa copie privee, OWL garde la sienne.
#
# Usage : bash scripts/rename_duckdb_mbedtls.sh <input.a> [output.a]
#         output.a default = input.a (in-place).
#
# Necessite : objcopy (binutils, MSYS2 UCRT64 sur Windows).
set -euo pipefail

IN="$1"
OUT="${2:-$1}"

[ -f "$IN" ] || { echo "ERROR: $IN absent"; exit 2; }

OBJCOPY="${OBJCOPY:-objcopy}"
NM="${NM:-nm}"

# Liste les symboles mbedtls_* definis (T ou D) dans l'archive.
SYMS_FILE="$(mktemp)"
REDEF_FILE="$(mktemp)"
trap 'rm -f "$SYMS_FILE" "$REDEF_FILE"' EXIT

"$NM" "$IN" 2>/dev/null \
    | awk '$2 == "T" || $2 == "D" || $2 == "R" || $2 == "B" || $2 == "C" {print $3}' \
    | grep '^mbedtls_' \
    | sort -u > "$SYMS_FILE"

NSYMS=$(wc -l < "$SYMS_FILE")
if [ "$NSYMS" -eq 0 ]; then
    echo "==> rename_duckdb_mbedtls: 0 symboles a renommer (deja fait ?)"
    [ "$IN" != "$OUT" ] && cp "$IN" "$OUT"
    exit 0
fi

awk '{print $1, "duckdb_" $1}' "$SYMS_FILE" > "$REDEF_FILE"

echo "==> rename_duckdb_mbedtls: $NSYMS symboles mbedtls_* -> duckdb_mbedtls_*"
"$OBJCOPY" --redefine-syms="$REDEF_FILE" "$IN" "$OUT"
echo "    out : $OUT"
