/*
 * ontology_emit.c — def -> graph nodes/edges.
 *
 * One emit core drives two sinks: the pipeline's in-memory graph buffer
 * (full + incremental indexes) and the SQLite store (live apply via
 * manage_ontology). The sink contract is the smallest piece of graph API
 * the emitter needs: upsert-node-by-QN, find-node-by-QN, insert-edge.
 *
 * Emit order is chosen so every reference resolves against nodes already
 * written: shared properties, interfaces, link types, actions and functions
 * first; object types next (they point at all of those); objects last (they
 * point at object types). Code bindings use find_by_qn against the fully
 * materialized graph — the pipeline pass runs after the resolve phases, and
 * live apply runs against a committed index.
 */
#include "ontology_emit.h"

#include "foundation/compat_fs.h"
#include "foundation/constants.h"
#include "foundation/log.h"
#include "foundation/str_util.h"
#include "store/store.h"

#include <yyjson/yyjson.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum {
    ONT_PROPS_MAX = 8192,
    ONT_EPROPS_MAX = 1024,
    ONT_MAX_FILE_BYTES = 4 * 1024 * 1024,
};

/* ── Sink interface ────────────────────────────────────────────── */

typedef struct {
    /* Upsert an element node; returns its graph id, or 0 on failure. */
    int64_t (*upsert_node)(void *ctx, const char *label, const char *name, const char *qn,
                           const char *file, const char *properties_json);
    /* Resolve a qualified name to a node id; 0 when absent. */
    int64_t (*find_by_qn)(void *ctx, const char *qn);
    /* Insert an edge; returns edge id or 0. */
    int64_t (*insert_edge)(void *ctx, int64_t src, int64_t dst, const char *type,
                           const char *properties_json);
    void *ctx;
} ont_sink_t;

static int64_t ont_gbuf_upsert(void *ctx, const char *label, const char *name, const char *qn,
                               const char *file, const char *properties_json) {
    return cbm_gbuf_upsert_node((cbm_gbuf_t *)ctx, label, name, qn, file, 0, 0,
                                properties_json ? properties_json : "{}");
}

static int64_t ont_gbuf_find(void *ctx, const char *qn) {
    const cbm_gbuf_node_t *n = cbm_gbuf_find_by_qn((cbm_gbuf_t *)ctx, qn);
    return n ? n->id : 0;
}

static int64_t ont_gbuf_edge(void *ctx, int64_t src, int64_t dst, const char *type,
                             const char *properties_json) {
    return cbm_gbuf_insert_edge((cbm_gbuf_t *)ctx, src, dst, type,
                                properties_json ? properties_json : "{}");
}

typedef struct {
    cbm_store_t *store;
    const char *project;
} ont_store_ctx_t;

static int64_t ont_store_upsert(void *vctx, const char *label, const char *name, const char *qn,
                                const char *file, const char *properties_json) {
    ont_store_ctx_t *c = (ont_store_ctx_t *)vctx;
    cbm_node_t n = {0};
    n.project = c->project;
    n.label = label;
    n.name = name;
    n.qualified_name = qn;
    n.file_path = file;
    n.start_line = 0;
    n.end_line = 0;
    n.properties_json = properties_json ? properties_json : "{}";
    return cbm_store_upsert_node(c->store, &n);
}

static int64_t ont_store_find(void *vctx, const char *qn) {
    ont_store_ctx_t *c = (ont_store_ctx_t *)vctx;
    cbm_node_t n;
    if (cbm_store_find_node_by_qn(c->store, c->project, qn, &n) != CBM_STORE_OK) {
        return 0;
    }
    return n.id;
}

static int64_t ont_store_edge(void *vctx, int64_t src, int64_t dst, const char *type,
                              const char *properties_json) {
    ont_store_ctx_t *c = (ont_store_ctx_t *)vctx;
    cbm_edge_t e = {0};
    e.project = c->project;
    e.source_id = src;
    e.target_id = dst;
    e.type = type;
    e.properties_json = properties_json ? properties_json : "{}";
    return cbm_store_insert_edge(c->store, &e);
}

/* ── JSON helpers ──────────────────────────────────────────────── */

