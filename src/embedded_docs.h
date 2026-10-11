// SPDX-License-Identifier: AGPL-3.0-only WITH LicenseRef-OWL-Runtime-Exception
/* embedded_docs.h — table d'accès aux fichiers de doc embarqués dans
 * le binaire owl. Le fichier embedded_docs.c est généré par
 * tools/embed_docs à chaque build (cf. CMakeLists.txt).
 *
 * Trois sections :
 *   - PUBLIC_DOCS : fiches docs/public_documentation (AI-native + humain)
 *   - SCENARIOS   : fichiers tests/scenarios (exemples canon)
 *   - TEMPLATES   : squelettes prêts à l'emploi (dossier docs/public_templates/)
 *
 * Chaque entrée pointe sur un blob de bytes du fichier source (texte
 * brut). Le parsing (@sujet, @résumé, procédures publiques, etc.) se
 * fait à l'exécution dans src/cli_doc.c.
 *
 * Le nom de fichier exposé est le STEM (basename sans extension) :
 *   "00_apercu_du_langage" -> docs/public_documentation/00_apercu_du_langage.md
 *   "test_ChaineDecoupe"   -> tests/scenarios/natives/test_ChaineDecoupe.owl
 *
 * Le path complet (relatif au repo) est aussi exposé pour diagnostic.
 */

#ifndef OWL_EMBEDDED_DOCS_H
#define OWL_EMBEDDED_DOCS_H

#include <stddef.h>  /* NULL */

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    const char*           name;     /* basename sans extension */
    const char*           path;     /* chemin relatif repo, séparateur '/' */
    const unsigned char*  data;     /* bytes du fichier (non zero-terminé) */
    long                  len;      /* longueur en octets */
} OwlEmbeddedDoc;

/* Tables terminées par { NULL, NULL, NULL, 0 }. */
extern const OwlEmbeddedDoc EMBEDDED_PUBLIC_DOCS[];
extern const OwlEmbeddedDoc EMBEDDED_SCENARIOS[];
extern const OwlEmbeddedDoc EMBEDDED_TEMPLATES[];

/* Comptages exacts (hors sentinel). */
extern const int EMBEDDED_PUBLIC_DOCS_COUNT;
extern const int EMBEDDED_SCENARIOS_COUNT;
extern const int EMBEDDED_TEMPLATES_COUNT;

#ifdef __cplusplus
}
#endif

#endif /* OWL_EMBEDDED_DOCS_H */
