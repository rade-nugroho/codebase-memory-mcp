/*
 * ontology_def.h — the .cbm-ontology.json definition model: parse + validate.
 *
 * An ontology file is ontology-as-code committed to the repo (the SSOT):
 * object types, concrete objects (instances), interfaces, link types,
 * action types (optionally executable), and named functions. Definitions
 * bind to the code graph by qualified name so an agent can see "which code
 * realizes this ontological concept".
 *
 * The parser is intentionally strict about shape and forgiving about
 * content: structural errors (wrong JSON type, missing required field)
 * collect into an error list; semantic errors (dangling references, bad
 * param types) are recorded the same way so callers can either reject or
 * emit-and-report.
 */
#ifndef CBM_ONTOLOGY_DEF_H
#define CBM_ONTOLOGY_DEF_H

#include <stdbool.h>

#define CBM_ONTOLOGY_FILENAME ".cbm-ontology.json"
#define CBM_ONTOLOGY_VERSION 1

/* ── Element model ─────────────────────────────────────────────── */

typedef struct {
    char *name;
    char *type;        /* string|number|boolean|enum; NULL when shared */
    char *bind;        /* optional code-node QN */
    char *description; /* optional */
    bool required;
    bool shared;       /* reference into shared_properties by name */
    char **enum_values;
    int enum_count;
} cbm_onto_prop_t;

typedef struct {
    char *name;
    char *type; /* string|number|boolean|enum */
    char *description;
    char *pattern; /* optional regex (validated with the platform regex) */
    bool required;
    char **enum_values;
    int enum_count;
} cbm_onto_param_t;

typedef struct {
    char *type;        /* link type name */
    char *target;      /* object type name */
    char *cardinality; /* "one"|"many"; NULL = unspecified */
} cbm_onto_link_decl_t;

typedef struct {
    char **argv;
    int argc;
    char *cwd;      /* relative to repo root; "." default */
    int timeout_ms; /* default 30000 */
    char **env;     /* "NAME=value" entries */
    int env_count;
} cbm_onto_exec_t;

typedef struct {
    char *name;
    char *description;
    char **implements;
    int implements_count;
    char **binds; /* code-node QNs this type is realized by */
    int binds_count;
    cbm_onto_prop_t *props;
    int prop_count;
    cbm_onto_link_decl_t *links;
    int link_count;
    char **actions; /* action type names */
    int action_count;
    char **functions; /* function names */
    int function_count;
} cbm_onto_object_type_t;

typedef struct {
    char *name;
    char *type;       /* object type name */
    char *props_json; /* raw JSON object of property values (owned) */
    char *bind;       /* optional code-node QN */
} cbm_onto_object_t;

typedef struct {
    char *name;
    char *description;
    cbm_onto_prop_t *props;
    int prop_count;
    cbm_onto_link_decl_t *links;
    int link_count;
} cbm_onto_interface_t;

typedef struct {
    char *name;
    char *description;
    char *source;      /* object type name */
    char *target;      /* object type name */
    char *cardinality; /* "one"|"many" */
} cbm_onto_link_type_t;

typedef struct {
    char *name;
    char *description;
    cbm_onto_param_t *params;
    int param_count;
    char **targets; /* object type names the action applies to */
    int target_count;
    bool confirm;   /* caller must pass confirm:true to run */
    bool has_exec;  /* exec block present */
    cbm_onto_exec_t exec;
    char *builtin; /* e.g. "reindex" — mutually exclusive with exec */
} cbm_onto_action_t;

typedef struct {
    char *name;
    char *description;
    char *signature; /* free text "(Order) -> float" */
    char *bind;      /* optional code-node QN */
} cbm_onto_function_t;

typedef struct {
    cbm_onto_prop_t *shared_props;
    int shared_prop_count;
    cbm_onto_interface_t *interfaces;
    int interface_count;
    cbm_onto_object_type_t *object_types;
    int object_type_count;
    cbm_onto_object_t *objects;
    int object_count;
    cbm_onto_link_type_t *link_types;
    int link_type_count;
    cbm_onto_action_t *actions;
    int action_count;
    cbm_onto_function_t *functions;
    int function_count;
} cbm_ontology_def_t;

/* Collected diagnostics; each item is a heap "section.field: message" line. */
typedef struct {
    char **items;
    int count;
} cbm_ontology_errors_t;

/* ── API ────────────────────────────────────────────────────────── */

/* Parse + validate an ontology definition. `project` is used only to tag
 * error locations and check that "bind" QNs look project-scoped. Returns a
 * heap def (free with cbm_ontology_def_free) or NULL when the text is not a
 * JSON object. `errors` may be NULL; when non-NULL it is always initialized
 * (count 0 on clean input) and collects both structural and reference
 * problems — a returned non-NULL def with errors->count > 0 is still
 * emit-able; emitters skip the offending members. */
cbm_ontology_def_t *cbm_ontology_def_parse(const char *json_text, int json_len,
                                           const char *project, cbm_ontology_errors_t *errors);

void cbm_ontology_def_free(cbm_ontology_def_t *def);
void cbm_ontology_errors_free(cbm_ontology_errors_t *errors);

/* Element lookups by name (linear; ontologies are small). */
const cbm_onto_object_type_t *cbm_ontology_find_object_type(const cbm_ontology_def_t *def,
                                                          const char *name);
const cbm_onto_action_t *cbm_ontology_find_action(const cbm_ontology_def_t *def, const char *name);
const cbm_onto_interface_t *cbm_ontology_find_interface(const cbm_ontology_def_t *def,
                                                      const char *name);
const cbm_onto_link_type_t *cbm_ontology_find_link_type(const cbm_ontology_def_t *def,
                                                      const char *name);
const cbm_onto_object_t *cbm_ontology_find_object(const cbm_ontology_def_t *def, const char *name);

/* Deterministic qualified name for an ontology element:
 * "<project>.ontology.<kind>.<name>". Caller frees. */
char *cbm_ontology_qn(const char *project, const char *kind, const char *name);

#endif /* CBM_ONTOLOGY_DEF_H */