/* Escape `src` into a fresh "\"...\"" JSON string; caller frees. */
static char *ont_jstr(const char *src) {
    size_t need = strlen(src) * 2 + 3;
    char *out = malloc(need + 64);
    if (!out) {
        return NULL;
    }
    out[0] = '"';
    int n = cbm_json_escape(out + 1, (int)need, src);
    if (n < 0) {
        free(out);
        return NULL;
    }
    out[1 + n] = '"';
    out[2 + n] = '\0';
    return out;
}

/* Build a JSON string array from char**; caller frees. Returns "[]" on empty. */
static char *ont_jarr_str(char **items, int count) {
    size_t cap = 4;
    for (int i = 0; i < count; i++) {
        cap += strlen(items[i]) * 2 + 4;
    }
    char *out = malloc(cap);
    if (!out) {
        return NULL;
    }
    int p = snprintf(out, cap, "[");
    for (int i = 0; i < count && p > 0 && (size_t)p < cap - 8; i++) {
        char *js = ont_jstr(items[i]);
        if (!js) {
            break;
        }
        p += snprintf(out + p, cap - (size_t)p, "%s%s", i ? "," : "", js);
        free(js);
    }
    snprintf(out + p, cap - (size_t)p, "]");
    return out;
}

/* Serialize inline (non-shared) properties of a prop list as a JSON object. */
static char *ont_props_json_inline(const cbm_onto_prop_t *props, int count) {
    yyjson_mut_doc *doc = yyjson_mut_doc_new(NULL);
    if (!doc) {
        return strdup("{}");
    }
    yyjson_mut_val *root = yyjson_mut_obj(doc);
    yyjson_mut_doc_set_root(doc, root);
    for (int i = 0; i < count; i++) {
        const cbm_onto_prop_t *pr = &props[i];
        if (pr->shared) {
            continue;
        }
        yyjson_mut_val *p = yyjson_mut_obj(doc);
        yyjson_mut_obj_add_str(doc, p, "type", pr->type ? pr->type : "string");
        if (pr->required) {
            yyjson_mut_obj_add_bool(doc, p, "required", true);
        }
        if (pr->bind) {
            yyjson_mut_obj_add_str(doc, p, "bind", pr->bind);
        }
        if (pr->description) {
            yyjson_mut_obj_add_str(doc, p, "description", pr->description);
        }
        if (pr->enum_count > 0) {
            yyjson_mut_val *vals = yyjson_mut_arr(doc);
            for (int j = 0; j < pr->enum_count; j++) {
                yyjson_mut_arr_add_str(doc, vals, pr->enum_values[j]);
            }
            yyjson_mut_obj_add_val(doc, p, "values", vals);
        }
        yyjson_mut_obj_add_val(doc, root, pr->name, p);
    }
    char *json = yyjson_mut_write(doc, 0, NULL);
    yyjson_mut_doc_free(doc);
    return json ? json : strdup("{}");
}

static char *ont_params_json(const cbm_onto_param_t *params, int count) {
    yyjson_mut_doc *doc = yyjson_mut_doc_new(NULL);
    if (!doc) {
        return strdup("{}");
    }
    yyjson_mut_val *root = yyjson_mut_obj(doc);
    yyjson_mut_doc_set_root(doc, root);
    for (int i = 0; i < count; i++) {
        const cbm_onto_param_t *pa = &params[i];
        yyjson_mut_val *p = yyjson_mut_obj(doc);
        yyjson_mut_obj_add_str(doc, p, "type", pa->type ? pa->type : "string");
        if (pa->required) {
            yyjson_mut_obj_add_bool(doc, p, "required", true);
        }
        if (pa->pattern) {
            yyjson_mut_obj_add_str(doc, p, "pattern", pa->pattern);
        }
        if (pa->description) {
            yyjson_mut_obj_add_str(doc, p, "description", pa->description);
        }
        if (pa->enum_count > 0) {
            yyjson_mut_val *vals = yyjson_mut_arr(doc);
            for (int j = 0; j < pa->enum_count; j++) {
                yyjson_mut_arr_add_str(doc, vals, pa->enum_values[j]);
            }
            yyjson_mut_obj_add_val(doc, p, "values", vals);
        }
        yyjson_mut_obj_add_val(doc, root, pa->name, p);
    }
    char *json = yyjson_mut_write(doc, 0, NULL);
    yyjson_mut_doc_free(doc);
    return json ? json : strdup("{}");
}

