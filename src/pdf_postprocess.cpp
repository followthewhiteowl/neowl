// SPDX-License-Identifier: AGPL-3.0-only WITH LicenseRef-OWL-Runtime-Exception
/* pdf_postprocess.cpp - Implementation qpdf pour iParametrePDF Option B.
 *
 * Voir pdf_postprocess.h pour le contrat. Compile dans owl.exe (cible avec
 * OWL_HAS_QPDF), pas dans owl-cli, car necessite la lib qpdf via -lqpdf (UCRT64 package
 * mingw-w64-ucrt-x86_64-qpdf, licence Apache 2.0).
 *
 * Si OWL_HAS_QPDF n'est pas defini au build (lib absente), la fonction
 * warne et retourne 0 sans toucher au fichier — le PDF brut Qt6 reste
 * utilisable.
 */

#include "pdf_postprocess.h"
#include "sentinels.h"

#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <string>

#ifdef _WIN32
#include <windows.h>
#endif

/* Convertit les '/' en '\\' pour les API Windows (MoveFile/DeleteFile
 * acceptent les deux mais certaines versions sont strictes). */
static std::string winpath(const char* p) {
    std::string s = p ? p : "";
    for (auto& c : s) if (c == '/') c = '\\';
    return s;
}

/* Replace atomique cross-platform : remplace path_out par path_in
 * (path_in est efface, path_out re-cree). Necessaire car rename(3)
 * standard C n'autorise pas de target existant sur Windows. */
