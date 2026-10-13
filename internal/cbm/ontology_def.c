/*
 * ontology_def.c — parser/validator for .cbm-ontology.json.
 *
 * The file is deliberately boring JSON (no anchors, no nesting tricks) so a
 * plain yyjson walk covers it. Validation happens in two layers:
 *
 *   1. shape — every map/array/scalar is the type the schema says; a bad
 *      member is recorded and skipped, the rest still parses;
 *   2. references — a second pass resolves every name a member points at
 *      (implements -> interface, link.target -> object type, actions[] ->
 *      action type, ...) and records the ones that dangle.
 *
 * Emitters consume the def and skip members the error list flagged; the
 * same list becomes the coverage report so `index_status` can show an
 * ontology that needs attention without failing the index.
 */
#include "ontology_def.h"

#include "foundation/constants.h"

#include <yyjson/yyjson.h>

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum {
    ONT_NAME_MAX = 128,
    ONT_ERR_INITIAL_CAP = 16,
};

typedef struct {
    cbm_ontology_errors_t *errors;
    const char *project;
} ont_ctx_t;

static void ont_err(ont_ctx_t *ctx, const char *path, const char *detail) {
    if (!ctx->errors) {
        return;
    }
    cbm_ontology_errors_t *e = ctx->errors;
    char **items = realloc(e->items, (size_t)(e->count + 1) * sizeof(*items));
    if (!items) {
        return;
    }
    e->items = items;
    char buf[CBM_SZ_1K];
    snprintf(buf, sizeof(buf), "%s: %s", path ? path : "?", detail ? detail : "invalid");
    e->items[e->count] = strdup(buf);
    if (e->items[e->count]) {
        e->count++;
    }
}

static char *ont_strdup(const char *s) { return s ? strdup(s) : NULL; }

static const char *ont_str(yyjson_val *v) { return yyjson_is_str(v) ? yyjson_get_str(v) : NULL; }

static bool ont_name_ok(const char *s) {
    if (!s || !s[0] || strlen(s) > ONT_NAME_MAX) {
        return false;
    }
    for (const char *p = s; *p; p++) {
        if (!(isalnum((unsigned char)*p) || *p == '_' || *p == '-' || *p == ' ')) {
            return false;
        }
    }
    return true;
}

static bool ont_prop_type_ok(const char *t) {
    return t && (strcmp(t, "string") == 0 || strcmp(t, "number") == 0 ||
                 strcmp(t, "boolean") == 0 || strcmp(t, "enum") == 0);
}

static bool ont_cardinality_ok(const char *c) {
    return !c || strcmp(c, "one") == 0 || strcmp(c, "many") == 0;
}

static char **ont_str_array(yyjson_val *arr, int *count, ont_ctx_t *ctx, const char *path) {
    *count = 0;
    if (!arr) {
        return NULL;
    }
    if (!yyjson_is_arr(arr)) {
        ont_err(ctx, path, "expected an array");
        return NULL;
    }
    size_t n = yyjson_arr_size(arr);
    char **out = calloc(n + 1, sizeof(*out));
    if (!out) {
        return NULL;
    }
    size_t i, max;
    yyjson_val *item;
    int w = 0;
    yyjson_arr_foreach(arr, i, max, item) {
        const char *s = ont_str(item);
        if (!s) {
            char p[CBM_SZ_1K];
            snprintf(p, sizeof(p), "%s[%zu]", path, i);
            ont_err(ctx, p, "expected a string");
            continue;
        }
        out[w++] = strdup(s);
    }
    *count = w;
    return out;
}

static void ont_free_str_array(char **arr, int count) {
    if (!arr) {
        return;
    }
    for (int i = 0; i < count; i++) {
        free(arr[i]);
    }
    free(arr);
}

static void ont_props_free(cbm_onto_prop_t *props, int count) {
    if (!props) {
        return;
    }
    for (int i = 0; i < count; i++) {
        free(props[i].name);
        free(props[i].type);
        free(props[i].bind);
        free(props[i].description);
        ont_free_str_array(props[i].enum_values, props[i].enum_count);
    }
    free(props);
}