/* Attach a freshly serialized JSON subtree under `key` on mut-doc `root`. */
static void ont_attach_json(yyjson_mut_doc *doc, yyjson_mut_val *root, const char *key,
                            const char *json) {
    if (!json) {
        return;
    }
    yyjson_doc *parsed = yyjson_read(json, strlen(json), 0);
    yyjson_mut_val *v =
        parsed ? yyjson_val_mut_copy(doc, yyjson_doc_get_root(parsed)) : NULL;
    if (v) {
        yyjson_mut_obj_add_val(doc, root, key, v);
    }
    yyjson_doc_free(parsed);
}

static void ont_unresolved_add(cbm_ontology_unresolved_t *u, const char *from, const char *to) {
    if (!u) {
        return;
    }
    cbm_ontology_unresolved_ref_t *items =
        realloc(u->items, (size_t)(u->count + 1) * sizeof(*items));
    if (!items) {
        return;
    }
    u->items = items;
    u->items[u->count].from_qn = strdup(from);
    u->items[u->count].to_qn = strdup(to);
    if (u->items[u->count].from_qn && u->items[u->count].to_qn) {
        u->count++;
    } else {
        free(u->items[u->count].from_qn);
        free(u->items[u->count].to_qn);
    }
}

void cbm_ontology_unresolved_free(cbm_ontology_unresolved_t *u) {
    if (!u) {
        return;
    }
    for (int i = 0; i < u->count; i++) {
        free(u->items[i].from_qn);
        free(u->items[i].to_qn);
    }
    free(u->items);
    u->items = NULL;
    u->count = 0;
}

/* Upsert an element node under the ontology namespace. */
static int64_t ont_node(const ont_sink_t *sink, const char *project, const char *label,
                        const char *kind, const char *name, const char *properties_json) {
    char *qn = cbm_ontology_qn(project, kind, name);
    if (!qn) {
        return 0;
    }
    int64_t id = sink->upsert_node(sink->ctx, label, name, qn, CBM_ONTOLOGY_FILENAME,
                                   properties_json ? properties_json : "{}");
    free(qn);
    return id;
}

static int64_t ont_bind(const ont_sink_t *sink, int64_t from_id, const char *from_qn,
                        const char *to_qn, cbm_ontology_unresolved_t *u) {
    int64_t to_id = sink->find_by_qn(sink->ctx, to_qn);
    if (to_id == 0) {
        ont_unresolved_add(u, from_qn, to_qn);
        return 0;
    }
    return sink->insert_edge(sink->ctx, from_id, to_id, "ONT_BINDS", "{}");
}

/* ── Emit core ─────────────────────────────────────────────────── */

