// SPDX-License-Identifier: AGPL-3.0-only
/* llama.cpp / ggml smoke test — validation standalone du vendoring.
 *
 * OBJECTIF PRINCIPAL : prouver au RUNTIME que le backend CPU ggml est bien
 * ENREGISTRE une fois la lib liee statiquement dans un binaire OWL. C'est le
 * vrai risque du vendoring (piege upstream #8166) : le static-initializer /
 * l'appel de registration du backend CPU peut etre droppe par le linker
 * (--gc-sections l'aggrave), donnant un "no backend registered" a l'execution
 * alors que le LINK a reussi. On le detecte ici sans avoir besoin d'un modele :
 * ggml_backend_dev_count() doit voir >= 1 device et un device de type CPU doit
 * exister. Si --whole-archive sur libggml-cpu.a n'a pas fait son boulot, ce
 * test echoue a l'execution.
 *
 * PARTIE OPTIONNELLE : si un chemin GGUF est fourni (argv[1] ou la variable
 * d'env OWL_LLAMA_TEST_GGUF), on exerce toute la surface d'inference qu'OWL
 * utilisera au sous-chantier B : chargement modele -> vocab -> tokenize
 * "Hello" -> contexte -> decode -> logits non vides. Sans GGUF fourni, cette
 * partie est SKIP (le test reste vert et deterministe pour la batterie : on
 * ne vendore aucun modele de 600 Mo).
 *
 * Build/lie via la cible CMake llamacpp_smoketest (owl_llama, whole-archive
 * ggml-cpu, memes flags de hardening qu'owl.exe).
 */
#include "llama.h"
#include "ggml-backend.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Silence le logging verbeux de llama/ggml : on ne garde que nos lignes. */
static void quiet_log(enum ggml_log_level level, const char *text, void *ud) {
    (void)level; (void)text; (void)ud;
}

static int prove_cpu_backend_registered(void) {
    size_t nreg = ggml_backend_reg_count();
    size_t ndev = ggml_backend_dev_count();
    printf("backend regs=%zu devices=%zu\n", nreg, ndev);
    if (ndev == 0) {
        fprintf(stderr, "FAIL: aucun device ggml enregistre "
                        "(#8166 : backend CPU droppe par le linker ?)\n");
        return 1;
    }
    ggml_backend_dev_t cpu = ggml_backend_dev_by_type(GGML_BACKEND_DEVICE_TYPE_CPU);
    if (cpu == NULL) {
        fprintf(stderr, "FAIL: aucun device CPU (registration CPU absente)\n");
        return 1;
    }
    printf("cpu device: %s\n", ggml_backend_dev_name(cpu));
    return 0;
}

/* Charge un GGUF et exerce le chemin d'inference complet. Retour 0 = OK. */
static int full_inference(const char *gguf_path) {
    struct llama_model_params mp = llama_model_default_params();
    mp.n_gpu_layers = 0; /* CPU-only, coherent avec le vendoring */
    struct llama_model *model = llama_model_load_from_file(gguf_path, mp);
    if (model == NULL) {
        fprintf(stderr, "FAIL: chargement du modele '%s'\n", gguf_path);
        return 1;
    }

    const struct llama_vocab *vocab = llama_model_get_vocab(model);
    int32_t n_vocab = llama_vocab_n_tokens(vocab);
    printf("model charge, n_vocab=%d\n", n_vocab);

    const char *prompt = "Hello";
    llama_token tokens[64];
    int32_t n_tok = llama_tokenize(vocab, prompt, (int32_t)strlen(prompt),
                                   tokens, 64, /*add_special*/ true,
                                   /*parse_special*/ false);
    if (n_tok <= 0) {
        fprintf(stderr, "FAIL: tokenize a retourne %d\n", n_tok);
        llama_model_free(model);
        return 1;
    }
    printf("tokenize '%s' -> %d tokens\n", prompt, n_tok);

    struct llama_context_params cp = llama_context_default_params();
    cp.n_ctx = 128;
    struct llama_context *ctx = llama_init_from_model(model, cp);
    if (ctx == NULL) {
        fprintf(stderr, "FAIL: creation du contexte\n");
        llama_model_free(model);
        return 1;
    }

    struct llama_batch batch = llama_batch_get_one(tokens, n_tok);
    int32_t rc = llama_decode(ctx, batch);
    if (rc != 0) {
        fprintf(stderr, "FAIL: llama_decode a retourne %d\n", rc);
        llama_free(ctx);
        llama_model_free(model);
        return 1;
    }

    float *logits = llama_get_logits_ith(ctx, n_tok - 1);
    if (logits == NULL) {
        fprintf(stderr, "FAIL: logits NULL apres decode\n");
        llama_free(ctx);
        llama_model_free(model);
        return 1;
    }
    /* Sortie non vide : au moins un logit fini et un argmax plausible. */
    int argmax = 0;
    float best = logits[0];
    for (int32_t i = 1; i < n_vocab; i++) {
        if (logits[i] > best) { best = logits[i]; argmax = i; }
    }
    char piece[128];
    int32_t pn = llama_token_to_piece(vocab, argmax, piece, sizeof(piece) - 1, 0, false);
    if (pn < 0) pn = 0;
    piece[pn] = '\0';
    printf("decode OK, next-token argmax=%d logit=%.3f piece=\"%s\"\n",
           argmax, best, piece);

    llama_free(ctx);
    llama_model_free(model);
    return 0;
}

int main(int argc, char **argv) {
    llama_log_set(quiet_log, NULL);

    llama_backend_init();

    int rc = prove_cpu_backend_registered();
    if (rc != 0) {
        llama_backend_free();
        return rc;
    }

    /* Chemin de chargement lie : path bidon -> doit echouer proprement (NULL),
     * ce qui prouve que le loader GGUF est bien dans le binaire. */
    {
        struct llama_model_params mp = llama_model_default_params();
        struct llama_model *m = llama_model_load_from_file("owl-nonexistent.gguf", mp);
        if (m != NULL) {
            fprintf(stderr, "FAIL: chargement d'un chemin bidon aurait du echouer\n");
            llama_model_free(m);
            llama_backend_free();
            return 1;
        }
        printf("bogus-path load: NULL comme attendu (loader GGUF lie)\n");
    }

    const char *gguf = (argc > 1) ? argv[1] : getenv("OWL_LLAMA_TEST_GGUF");
    if (gguf != NULL && gguf[0] != '\0') {
        printf("== inference complete sur %s ==\n", gguf);
        rc = full_inference(gguf);
        if (rc != 0) { llama_backend_free(); return rc; }
    } else {
        printf("(inference complete SKIP : fournir un GGUF via argv[1] ou "
               "OWL_LLAMA_TEST_GGUF)\n");
    }

    llama_backend_free();
    printf("llamacpp smoke OK\n");
    return 0;
}