static void ont_links_free(cbm_onto_link_decl_t *links, int count) {
    if (!links) {
        return;
    }
    for (int i = 0; i < count; i++) {
        free(links[i].type);
        free(links[i].target);
        free(links[i].cardinality);
    }
    free(links);
}

static cbm_onto_prop_t *ont_parse_props(yyjson_val *obj, int *count, ont_ctx_t *ctx,
                                        const char *path) {
    *count = 0;
    if (!obj) {
        return NULL;
    }
    if (!yyjson_is_obj(obj)) {
        ont_err(ctx, path, "expected an object of property definitions");
        return NULL;
    }
    size_t n = yyjson_obj_size(obj);
    cbm_onto_prop_t *props = calloc(n, sizeof(*props));
    if (!props) {
        return NULL;
    }
    int w = 0;
    size_t i, max;
    yyjson_val *key, *val;
    yyjson_obj_foreach(obj, i, max, key, val) {
        const char *name = ont_str(key);
        char p[CBM_SZ_1K];
        snprintf(p, sizeof(p), "%s.%s", path, name ? name : "?");
        if (!name || !ont_name_ok(name)) {
            ont_err(ctx, p, "invalid property name");
            continue;
        }
        if (!yyjson_is_obj(val)) {
            ont_err(ctx, p, "property must be an object");
            continue;
        }
        cbm_onto_prop_t *pr = &props[w];
        memset(pr, 0, sizeof(*pr));
        pr->name = strdup(name);
        pr->shared = yyjson_get_bool(yyjson_obj_get(val, "shared"));
        pr->required = yyjson_get_bool(yyjson_obj_get(val, "required"));
        pr->type = ont_strdup(ont_str(yyjson_obj_get(val, "type")));
        pr->bind = ont_strdup(ont_str(yyjson_obj_get(val, "bind")));
        pr->description = ont_strdup(ont_str(yyjson_obj_get(val, "description")));
        pr->enum_values =
            ont_str_array(yyjson_obj_get(val, "values"), &pr->enum_count, ctx, p);
        if (pr->shared) {
            free(pr->type);
            pr->type = NULL;
        } else if (!ont_prop_type_ok(pr->type)) {
            ont_err(ctx, p, "type must be string|number|boolean|enum (or shared:true)");
            ont_props_free(&props[w], 1);
            continue;
        }
        if (pr->type && strcmp(pr->type, "enum") == 0 && pr->enum_count == 0) {
            ont_err(ctx, p, "enum type requires a values array");
        }
        w++;
    }
    *count = w;
    return props;
}

static cbm_onto_param_t *ont_parse_params(yyjson_val *obj, int *count, ont_ctx_t *ctx,
                                          const char *path) {
    *count = 0;
    if (!obj) {
        return NULL;
    }
    if (!yyjson_is_obj(obj)) {
        ont_err(ctx, path, "expected an object of parameters");
        return NULL;
    }
    size_t n = yyjson_obj_size(obj);
    cbm_onto_param_t *params = calloc(n, sizeof(*params));
    if (!params) {
        return NULL;
    }
    int w = 0;
    size_t i, max;
    yyjson_val *key, *val;
    yyjson_obj_foreach(obj, i, max, key, val) {
        const char *name = ont_str(key);
        char p[CBM_SZ_1K];
        snprintf(p, sizeof(p), "%s.%s", path, name ? name : "?");
        if (!name || !ont_name_ok(name)) {
            ont_err(ctx, p, "invalid parameter name");
            continue;
        }
        if (!yyjson_is_obj(val)) {
            ont_err(ctx, p, "parameter must be an object");
            continue;
        }
        cbm_onto_param_t *pa = &params[w];
        memset(pa, 0, sizeof(*pa));
        pa->name = strdup(name);
        pa->type = ont_strdup(ont_str(yyjson_obj_get(val, "type")));
        pa->description = ont_strdup(ont_str(yyjson_obj_get(val, "description")));
        pa->pattern = ont_strdup(ont_str(yyjson_obj_get(val, "pattern")));
        pa->required = yyjson_get_bool(yyjson_obj_get(val, "required"));
        pa->enum_values =
            ont_str_array(yyjson_obj_get(val, "values"), &pa->enum_count, ctx, p);
        if (!pa->type) {
            pa->type = strdup("string"); /* params default to string */
        }
        if (!ont_prop_type_ok(pa->type)) {
            ont_err(ctx, p, "parameter type must be string|number|boolean|enum");
        }
        if (strcmp(pa->type, "enum") == 0 && pa->enum_count == 0) {
            ont_err(ctx, p, "enum parameter requires a values array");
        }
        w++;
    }
    *count = w;
    return params;
}