static int ont_emit_run(const ont_sink_t *sink, const cbm_ontology_def_t *def,
                        const char *project, cbm_ontology_unresolved_t *u) {
    /* root */
    char *root_qn = malloc(strlen(project) + 12);
    if (root_qn) {
        sprintf(root_qn, "%s.ontology", project);
        sink->upsert_node(sink->ctx, "Ontology", "ontology", root_qn, CBM_ONTOLOGY_FILENAME,
                          "{\"version\":1}");
        free(root_qn);
    }

    /* shared properties */
    for (int i = 0; i < def->shared_prop_count; i++) {
        const cbm_onto_prop_t *sp = &def->shared_props[i];
        char *tj = ont_jstr(sp->type ? sp->type : "string");
        char *dj = ont_jstr(sp->description ? sp->description : "");
        char buf[ONT_PROPS_MAX];
        snprintf(buf, sizeof(buf), "{\"type\":%s,\"description\":%s}", tj ? tj : "null",
                 dj ? dj : "null");
        free(tj);
        free(dj);
        ont_node(sink, project, "OntologySharedProperty", "shared-property", sp->name, buf);
    }

    /* interfaces */
    for (int i = 0; i < def->interface_count; i++) {
        const cbm_onto_interface_t *in = &def->interfaces[i];
        char *pj = ont_props_json_inline(in->props, in->prop_count);
        char *dj = ont_jstr(in->description ? in->description : "");
        char buf[ONT_PROPS_MAX];
        snprintf(buf, sizeof(buf), "{\"description\":%s,\"properties\":%s}",
                 dj ? dj : "null", pj ? pj : "{}");
        free(pj);
        free(dj);
        ont_node(sink, project, "OntologyInterface", "interface", in->name, buf);
    }

    /* link types */
    for (int i = 0; i < def->link_type_count; i++) {
        const cbm_onto_link_type_t *lt = &def->link_types[i];
        char buf[ONT_PROPS_MAX];
        char *sj = ont_jstr(lt->source ? lt->source : "");
        char *tj = ont_jstr(lt->target ? lt->target : "");
        char *cj = ont_jstr(lt->cardinality ? lt->cardinality : "");
        snprintf(buf, sizeof(buf), "{\"source\":%s,\"target\":%s,\"cardinality\":%s}",
                 sj ? sj : "null", tj ? tj : "null", cj ? cj : "null");
        free(sj);
        free(tj);
        free(cj);
        ont_node(sink, project, "OntologyLinkType", "link-type", lt->name, buf);
    }

    /* action types */
    for (int i = 0; i < def->action_count; i++) {
        const cbm_onto_action_t *a = &def->actions[i];
        char *pj = ont_params_json(a->params, a->param_count);
        char *tj = ont_jarr_str(a->targets, a->target_count);
        yyjson_mut_doc *doc = yyjson_mut_doc_new(NULL);
        if (doc) {
            yyjson_mut_val *root = yyjson_mut_obj(doc);
            yyjson_mut_doc_set_root(doc, root);
            if (a->description) {
                yyjson_mut_obj_add_str(doc, root, "description", a->description);
            }
            yyjson_mut_obj_add_bool(doc, root, "confirm", a->confirm);
            ont_attach_json(doc, root, "params", pj);
            ont_attach_json(doc, root, "targets", tj);
            if (a->has_exec) {
                yyjson_mut_val *ex = yyjson_mut_obj(doc);
                yyjson_mut_val *av = yyjson_mut_arr(doc);
                for (int j = 0; j < a->exec.argc; j++) {
                    yyjson_mut_arr_add_str(doc, av, a->exec.argv[j]);
                }
                yyjson_mut_obj_add_val(doc, ex, "argv", av);
                yyjson_mut_obj_add_str(doc, ex, "cwd", a->exec.cwd ? a->exec.cwd : ".");
                yyjson_mut_obj_add_int(doc, ex, "timeout_ms", a->exec.timeout_ms);
                yyjson_mut_obj_add_val(doc, root, "exec", ex);
            }
            if (a->builtin) {
                yyjson_mut_obj_add_str(doc, root, "builtin", a->builtin);
            }
            char *json = yyjson_mut_write(doc, 0, NULL);
            yyjson_mut_doc_free(doc);
            ont_node(sink, project, "OntologyActionType", "action-type", a->name,
                     json ? json : "{}");
            free(json);
        }
        free(pj);
        free(tj);
    }

    /* functions */
    for (int i = 0; i < def->function_count; i++) {
        const cbm_onto_function_t *f = &def->functions[i];
        char buf[ONT_PROPS_MAX];
        char *sj = ont_jstr(f->signature ? f->signature : "");
        char *dj = ont_jstr(f->description ? f->description : "");
        snprintf(buf, sizeof(buf), "{\"signature\":%s,\"description\":%s}", sj ? sj : "null",
                 dj ? dj : "null");
        free(sj);
        free(dj);
        int64_t id = ont_node(sink, project, "OntologyFunction", "function", f->name, buf);
        if (id > 0 && f->bind) {
            char *fqn = cbm_ontology_qn(project, "function", f->name);
            if (fqn) {
                ont_bind(sink, id, fqn, f->bind, u);
                free(fqn);
            }
        }
    }

    /* object types */
    for (int i = 0; i < def->object_type_count; i++) {
        const cbm_onto_object_type_t *t = &def->object_types[i];
        char *pj = ont_props_json_inline(t->props, t->prop_count);
        char *ij = ont_jarr_str(t->implements, t->implements_count);
        char *bj = ont_jarr_str(t->binds, t->binds_count);
        char *dj = ont_jstr(t->description ? t->description : "");
        char buf[ONT_PROPS_MAX];
        snprintf(buf, sizeof(buf),
                 "{\"description\":%s,\"implements\":%s,\"binds\":%s,"
                 "\"properties\":%s}",
                 dj ? dj : "null", ij ? ij : "[]", bj ? bj : "[]", pj ? pj : "{}");
        free(pj);
        free(ij);
        free(bj);
        free(dj);
        int64_t id = ont_node(sink, project, "OntologyObjectType", "object-type", t->name, buf);
        if (id <= 0) {
            continue;
        }
        char *tqn = cbm_ontology_qn(project, "object-type", t->name);
        /* ONT_IMPLEMENTS edges */
        for (int j = 0; j < t->implements_count; j++) {
            char *iqn = cbm_ontology_qn(project, "interface", t->implements[j]);
            int64_t iid = iqn ? sink->find_by_qn(sink->ctx, iqn) : 0;
            if (iid > 0) {
                sink->insert_edge(sink->ctx, id, iid, "ONT_IMPLEMENTS", "{}");
            }
            free(iqn);
        }
        /* ONT_BINDS edges to code nodes */
        for (int j = 0; j < t->binds_count; j++) {
            ont_bind(sink, id, tqn, t->binds[j], u);
        }
        /* property bindings + shared property edges */
        for (int j = 0; j < t->prop_count; j++) {
            const cbm_onto_prop_t *pr = &t->props[j];
            if (pr->shared) {
                char *sqn = cbm_ontology_qn(project, "shared-property", pr->name);
                int64_t sid = sqn ? sink->find_by_qn(sink->ctx, sqn) : 0;
                if (sid > 0) {
                    sink->insert_edge(sink->ctx, id, sid, "ONT_HAS_PROPERTY", "{}");
                }
                free(sqn);
            } else if (pr->bind) {
                ont_bind(sink, id, tqn, pr->bind, u);
            }
        }
        /* ONT_LINKS edges to target object types */
        for (int j = 0; j < t->link_count; j++) {
            const cbm_onto_link_decl_t *l = &t->links[j];
            char *dqn = cbm_ontology_qn(project, "object-type", l->target);
            int64_t did = dqn ? sink->find_by_qn(sink->ctx, dqn) : 0;
            if (did > 0) {
                char ep[ONT_EPROPS_MAX];
                char *tj2 = ont_jstr(l->type);
                char *cj2 = ont_jstr(l->cardinality ? l->cardinality : "");
                snprintf(ep, sizeof(ep), "{\"type\":%s,\"cardinality\":%s}", tj2 ? tj2 : "null",
                         cj2 ? cj2 : "null");
                free(tj2);
                free(cj2);
                sink->insert_edge(sink->ctx, id, did, "ONT_LINKS", ep);
            }
            free(dqn);
        }
        /* ONT_HAS_ACTION edges */
        for (int j = 0; j < t->action_count; j++) {
            char *aqn = cbm_ontology_qn(project, "action-type", t->actions[j]);
            int64_t aid = aqn ? sink->find_by_qn(sink->ctx, aqn) : 0;
            if (aid > 0) {
                sink->insert_edge(sink->ctx, id, aid, "ONT_HAS_ACTION", "{}");
            }
            free(aqn);
        }
        /* ONT_HAS_FUNCTION edges */
        for (int j = 0; j < t->function_count; j++) {
            char *fqn = cbm_ontology_qn(project, "function", t->functions[j]);
            int64_t fid = fqn ? sink->find_by_qn(sink->ctx, fqn) : 0;
            if (fid > 0) {
                sink->insert_edge(sink->ctx, id, fid, "ONT_HAS_FUNCTION", "{}");
            }
            free(fqn);
        }
        free(tqn);
    }

    /* standalone link-type declarations produce ONT_LINKS too */
    for (int i = 0; i < def->link_type_count; i++) {
        const cbm_onto_link_type_t *lt = &def->link_types[i];
        char *sqn = cbm_ontology_qn(project, "object-type", lt->source);
        char *dqn = cbm_ontology_qn(project, "object-type", lt->target);
        int64_t sid = sqn ? sink->find_by_qn(sink->ctx, sqn) : 0;
        int64_t did = dqn ? sink->find_by_qn(sink->ctx, dqn) : 0;
        if (sid > 0 && did > 0) {
            char ep[ONT_EPROPS_MAX];
            char *tj = ont_jstr(lt->name);
            char *cj = ont_jstr(lt->cardinality ? lt->cardinality : "");
            snprintf(ep, sizeof(ep), "{\"type\":%s,\"cardinality\":%s}", tj ? tj : "null",
                     cj ? cj : "null");
            free(tj);
            free(cj);
            sink->insert_edge(sink->ctx, sid, did, "ONT_LINKS", ep);
        }
        free(sqn);
        free(dqn);
    }

    /* objects (instances) */
    for (int i = 0; i < def->object_count; i++) {
        const cbm_onto_object_t *o = &def->objects[i];
        char *tj = ont_jstr(o->type ? o->type : "");
        char buf[ONT_PROPS_MAX];
        snprintf(buf, sizeof(buf), "{\"type\":%s,\"values\":%s}", tj ? tj : "null",
                 o->props_json ? o->props_json : "{}");
        free(tj);
        int64_t id = ont_node(sink, project, "OntologyObject", "object", o->name, buf);
        if (id <= 0) {
            continue;
        }
        char *oqn = cbm_ontology_qn(project, "object", o->name);
        /* instance-of edge */
        char *tqn = cbm_ontology_qn(project, "object-type", o->type);
        int64_t tid = tqn ? sink->find_by_qn(sink->ctx, tqn) : 0;
        if (tid > 0) {
            sink->insert_edge(sink->ctx, id, tid, "ONT_INSTANCE_OF", "{}");
        }
        free(tqn);
        if (o->bind && oqn) {
            ont_bind(sink, id, oqn, o->bind, u);
        }
        free(oqn);
    }

    return 0;
}

