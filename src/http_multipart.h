// SPDX-License-Identifier: AGPL-3.0-only WITH LicenseRef-OWL-Runtime-Exception
#ifndef OWL_HTTP_MULTIPART_H
#define OWL_HTTP_MULTIPART_H

/* Parseur multipart/form-data pour le DSL de service HTTP.
 *
 * Entree = un corps DEJA bufferise (le plafond corps_max, , s'applique
 * en amont : ce module n'ajoute aucune limite de taille). Le corps est decoupe
 * par le boundary annonce dans l'en-tete Content-Type ; chaque partie est
 * dissociee en metadonnees (name / filename / Content-Type) et contenu.
 *
 * Discipline memoire : le CONTENU d'une partie (`data`) pointe DANS le corps
 * fourni (aucune copie), les metadonnees sont des chaines possedees. Le corps
 * doit rester vivant tant que le formulaire est consulte. Aucun objet VM ni GC
 * ici : allocation C simple, liberee par owl_multipart_free. */

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    char*       name;         /* nom du champ (form-data name), possede */
    char*       filename;     /* nom de fichier client, NULL si champ texte */
    char*       contentType;  /* Content-Type de la partie, NULL si absent */
    const char* data;         /* contenu : pointeur DANS le corps (emprunte) */
    size_t      dataLen;      /* longueur du contenu de la partie */
} OwlMultipartPart;

typedef struct {
    OwlMultipartPart* parts;
    int               count;
} OwlMultipartForm;

/* Parse un corps multipart/form-data. `contentType` = valeur brute de l'en-tete
 * (pour extraire le boundary). Retourne 0 si OK, -1 si malforme (boundary
 * absent, structure invalide, en-tetes tronques). `out` est toujours initialise
 * et doit etre libere via owl_multipart_free, quel que soit le retour. */
int  owl_multipart_parse(const char* contentType,
                         const char* body, size_t bodyLen,
                         OwlMultipartForm* out);

void owl_multipart_free(OwlMultipartForm* form);

/* Ecrit le contenu d'une partie dans un fichier temporaire au nom GENERE en C
 * (jamais derive du nom client, potentiellement hostile). `outPath` recoit le
 * chemin du fichier cree. Retourne 0 si OK, -1 sinon (repertoire temp ou
 * ecriture indisponible). L'appelant est responsable du remove() en fin de
 * requete (owl_multipart_remove_temp). */
int  owl_multipart_write_temp(const char* data, size_t len,
                              char* outPath, size_t cap);

/* Supprime un fichier temporaire cree par owl_multipart_write_temp. Tolere un
 * chemin vide (no-op). */
void owl_multipart_remove_temp(const char* path);

#ifdef __cplusplus
}
#endif

#endif /* OWL_HTTP_MULTIPART_H */