static void ont_params_free(cbm_onto_param_t *params, int count) {
    if (!params) {
        return;
    }
    for (int i = 0; i < count; i++) {
        free(params[i].name);
        free(params[i].type);
        free(params[i].description);
        free(params[i].pattern);
        ont_free_str_array(params[i].enum_values, params[i].enum_count);
    }
    free(params);
}

static cbm_onto_link_decl_t *ont_parse_links(yyjson_val *arr, int *count, ont_ctx_t *ctx,
                                             const char *path) {
    *count = 0;
    if (!arr) {
        return NULL;
    }
    if (!yyjson_is_arr(arr)) {
        ont_err(ctx, path, "expected an array of link declarations");
        return NULL;
    }
    size_t n = yyjson_arr_size(arr);
    cbm_onto_link_decl_t *links = calloc(n, sizeof(*links));
    if (!links) {
        return NULL;
    }
    int w = 0;
    size_t i, max;
    yyjson_val *item;
    yyjson_arr_foreach(arr, i, max, item) {
        char p[CBM_SZ_1K];
        snprintf(p, sizeof(p), "%s[%zu]", path, i);
        if (!yyjson_is_obj(item)) {
            ont_err(ctx, p, "link must be an object {type,target}");
            continue;
        }
        cbm_onto_link_decl_t *l = &links[w];
        memset(l, 0, sizeof(*l));
        l->type = ont_strdup(ont_str(yyjson_obj_get(item, "type")));
        l->target = ont_strdup(ont_str(yyjson_obj_get(item, "target")));
        l->cardinality = ont_strdup(ont_str(yyjson_obj_get(item, "cardinality")));
        if (!l->type || !l->target) {
            ont_err(ctx, p, "link requires type and target");
            ont_links_free(&links[w], 1);
            continue;
        }
        if (!ont_cardinality_ok(l->cardinality)) {
            ont_err(ctx, p, "cardinality must be \"one\" or \"many\"");
        }
        w++;
    }
    *count = w;
    return links;
}

static void ont_exec_free(cbm_onto_exec_t *exec) {
    if (!exec) {
        return;
    }
    ont_free_str_array(exec->argv, exec->argc);
    free(exec->cwd);
    ont_free_str_array(exec->env, exec->env_count);
}