/* ── Public entry points ───────────────────────────────────────── */

int cbm_ontology_emit(cbm_gbuf_t *gb, const cbm_ontology_def_t *def, const char *project,
                      cbm_ontology_unresolved_t *unresolved) {
    if (!gb || !def || !project) {
        return -1;
    }
    ont_sink_t sink = {
        .upsert_node = ont_gbuf_upsert,
        .find_by_qn = ont_gbuf_find,
        .insert_edge = ont_gbuf_edge,
        .ctx = gb,
    };
    return ont_emit_run(&sink, def, project, unresolved);
}

int cbm_ontology_emit_store(cbm_store_t *store, const cbm_ontology_def_t *def,
                            const char *project, cbm_ontology_unresolved_t *unresolved) {
    if (!store || !def || !project) {
        return -1;
    }
    ont_store_ctx_t sc = {.store = store, .project = project};
    ont_sink_t sink = {
        .upsert_node = ont_store_upsert,
        .find_by_qn = ont_store_find,
        .insert_edge = ont_store_edge,
        .ctx = &sc,
    };
    return ont_emit_run(&sink, def, project, unresolved);
}

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

cbm_ontology_def_t *cbm_ontology_load_file(const char *repo_path, const char *project,
                                           cbm_ontology_errors_t *errors) {
    if (!repo_path || !project) {
        return NULL;
    }
    char path[CBM_SZ_4K];
    int n = snprintf(path, sizeof(path), "%s/%s", repo_path, CBM_ONTOLOGY_FILENAME);
    if (n <= 0 || (size_t)n >= sizeof(path)) {
        return NULL;
    }
    size_t len = 0;
    char *text = ont_read_file(path, &len);
    if (!text) {
        return NULL;
    }
    cbm_ontology_def_t *def = cbm_ontology_def_parse(text, (int)len, project, errors);
    free(text);
    return def;
}