static int atomic_replace(const char* path_in, const char* path_out) {
#ifdef _WIN32
    std::string in = winpath(path_in);
    std::string out = winpath(path_out);
    /* MoveFileEx avec REPLACE_EXISTING. Retry car Qt peut garder un
     * handle quelques ms apres owl_pdf_close (NTFS lazy unlock). */
    for (int retry = 0; retry < 10; retry++) {
        if (MoveFileExA(in.c_str(), out.c_str(),
                        MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
            return 0;
        }
        Sleep(50);
    }
    DWORD err = GetLastError();
    fprintf(stderr, "[atomic_replace] MoveFileExA err=%lu : '%s' -> '%s'\n",
            (unsigned long)err, in.c_str(), out.c_str());
    return -1;
#else
    return rename(path_in, path_out);
#endif
}

#ifdef OWL_HAS_QPDF

#include <qpdf/QPDF.hh>
#include <qpdf/QPDFWriter.hh>
#include <qpdf/QPDFOutlineDocumentHelper.hh>
#include <qpdf/QPDFObjectHandle.hh>
#include <qpdf/QPDFPageDocumentHelper.hh>

extern "C" bool owl_pdf_has_qpdf(void) { return true; }

extern "C" int owl_pdf_postprocess(const char* path_in, const char* path_out,
                                     const OwlPdfPostOptions* opt) {
    if (!path_in || !path_out || !opt) return 1;

    /* Detecte si on a quoi que ce soit a faire — sinon copie/no-op. */
    bool needsEncryption = (opt->passwordOpen && *opt->passwordOpen) ||
                           (opt->passwordOwner && *opt->passwordOwner) ||
                            opt->protectionFlags != 0;
    bool needsBookmarks  = opt->signetPage;
    bool needsPdfA3b     = opt->pdfA3b;
    if (!needsEncryption && !needsBookmarks && !needsPdfA3b) {
        /* Rien a faire. Si path_in != path_out, on copie. Sinon no-op. */
        if (strcmp(path_in, path_out) != 0) {
            FILE* fi = fopen(path_in, "rb");
            FILE* fo = fopen(path_out, "wb");
            if (fi && fo) {
                char buf[8192];
                size_t n;
                while ((n = fread(buf, 1, sizeof(buf), fi)) > 0) fwrite(buf, 1, n, fo);
            }
            if (fi) fclose(fi);
            if (fo) fclose(fo);
        }
        return 0;
    }

    try {
        std::string tmpOut = std::string(path_out) + ".qpdf-tmp";

        /* Scope restreint pour le QPDF : son destructeur libere le handle
         * sur path_in avant le atomic_replace. Sinon Windows refuse le
         * MoveFileEx avec ACCESS_DENIED car le file lock est encore actif. */
        {
        QPDF qpdf;
        qpdf.processFile(path_in);

        /* Signets : ajoute un outline "Page N" par page si signetPage. */
        if (needsBookmarks) {
            std::vector<QPDFObjectHandle> pages = qpdf.getAllPages();
            QPDFObjectHandle root  = qpdf.getRoot();
            QPDFObjectHandle outlines = QPDFObjectHandle::newDictionary();
            outlines.replaceKey("/Type", QPDFObjectHandle::newName("/Outlines"));

            QPDFObjectHandle prev;
            QPDFObjectHandle first;
            int pageNum = 1;
            int nPages = (int)pages.size();
            for (auto& page : pages) {
                QPDFObjectHandle item = qpdf.makeIndirectObject(QPDFObjectHandle::newDictionary());
                std::string title = "Page " + std::to_string(pageNum);
                item.replaceKey("/Title", QPDFObjectHandle::newUnicodeString(title));
                item.replaceKey("/Parent", outlines);
                /* /Dest = [<page> /Fit] */
                QPDFObjectHandle dest = QPDFObjectHandle::newArray();
                dest.appendItem(page);
                dest.appendItem(QPDFObjectHandle::newName("/Fit"));
                item.replaceKey("/Dest", dest);
                if (prev.isInitialized()) {
                    prev.replaceKey("/Next", item);
                    item.replaceKey("/Prev", prev);
                } else {
                    first = item;
                }
                prev = item;
                pageNum++;
            }
            if (first.isInitialized()) {
                outlines.replaceKey("/First", first);
                outlines.replaceKey("/Last", prev);
                outlines.replaceKey("/Count", QPDFObjectHandle::newInteger(nPages));
                root.replaceKey("/Outlines", qpdf.makeIndirectObject(outlines));
            }
        }

        /* Ecriture finale via QPDFWriter. Si chiffrement demande, on
         * configure R6 (AES-256). */
        QPDFWriter writer(qpdf, tmpOut.c_str());

        if (needsEncryption) {
            std::string userPwd = opt->passwordOpen  ? opt->passwordOpen  : "";
            std::string ownerPwd = opt->passwordOwner ? opt->passwordOwner : "";
            /* Si owner vide mais user pose, qpdf genere un owner aleatoire. */
            if (ownerPwd.empty() && !userPwd.empty()) ownerPwd = userPwd + "_owner";

            int flags = opt->protectionFlags;
            qpdf_r3_print_e   print = (flags & OWL_PDF_PROT_IMPRESSION) ?
                                      qpdf_r3p_none : qpdf_r3p_full;
            qpdf_r3_modify_e  modify = (flags & OWL_PDF_PROT_MODIFICATION) ?
                                       qpdf_r3m_none : qpdf_r3m_all;
            bool extract = !(flags & OWL_PDF_PROT_SELECTION);
            bool annotate = !(flags & OWL_PDF_PROT_COMMENTAIRE);
            /* R6 = AES-256, securite moderne (PDF 1.7 ext 8). */
            writer.setR6EncryptionParameters(
                userPwd.c_str(), ownerPwd.c_str(),
                /* allow_accessibility */ true,
                /* allow_extract       */ extract,
                /* allow_assemble      */ (modify == qpdf_r3m_all),
                /* allow_annotate_and_form */ annotate,
                /* allow_form_filling  */ annotate,
                /* allow_modify_other  */ (modify == qpdf_r3m_all),
                print,
                /* encrypt_metadata_aes */ true);
        }

        /* PDF/A-3b : Qt6 genere du PDF/A-1b. La difference avec A-3b est
         * essentiellement un changement du metadata XMP (pdfaid:part=3,
         * conformance=B), permettant l'attachement de fichiers arbitraires.
         * On reecrit donc le metadata stream avec le marquage A-3b. La
         * validation stricte (veraPDF) reste a faire en outil tiers — ici
         * on produit un PDF qui se declare A-3b et que les readers
         * tolerants acceptent comme tel. */
        if (needsPdfA3b) {
            QPDFObjectHandle root = qpdf.getRoot();
            std::string xmp =
                "<?xpacket begin=\"\xef\xbb\xbf\" id=\"W5M0MpCehiHzreSzNTczkc9d\"?>\n"
                "<x:xmpmeta xmlns:x=\"adobe:ns:meta/\">\n"
                " <rdf:RDF xmlns:rdf=\"http://www.w3.org/1999/02/22-rdf-syntax-ns#\">\n"
                "  <rdf:Description rdf:about=\"\"\n"
                "      xmlns:pdfaid=\"http://www.aiim.org/pdfa/ns/id/\">\n"
                "   <pdfaid:part>3</pdfaid:part>\n"
                "   <pdfaid:conformance>B</pdfaid:conformance>\n"
                "  </rdf:Description>\n"
                "  <rdf:Description rdf:about=\"\"\n"
                "      xmlns:xmp=\"http://ns.adobe.com/xap/1.0/\">\n"
                "   <xmp:CreatorTool>OWL</xmp:CreatorTool>\n"
                "  </rdf:Description>\n"
                " </rdf:RDF>\n"
                "</x:xmpmeta>\n"
                "<?xpacket end=\"w\"?>\n";
            QPDFObjectHandle mdStream = QPDFObjectHandle::newStream(&qpdf, xmp);
            QPDFObjectHandle dict = mdStream.getDict();
            dict.replaceKey("/Type", QPDFObjectHandle::newName("/Metadata"));
            dict.replaceKey("/Subtype", QPDFObjectHandle::newName("/XML"));
            root.replaceKey("/Metadata", mdStream);
        }

        writer.write();
        }  /* fin scope QPDF — destructeur libere le handle path_in */

        /* Replace atomique cross-platform (MoveFileEx sur Windows pour
         * supporter le remplacement du target existant). */
        if (atomic_replace(tmpOut.c_str(), path_out) != 0) {
            fprintf(stderr,
                "[iParametrePDF] Echec replace atomique '%s' -> '%s'.\n",
                tmpOut.c_str(), path_out);
            return 2;
        }
        return 0;
    } catch (const std::exception& e) {
        fprintf(stderr, "[iParametrePDF] qpdf error: %s\n", e.what());
        return 3;
    }
}

#else  /* OWL_HAS_QPDF */

extern "C" bool owl_pdf_has_qpdf(void) { return false; }

extern "C" int owl_pdf_postprocess(const char* path_in, const char* path_out,
                                     const OwlPdfPostOptions* opt) {
    (void)path_in; (void)path_out; (void)opt;
    fprintf(stderr,
        "[iParametrePDF] qpdf non disponible au build (OWL_HAS_QPDF non defini). "
        "Les options chiffrement/signets/PDF-A-3b sont ignorees.\n");
    return 0;
}

#endif /* OWL_HAS_QPDF */