static bool ont_parse_exec(yyjson_val *obj, cbm_onto_exec_t *out, ont_ctx_t *ctx,
                           const char *path) {
    memset(out, 0, sizeof(*out));
    if (!yyjson_is_obj(obj)) {
        ont_err(ctx, path, "exec must be an object {argv:[...]}");
        return false;
    }
    out->argv = ont_str_array(yyjson_obj_get(obj, "argv"), &out->argc, ctx, path);
    if (out->argc == 0) {
        ont_err(ctx, path, "exec.argv must be a non-empty string array");
        ont_exec_free(out);
        memset(out, 0, sizeof(*out));
        return false;
    }
    out->cwd = ont_strdup(ont_str(yyjson_obj_get(obj, "cwd")));
    if (!out->cwd) {
        out->cwd = strdup(".");
    }
    if (out->cwd[0] == '/' || strstr(out->cwd, "..")) {
        ont_err(ctx, path, "exec.cwd must be a path inside the repo (no absolute paths, no ..)");
    }
    yyjson_val *t = yyjson_obj_get(obj, "timeout_ms");
    out->timeout_ms = yyjson_is_num(t) ? (int)yyjson_get_sint(t) : 30000;
    if (out->timeout_ms <= 0 || out->timeout_ms > 600000) {
        ont_err(ctx, path, "timeout_ms must be 1..600000");
    }
    yyjson_val *env = yyjson_obj_get(obj, "env");
    if (env) {
        if (!yyjson_is_obj(env)) {
            ont_err(ctx, path, "exec.env must be an object of string values");
        } else {
            size_t n = yyjson_obj_size(env);
            out->env = calloc(n + 1, sizeof(*out->env));
            if (out->env) {
                size_t i, max;
                yyjson_val *key, *val;
                yyjson_obj_foreach(env, i, max, key, val) {
                    const char *k = ont_str(key);
                    const char *v = ont_str(val);
                    if (!k || !v || !k[0] || strchr(k, '=')) {
                        ont_err(ctx, path, "env entries need a NAME key and string value");
                        continue;
                    }
                    char *entry = malloc(strlen(k) + strlen(v) + 2);
                    if (entry) {
                        sprintf(entry, "%s=%s", k, v);
                        out->env[out->env_count++] = entry;
                    }
                }
            }
        }
    }
    return true;
}

static void ont_action_free(cbm_onto_action_t *a) {
    if (!a) {
        return;
    }
    free(a->name);
    free(a->description);
    ont_params_free(a->params, a->param_count);
    ont_free_str_array(a->targets, a->target_count);
    ont_exec_free(&a->exec);
    free(a->builtin);
}

