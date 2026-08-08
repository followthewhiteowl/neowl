# syntax=docker/dockerfile:1
#
# NeOwl - image de base conteneur (variante Alpine, minimale - c'est le :latest).
#
# `owl` est un binaire autonome, sans dependance partagee : il tourne sur une
# base minuscule. On ajoute juste ce dont il a besoin au runtime quand il parle
# au monde :
#   - ca-certificates : connexions TLS (client/serveur HTTP, SQL...)
#   - font-dejavu     : polices reelles pour le rendu PDF
#   - tzdata          : fuseaux horaires (dates)
#
# `owl` = le binaire Linux de la release (voir la page des releases :
# neowl-<version>-linux-x86_64.tar.gz). Pour une base plus riche (glibc + apt),
# voir la variante Debian (tag :<version>-debian).
#
# Utilisation :
#   docker run --rm ghcr.io/followthewhiteowl/neowl:latest --version
#   docker run --rm -v "$PWD:/work" ghcr.io/followthewhiteowl/neowl:latest /work/script.owl
# ou comme base d'une image :
#   FROM ghcr.io/followthewhiteowl/neowl:latest

FROM alpine:3.20

RUN apk add --no-cache ca-certificates font-dejavu tzdata

COPY owl /usr/local/bin/owl
RUN chmod 0755 /usr/local/bin/owl

# Utilisateur non-root (bonne pratique image de base).
RUN adduser -D -u 1000 neowl
USER neowl
WORKDIR /work

ENTRYPOINT ["/usr/local/bin/owl"]