int cbm_ontology_apply_store(cbm_store_t *store, const char *repo_path, const char *project,
                             cbm_ontology_errors_t *errors,
                             cbm_ontology_unresolved_t *unresolved) {
    if (!store || !repo_path || !project) {
        return -1;
    }
    cbm_ontology_def_t *def = cbm_ontology_load_file(repo_path, project, errors);
    if (!def) {
        return CBM_ONTOLOGY_APPLY_NO_FILE;
    }
    if (errors && errors->count > 0) {
        cbm_ontology_def_free(def);
        return CBM_ONTOLOGY_APPLY_INVALID;
    }
    /* Old elements disappear together: every ontology node carries the
     * definition file as file_path, and edge delete cascades on node id. */
    if (cbm_store_delete_nodes_by_file(store, project, CBM_ONTOLOGY_FILENAME) != CBM_STORE_OK) {
        cbm_ontology_def_free(def);
        return CBM_ONTOLOGY_APPLY_STORE_ERR;
    }
    int rc = cbm_ontology_emit_store(store, def, project, unresolved);
    cbm_ontology_def_free(def);
    if (rc != 0) {
        return CBM_ONTOLOGY_APPLY_STORE_ERR;
    }
    cbm_log_info("ontology.apply", "project", project);
    return CBM_ONTOLOGY_APPLY_OK;
}