cbm_ontology_def_t *cbm_ontology_def_parse(const char *json_text, int json_len,
                                           const char *project, cbm_ontology_errors_t *errors) {
    if (errors) {
        errors->items = NULL;
        errors->count = 0;
    }
    ont_ctx_t ctx = {.errors = errors, .project = project};

    yyjson_doc *doc = yyjson_read(json_text, json_len < 0 ? (int)strlen(json_text) : json_len, 0);
    if (!doc) {
        ont_err(&ctx, "$", "not valid JSON");
        return NULL;
    }
    yyjson_val *root = yyjson_doc_get_root(doc);
    if (!yyjson_is_obj(root)) {
        ont_err(&ctx, "$", "top level must be an object");
        yyjson_doc_free(doc);
        return NULL;
    }

    yyjson_val *ver = yyjson_obj_get(root, "version");
    if (ver && (!yyjson_is_num(ver) || yyjson_get_sint(ver) != CBM_ONTOLOGY_VERSION)) {
        ont_err(&ctx, "version", "unsupported version (expected 1)");
    }

    cbm_ontology_def_t *def = calloc(1, sizeof(*def));
    if (!def) {
        yyjson_doc_free(doc);
        return NULL;
    }

    /* ── shared_properties / interfaces / object_types / objects ── */
    def->shared_props =
        ont_parse_props(yyjson_obj_get(root, "shared_properties"), &def->shared_prop_count, &ctx,
                        "shared_properties");

    yyjson_val *ifaces = yyjson_obj_get(root, "interfaces");
    if (ifaces && !yyjson_is_obj(ifaces)) {
        ont_err(&ctx, "interfaces", "expected an object");
    } else if (ifaces) {
        size_t n = yyjson_obj_size(ifaces);
        def->interfaces = calloc(n, sizeof(*def->interfaces));
        size_t i, max;
        yyjson_val *key, *val;
        yyjson_obj_foreach(ifaces, i, max, key, val) {
            const char *name = ont_str(key);
            char p[CBM_SZ_1K];
            snprintf(p, sizeof(p), "interfaces.%s", name ? name : "?");
            if (!name || !ont_name_ok(name) || !yyjson_is_obj(val)) {
                ont_err(&ctx, p, "invalid interface");
                continue;
            }
            cbm_onto_interface_t *in = &def->interfaces[def->interface_count];
            memset(in, 0, sizeof(*in));
            in->name = strdup(name);
            in->description = ont_strdup(ont_str(yyjson_obj_get(val, "description")));
            in->props = ont_parse_props(yyjson_obj_get(val, "properties"), &in->prop_count, &ctx, p);
            in->links = ont_parse_links(yyjson_obj_get(val, "links"), &in->link_count, &ctx, p);
            def->interface_count++;
        }
    }

    yyjson_val *types = yyjson_obj_get(root, "object_types");
    if (types && !yyjson_is_obj(types)) {
        ont_err(&ctx, "object_types", "expected an object");
    } else if (types) {
        size_t n = yyjson_obj_size(types);
        def->object_types = calloc(n, sizeof(*def->object_types));
        size_t i, max;
        yyjson_val *key, *val;
        yyjson_obj_foreach(types, i, max, key, val) {
            const char *name = ont_str(key);
            char p[CBM_SZ_1K];
            snprintf(p, sizeof(p), "object_types.%s", name ? name : "?");
            if (!name || !ont_name_ok(name) || !yyjson_is_obj(val)) {
                ont_err(&ctx, p, "invalid object type");
                continue;
            }
            cbm_onto_object_type_t *t = &def->object_types[def->object_type_count];
            memset(t, 0, sizeof(*t));
            t->name = strdup(name);
            t->description = ont_strdup(ont_str(yyjson_obj_get(val, "description")));
            t->implements = ont_str_array(yyjson_obj_get(val, "implements"),
                                          &t->implements_count, &ctx, p);
            t->binds =
                ont_str_array(yyjson_obj_get(val, "binds"), &t->binds_count, &ctx, p);
            t->props = ont_parse_props(yyjson_obj_get(val, "properties"), &t->prop_count, &ctx, p);
            t->links = ont_parse_links(yyjson_obj_get(val, "links"), &t->link_count, &ctx, p);
            t->actions =
                ont_str_array(yyjson_obj_get(val, "actions"), &t->action_count, &ctx, p);
            t->functions = ont_str_array(yyjson_obj_get(val, "functions"), &t->function_count,
                                          &ctx, p);
            def->object_type_count++;
        }
    }

    yyjson_val *objs = yyjson_obj_get(root, "objects");
    if (objs && !yyjson_is_obj(objs)) {
        ont_err(&ctx, "objects", "expected an object");
    } else if (objs) {
        size_t n = yyjson_obj_size(objs);
        def->objects = calloc(n, sizeof(*def->objects));
        size_t i, max;
        yyjson_val *key, *val;
        yyjson_obj_foreach(objs, i, max, key, val) {
            const char *name = ont_str(key);
            char p[CBM_SZ_1K];
            snprintf(p, sizeof(p), "objects.%s", name ? name : "?");
            if (!name || !ont_name_ok(name) || !yyjson_is_obj(val)) {
                ont_err(&ctx, p, "invalid object");
                continue;
            }
            cbm_onto_object_t *o = &def->objects[def->object_count];
            memset(o, 0, sizeof(*o));
            o->name = strdup(name);
            o->type = ont_strdup(ont_str(yyjson_obj_get(val, "type")));
            o->bind = ont_strdup(ont_str(yyjson_obj_get(val, "bind")));
            yyjson_val *props = yyjson_obj_get(val, "properties");
            if (props && yyjson_is_obj(props)) {
                o->props_json = yyjson_val_write(props, 0, NULL);
            }
            if (!o->type) {
                ont_err(&ctx, p, "object requires a type");
                free(o->name);
                free(o->props_json);
                free(o->bind);
                memset(o, 0, sizeof(*o));
                continue;
            }
            def->object_count++;
        }
    }

    yyjson_val *lts = yyjson_obj_get(root, "link_types");
    if (lts && !yyjson_is_obj(lts)) {
        ont_err(&ctx, "link_types", "expected an object");
    } else if (lts) {
        size_t n = yyjson_obj_size(lts);
        def->link_types = calloc(n, sizeof(*def->link_types));
        size_t i, max;
        yyjson_val *key, *val;
        yyjson_obj_foreach(lts, i, max, key, val) {
            const char *name = ont_str(key);
            char p[CBM_SZ_1K];
            snprintf(p, sizeof(p), "link_types.%s", name ? name : "?");
            if (!name || !ont_name_ok(name) || !yyjson_is_obj(val)) {
                ont_err(&ctx, p, "invalid link type");
                continue;
            }
            cbm_onto_link_type_t *lt = &def->link_types[def->link_type_count];
            memset(lt, 0, sizeof(*lt));
            lt->name = strdup(name);
            lt->description = ont_strdup(ont_str(yyjson_obj_get(val, "description")));
            lt->source = ont_strdup(ont_str(yyjson_obj_get(val, "source")));
            lt->target = ont_strdup(ont_str(yyjson_obj_get(val, "target")));
            lt->cardinality = ont_strdup(ont_str(yyjson_obj_get(val, "cardinality")));
            if (!lt->source || !lt->target) {
                ont_err(&ctx, p, "link type requires source and target");
            }
            if (!ont_cardinality_ok(lt->cardinality)) {
                ont_err(&ctx, p, "cardinality must be \"one\" or \"many\"");
            }
            def->link_type_count++;
        }
    }

    yyjson_val *acts = yyjson_obj_get(root, "action_types");
    if (acts && !yyjson_is_obj(acts)) {
        ont_err(&ctx, "action_types", "expected an object");
    } else if (acts) {
        size_t n = yyjson_obj_size(acts);
        def->actions = calloc(n, sizeof(*def->actions));
        size_t i, max;
        yyjson_val *key, *val;
        yyjson_obj_foreach(acts, i, max, key, val) {
            const char *name = ont_str(key);
            char p[CBM_SZ_1K];
            snprintf(p, sizeof(p), "action_types.%s", name ? name : "?");
            if (!name || !ont_name_ok(name) || !yyjson_is_obj(val)) {
                ont_err(&ctx, p, "invalid action type");
                continue;
            }
            cbm_onto_action_t *a = &def->actions[def->action_count];
            memset(a, 0, sizeof(*a));
            a->name = strdup(name);
            a->description = ont_strdup(ont_str(yyjson_obj_get(val, "description")));
            a->confirm = yyjson_get_bool(yyjson_obj_get(val, "confirm"));
            a->params =
                ont_parse_params(yyjson_obj_get(val, "params"), &a->param_count, &ctx, p);
            a->targets =
                ont_str_array(yyjson_obj_get(val, "targets"), &a->target_count, &ctx, p);
            yyjson_val *builtin = yyjson_obj_get(val, "builtin");
            a->builtin = ont_strdup(ont_str(builtin));
            yyjson_val *exec = yyjson_obj_get(val, "exec");
            if (exec) {
                a->has_exec = ont_parse_exec(exec, &a->exec, &ctx, p);
            }
            if (a->builtin && exec) {
                ont_err(&ctx, p, "an action may declare exec or builtin, not both");
            }
            if (a->builtin && strcmp(a->builtin, "reindex") != 0) {
                ont_err(&ctx, p, "unknown builtin (supported: reindex)");
            }
            if (!a->has_exec && !a->builtin) {
                /* declarative-only actions are legal — they carry metadata */
            }
            def->action_count++;
        }
    }

    yyjson_val *fns = yyjson_obj_get(root, "functions");
    if (fns && !yyjson_is_obj(fns)) {
        ont_err(&ctx, "functions", "expected an object");
    } else if (fns) {
        size_t n = yyjson_obj_size(fns);
        def->functions = calloc(n, sizeof(*def->functions));
        size_t i, max;
        yyjson_val *key, *val;
        yyjson_obj_foreach(fns, i, max, key, val) {
            const char *name = ont_str(key);
            char p[CBM_SZ_1K];
            snprintf(p, sizeof(p), "functions.%s", name ? name : "?");
            if (!name || !ont_name_ok(name) || !yyjson_is_obj(val)) {
                ont_err(&ctx, p, "invalid function");
                continue;
            }
            cbm_onto_function_t *f = &def->functions[def->function_count];
            memset(f, 0, sizeof(*f));
            f->name = strdup(name);
            f->description = ont_strdup(ont_str(yyjson_obj_get(val, "description")));
            f->signature = ont_strdup(ont_str(yyjson_obj_get(val, "signature")));
            f->bind = ont_strdup(ont_str(yyjson_obj_get(val, "bind")));
            def->function_count++;
        }
    }

    yyjson_doc_free(doc);

    /* ── reference pass ── */
    for (int i = 0; i < def->object_type_count; i++) {
        const cbm_onto_object_type_t *t = &def->object_types[i];
        for (int j = 0; j < t->implements_count; j++) {
            if (!cbm_ontology_find_interface(def, t->implements[j])) {
                char p[CBM_SZ_1K];
                snprintf(p, sizeof(p), "object_types.%s.implements[%d]", t->name, j);
                ont_err(&ctx, p, "unknown interface");
            }
        }
        for (int j = 0; j < t->action_count; j++) {
            if (!cbm_ontology_find_action(def, t->actions[j])) {
                char p[CBM_SZ_1K];
                snprintf(p, sizeof(p), "object_types.%s.actions[%d]", t->name, j);
                ont_err(&ctx, p, "unknown action type");
            }
        }
        for (int j = 0; j < t->function_count; j++) {
            bool found = false;
            for (int k = 0; k < def->function_count; k++) {
                if (strcmp(def->functions[k].name, t->functions[j]) == 0) {
                    found = true;
                    break;
                }
            }
            if (!found) {
                char p[CBM_SZ_1K];
                snprintf(p, sizeof(p), "object_types.%s.functions[%d]", t->name, j);
                ont_err(&ctx, p, "unknown function");
            }
        }
        for (int j = 0; j < t->prop_count; j++) {
            const cbm_onto_prop_t *pr = &t->props[j];
            if (pr->shared) {
                bool found = false;
                for (int k = 0; k < def->shared_prop_count; k++) {
                    if (strcmp(def->shared_props[k].name, pr->name) == 0) {
                        found = true;
                        break;
                    }
                }
                if (!found) {
                    char p[CBM_SZ_1K];
                    snprintf(p, sizeof(p), "object_types.%s.properties.%s", t->name, pr->name);
                    ont_err(&ctx, p, "shared property not declared in shared_properties");
                }
            }
        }
    }
    for (int i = 0; i < def->object_count; i++) {
        const cbm_onto_object_t *o = &def->objects[i];
        if (!cbm_ontology_find_object_type(def, o->type)) {
            char p[CBM_SZ_1K];
            snprintf(p, sizeof(p), "objects.%s.type", o->name);
            ont_err(&ctx, p, "unknown object type");
        }
    }
    for (int i = 0; i < def->action_count; i++) {
        const cbm_onto_action_t *a = &def->actions[i];
        for (int j = 0; j < a->target_count; j++) {
            if (!cbm_ontology_find_object_type(def, a->targets[j])) {
                char p[CBM_SZ_1K];
                snprintf(p, sizeof(p), "action_types.%s.targets[%d]", a->name, j);
                ont_err(&ctx, p, "unknown object type");
            }
        }
    }

    return def;
}

