/*
 * pass_ontology.c — materialize <repo>/.cbm-ontology.json into the graph.
 *
 * The file is ontology-as-code: object types, objects, interfaces, link
 * types, action types and functions that bind to the code graph by
 * qualified name. It runs last in the sequential pass list (and after k8s
 * on the parallel path) so every code QN it can bind to already exists.
 *
 * Failure policy: a missing file is normal (no ontology), a broken file is
 * a recorded skip — never a failed index — and unresolved code bindings
 * become "ontology_binding" skip rows visible in index_status.
 */
#include "pipeline/pipeline_internal.h"

#include "ontology_def.h"
#include "ontology_emit.h"
#include "foundation/compat_fs.h"
#include "foundation/constants.h"
#include "foundation/log.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define ONT_MAX_FILE_BYTES (4 * 1024 * 1024)

static char *ont_read_file(const char *path, size_t *out_len) {
    FILE *fp = cbm_fopen(path, "rb");
    if (!fp) {
        return NULL;
    }
    (void)fseek(fp, 0, SEEK_END);
    long sz = ftell(fp);
    if (sz <= 0 || sz > ONT_MAX_FILE_BYTES) {
        (void)fclose(fp);
        return NULL;
    }
    (void)fseek(fp, 0, SEEK_SET);
    char *buf = malloc((size_t)sz + 1);
    if (!buf) {
        (void)fclose(fp);
        return NULL;
    }
    size_t rd = fread(buf, 1, (size_t)sz, fp);
    (void)fclose(fp);
    if ((long)rd != sz) {
        free(buf);
        return NULL;
    }
    buf[sz] = '\0';
    *out_len = (size_t)sz;
    return buf;
}

int cbm_pipeline_pass_ontology(cbm_pipeline_ctx_t *ctx, const cbm_file_info_t *files,
                               int file_count) {
    (void)files;
    (void)file_count;
    if (!ctx || !ctx->repo_path || !ctx->gbuf || !ctx->project_name) {
        return 0;
    }

    char path[CBM_SZ_4K];
    int n = snprintf(path, sizeof(path), "%s/%s", ctx->repo_path, CBM_ONTOLOGY_FILENAME);
    if (n <= 0 || (size_t)n >= sizeof(path)) {
        return 0;
    }
    size_t len = 0;
    char *text = ont_read_file(path, &len);
    if (!text) {
        return 0; /* no ontology file — nothing to do */
    }

    cbm_ontology_errors_t errors = {0};
    cbm_ontology_def_t *def =
        cbm_ontology_def_parse(text, (int)len, ctx->project_name, &errors);
    free(text);
    if (!def) {
        cbm_pipeline_add_file_error(ctx->pipeline, CBM_ONTOLOGY_FILENAME,
                                    errors.count > 0 ? errors.items[0] : "invalid ontology file",
                                    "ontology");
        cbm_ontology_errors_free(&errors);
        return 0;
    }
    for (int i = 0; i < errors.count; i++) {
        cbm_pipeline_add_file_error(ctx->pipeline, CBM_ONTOLOGY_FILENAME, errors.items[i],
                                    "ontology");
    }
    cbm_ontology_errors_free(&errors);

    cbm_ontology_unresolved_t unresolved = {0};
    int rc = cbm_ontology_emit(ctx->gbuf, def, ctx->project_name, &unresolved);
    for (int i = 0; i < unresolved.count; i++) {
        char detail[CBM_SZ_1K];
        snprintf(detail, sizeof(detail), "%s -> %s", unresolved.items[i].from_qn,
                 unresolved.items[i].to_qn);
        cbm_pipeline_add_file_error(ctx->pipeline, CBM_ONTOLOGY_FILENAME, detail,
                                    "ontology_binding");
    }
    cbm_ontology_unresolved_free(&unresolved);
    cbm_ontology_def_free(def);

    cbm_log_info("pass.ontology", "rc", rc == 0 ? "ok" : "err");
    return 0;
}
