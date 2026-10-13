/*
 * ontology_emit.h — materialize a parsed ontology definition into the graph
 * buffer as first-class nodes and edges.
 *
 * Every emitted node carries file_path == CBM_ONTOLOGY_FILENAME so the
 * incremental indexer's per-file lifecycle deletes/re-emits them exactly
 * like file content. Qualified names are deterministic
 * (<project>.ontology.<kind>.<name>), so ONT_BINDS edges to code nodes are
 * re-resolvable on every index — and so are references INTO the ontology
 * (e.g. doc links).
 */
#ifndef CBM_ONTOLOGY_EMIT_H
#define CBM_ONTOLOGY_EMIT_H

#include "graph_buffer/graph_buffer.h"
#include "ontology_def.h"

typedef struct cbm_store cbm_store_t;

/* One binding that named a code QN nothing in the graph answers to. */
typedef struct {
    char *from_qn; /* the ontology element that holds the reference */
    char *to_qn;   /* the code QN it pointed at */
} cbm_ontology_unresolved_ref_t;

typedef struct {
    cbm_ontology_unresolved_ref_t *items;
    int count;
} cbm_ontology_unresolved_t;

void cbm_ontology_unresolved_free(cbm_ontology_unresolved_t *u);

/* Emit the whole definition. Returns 0 on success (unresolved bindings are
 * not failures — they land in `unresolved` when non-NULL). */
int cbm_ontology_emit(cbm_gbuf_t *gbuf, const cbm_ontology_def_t *def, const char *project,
                      cbm_ontology_unresolved_t *unresolved);

/* Same emit against a live SQLite store — used by manage_ontology live
 * apply, where bindings must resolve to real store node ids. */
int cbm_ontology_emit_store(cbm_store_t *store, const cbm_ontology_def_t *def,
                            const char *project, cbm_ontology_unresolved_t *unresolved);

/* Parse <repo_path>/.cbm-ontology.json. Returns NULL when the file is absent
 * or unreadable; parse diagnostics still land in `errors` when given. */
cbm_ontology_def_t *cbm_ontology_load_file(const char *repo_path, const char *project,
                                           cbm_ontology_errors_t *errors);

/* Live apply: load the file, refuse on diagnostics, replace all ontology
 * nodes in the store, re-emit. Does NOT touch the definition file itself. */
enum {
    CBM_ONTOLOGY_APPLY_OK = 0,
    CBM_ONTOLOGY_APPLY_NO_FILE = 1,
    CBM_ONTOLOGY_APPLY_INVALID = 2,
    CBM_ONTOLOGY_APPLY_STORE_ERR = 3,
    CBM_ONTOLOGY_APPLY_ERR = -1,
};
int cbm_ontology_apply_store(cbm_store_t *store, const char *repo_path, const char *project,
                             cbm_ontology_errors_t *errors,
                             cbm_ontology_unresolved_t *unresolved);

#endif /* CBM_ONTOLOGY_EMIT_H */