/* ── frees ──────────────────────────────────────────────────────── */

void cbm_ontology_def_free(cbm_ontology_def_t *def) {
    if (!def) {
        return;
    }
    ont_props_free(def->shared_props, def->shared_prop_count);
    for (int i = 0; i < def->interface_count; i++) {
        free(def->interfaces[i].name);
        free(def->interfaces[i].description);
        ont_props_free(def->interfaces[i].props, def->interfaces[i].prop_count);
        ont_links_free(def->interfaces[i].links, def->interfaces[i].link_count);
    }
    free(def->interfaces);
    for (int i = 0; i < def->object_type_count; i++) {
        cbm_onto_object_type_t *t = &def->object_types[i];
        free(t->name);
        free(t->description);
        ont_free_str_array(t->implements, t->implements_count);
        ont_free_str_array(t->binds, t->binds_count);
        ont_props_free(t->props, t->prop_count);
        ont_links_free(t->links, t->link_count);
        ont_free_str_array(t->actions, t->action_count);
        ont_free_str_array(t->functions, t->function_count);
    }
    free(def->object_types);
    for (int i = 0; i < def->object_count; i++) {
        free(def->objects[i].name);
        free(def->objects[i].type);
        free(def->objects[i].props_json);
        free(def->objects[i].bind);
    }
    free(def->objects);
    for (int i = 0; i < def->link_type_count; i++) {
        free(def->link_types[i].name);
        free(def->link_types[i].description);
        free(def->link_types[i].source);
        free(def->link_types[i].target);
        free(def->link_types[i].cardinality);
    }
    free(def->link_types);
    for (int i = 0; i < def->action_count; i++) {
        ont_action_free(&def->actions[i]);
    }
    free(def->actions);
    for (int i = 0; i < def->function_count; i++) {
        free(def->functions[i].name);
        free(def->functions[i].description);
        free(def->functions[i].signature);
        free(def->functions[i].bind);
    }
    free(def->functions);
    free(def);
}

void cbm_ontology_errors_free(cbm_ontology_errors_t *errors) {
    if (!errors) {
        return;
    }
    for (int i = 0; i < errors->count; i++) {
        free(errors->items[i]);
    }
    free(errors->items);
    errors->items = NULL;
    errors->count = 0;
}

/* ── lookups ────────────────────────────────────────────────────── */

const cbm_onto_object_type_t *cbm_ontology_find_object_type(const cbm_ontology_def_t *def,
                                                          const char *name) {
    if (!def || !name) {
        return NULL;
    }
    for (int i = 0; i < def->object_type_count; i++) {
        if (strcmp(def->object_types[i].name, name) == 0) {
            return &def->object_types[i];
        }
    }
    return NULL;
}

const cbm_onto_action_t *cbm_ontology_find_action(const cbm_ontology_def_t *def,
                                                  const char *name) {
    if (!def || !name) {
        return NULL;
    }
    for (int i = 0; i < def->action_count; i++) {
        if (strcmp(def->actions[i].name, name) == 0) {
            return &def->actions[i];
        }
    }
    return NULL;
}

const cbm_onto_interface_t *cbm_ontology_find_interface(const cbm_ontology_def_t *def,
                                                        const char *name) {
    if (!def || !name) {
        return NULL;
    }
    for (int i = 0; i < def->interface_count; i++) {
        if (strcmp(def->interfaces[i].name, name) == 0) {
            return &def->interfaces[i];
        }
    }
    return NULL;
}

const cbm_onto_link_type_t *cbm_ontology_find_link_type(const cbm_ontology_def_t *def,
                                                        const char *name) {
    if (!def || !name) {
        return NULL;
    }
    for (int i = 0; i < def->link_type_count; i++) {
        if (strcmp(def->link_types[i].name, name) == 0) {
            return &def->link_types[i];
        }
    }
    return NULL;
}

const cbm_onto_object_t *cbm_ontology_find_object(const cbm_ontology_def_t *def,
                                                  const char *name) {
    if (!def || !name) {
        return NULL;
    }
    for (int i = 0; i < def->object_count; i++) {
        if (strcmp(def->objects[i].name, name) == 0) {
            return &def->objects[i];
        }
    }
    return NULL;
}

char *cbm_ontology_qn(const char *project, const char *kind, const char *name) {
    char *out = malloc(strlen(project) + strlen(kind) + strlen(name) + 12);
    if (!out) {
        return NULL;
    }
    sprintf(out, "%s.ontology.%s.%s", project, kind, name);
    return out;
}
