/* test_ontology_def.c — .cbm-ontology.json parsing, validation, and gbuf emit. */
#include "test_framework.h"

#include "ontology_def.h"
#include "ontology_emit.h"
#include "graph_buffer/graph_buffer.h"

#include <string.h>

#define PROJECT "onto-test"

static const char *FULL_DEF =
    "{"
    "  \"version\": 1,"
    "  \"shared_properties\": {"
    "    \"tenant_id\": {\"type\": \"string\", \"description\": \"Owning tenant\"}"
    "  },"
    "  \"interfaces\": {"
    "    \"Audited\": {\"description\": \"With audit\","
    "                  \"properties\": {\"created_at\": {\"type\": \"string\", \"required\": true}}}"
    "  },"
    "  \"object_types\": {"
    "    \"Order\": {"
    "      \"description\": \"A customer order\","
    "      \"implements\": [\"Audited\"],"
    "      \"binds\": [\"" PROJECT ".src.service.Order\"],"
    "      \"properties\": {"
    "        \"order_id\": {\"type\": \"string\", \"required\": true},"
    "        \"status\": {\"type\": \"enum\", \"values\": [\"pending\", \"fulfilled\"]},"
    "        \"tenant_id\": {\"shared\": true}"
    "      },"
    "      \"links\": [{\"type\": \"placed_by\", \"target\": \"Customer\", \"cardinality\": \"many\"}],"
    "      \"actions\": [\"cancel_order\"],"
    "      \"functions\": [\"order_total\"]"
    "    },"
    "    \"Customer\": {}"
    "  },"
    "  \"objects\": {"
    "    \"demo-order-1\": {\"type\": \"Order\", \"bind\": \"" PROJECT ".src.service.Order\","
    "                      \"properties\": {\"order_id\": \"ORD-42\"}}"
    "  },"
    "  \"link_types\": {"
    "    \"placed_by\": {\"source\": \"Order\", \"target\": \"Customer\", \"cardinality\": \"many\"}"
    "  },"
    "  \"action_types\": {"
    "    \"cancel_order\": {"
    "      \"description\": \"Cancel an order\","
    "      \"confirm\": true,"
    "      \"targets\": [\"Order\"],"
    "      \"params\": {\"order_id\": {\"type\": \"string\", \"required\": true,"
    "                                 \"pattern\": \"^ORD-\"}},"
    "      \"exec\": {\"argv\": [\"./cancel.sh\", \"{{order_id}}\"], \"cwd\": \".\","
    "                \"timeout_ms\": 5000}"
    "    }"
    "  },"
    "  \"functions\": {"
    "    \"order_total\": {\"signature\": \"(Order) -> number\","
    "                     \"bind\": \"" PROJECT ".src.service.orderTotal\"}"
    "  }"
    "}";

static cbm_ontology_def_t *parse(const char *json, cbm_ontology_errors_t *errors) {
    return cbm_ontology_def_parse(json, (int)strlen(json), PROJECT, errors);
}

static bool errors_contain(const cbm_ontology_errors_t *errors, const char *needle) {
    for (int i = 0; i < errors->count; i++) {
        if (strstr(errors->items[i], needle)) {
            return true;
        }
    }
    return false;
}

/* ── parsing a complete definition ─────────────────────────────── */

TEST(ontology_parse_full) {
    cbm_ontology_errors_t errors = {0};
    cbm_ontology_def_t *def = parse(FULL_DEF, &errors);
    ASSERT_NOT_NULL(def);
    ASSERT_EQ(errors.count, 0);
    ASSERT_EQ(def->object_type_count, 2);
    ASSERT_EQ(def->object_count, 1);
    ASSERT_EQ(def->interface_count, 1);
    ASSERT_EQ(def->link_type_count, 1);
    ASSERT_EQ(def->action_count, 1);
    ASSERT_EQ(def->function_count, 1);
    ASSERT_EQ(def->shared_prop_count, 1);

    const cbm_onto_object_type_t *order = cbm_ontology_find_object_type(def, "Order");
    ASSERT_NOT_NULL(order);
    ASSERT_STR_EQ(order->description, "A customer order");
    ASSERT_EQ(order->implements_count, 1);
    ASSERT_STR_EQ(order->implements[0], "Audited");
    ASSERT_EQ(order->binds_count, 1);
    ASSERT_EQ(order->prop_count, 3);
    ASSERT_TRUE(order->props[0].required);
    ASSERT_EQ(order->props[1].enum_count, 2);
    ASSERT_TRUE(order->props[2].shared);
    ASSERT_EQ(order->link_count, 1);
    ASSERT_STR_EQ(order->links[0].target, "Customer");
    ASSERT_STR_EQ(order->links[0].cardinality, "many");
    ASSERT_EQ(order->action_count, 1);
    ASSERT_EQ(order->function_count, 1);

    const cbm_onto_object_t *obj = cbm_ontology_find_object(def, "demo-order-1");
    ASSERT_NOT_NULL(obj);
    ASSERT_STR_EQ(obj->type, "Order");
    ASSERT_STR_EQ(obj->bind, PROJECT ".src.service.Order");
    ASSERT_NOT_NULL(obj->props_json);
    ASSERT_NOT_NULL(strstr(obj->props_json, "ORD-42"));

    const cbm_onto_action_t *act = cbm_ontology_find_action(def, "cancel_order");
    ASSERT_NOT_NULL(act);
    ASSERT_TRUE(act->confirm);
    ASSERT_TRUE(act->has_exec);
    ASSERT_EQ(act->exec.argc, 2);
    ASSERT_STR_EQ(act->exec.argv[1], "{{order_id}}");
    ASSERT_EQ(act->exec.timeout_ms, 5000);
    ASSERT_EQ(act->param_count, 1);
    ASSERT_STR_EQ(act->params[0].pattern, "^ORD-");
    ASSERT_EQ(act->target_count, 1);

    ASSERT_EQ(def->function_count, 1);
    const cbm_onto_function_t *fn = &def->functions[0];
    ASSERT_STR_EQ(fn->name, "order_total");
    ASSERT_STR_EQ(fn->bind, PROJECT ".src.service.orderTotal");

    const cbm_onto_link_type_t *lt = cbm_ontology_find_link_type(def, "placed_by");
    ASSERT_NOT_NULL(lt);
    ASSERT_STR_EQ(lt->source, "Order");

    const cbm_onto_interface_t *in = cbm_ontology_find_interface(def, "Audited");
    ASSERT_NOT_NULL(in);
    ASSERT_EQ(in->prop_count, 1);

    cbm_ontology_def_free(def);
    cbm_ontology_errors_free(&errors);
    PASS();
}

TEST(ontology_qn_format) {
    char *qn = cbm_ontology_qn("proj", "object-type", "Order");
    ASSERT_NOT_NULL(qn);
    ASSERT_STR_EQ(qn, "proj.ontology.object-type.Order");
    free(qn);
    PASS();
}

/* ── structural validation ─────────────────────────────────────── */

TEST(ontology_rejects_non_object) {
    cbm_ontology_errors_t errors = {0};
    ASSERT_NULL(parse("[1,2,3]", &errors));
    ASSERT_GT(errors.count, 0);
    cbm_ontology_errors_free(&errors);
    ASSERT_NULL(parse("not json", NULL));
    PASS();
}

TEST(ontology_rejects_bad_version) {
    cbm_ontology_errors_t errors = {0};
    cbm_ontology_def_t *def = parse("{\"version\":2}", &errors);
    ASSERT_NOT_NULL(def); /* def still returned; the diagnostic marks it rejected */
    ASSERT_TRUE(errors_contain(&errors, "version"));
    cbm_ontology_def_free(def);
    cbm_ontology_errors_free(&errors);
    PASS();
}

TEST(ontology_links_must_be_array) {
    cbm_ontology_errors_t errors = {0};
    cbm_ontology_def_t *def = parse(
        "{\"version\":1,\"object_types\":{\"X\":{\"links\":{\"type\":\"t\",\"target\":\"Y\"}}}}",
        &errors);
    ASSERT_NOT_NULL(def);
    ASSERT_TRUE(errors_contain(&errors, "object_types.X: expected an array of link declarations"));
    cbm_ontology_def_free(def);
    cbm_ontology_errors_free(&errors);
    PASS();
}

TEST(ontology_enum_needs_values) {
    cbm_ontology_errors_t errors = {0};
    cbm_ontology_def_t *def = parse(
        "{\"version\":1,\"object_types\":{\"X\":{\"properties\":{\"s\":{\"type\":\"enum\"}}}}}",
        &errors);
    ASSERT_TRUE(errors_contain(&errors, "enum type requires a values array"));
    cbm_ontology_def_free(def);
    cbm_ontology_errors_free(&errors);
    PASS();
}

TEST(ontology_bad_cardinality) {
    cbm_ontology_errors_t errors = {0};
    cbm_ontology_def_t *def = parse(
        "{\"version\":1,\"link_types\":{\"l\":{\"source\":\"A\",\"target\":\"B\","
        "\"cardinality\":\"some\"}}}",
        &errors);
    ASSERT_TRUE(errors_contain(&errors, "cardinality"));
    cbm_ontology_def_free(def);
    cbm_ontology_errors_free(&errors);
    PASS();
}

/* ── reference validation ──────────────────────────────────────── */

TEST(ontology_dangling_references) {
    cbm_ontology_errors_t errors = {0};
    cbm_ontology_def_t *def = parse(
        "{\"version\":1,"
        "\"object_types\":{\"X\":{\"implements\":[\"Missing\"],\"actions\":[\"nope\"],"
        "\"functions\":[\"gone\"],\"properties\":{\"t\":{\"shared\":true}},"
        "\"links\":[{\"type\":\"l\",\"target\":\"Nowhere\"}]}},"
        "\"objects\":{\"o\":{\"type\":\"Ghost\"}},"
        "\"link_types\":{\"l\":{\"source\":\"X\",\"target\":\"Nowhere\"}},"
        "\"action_types\":{\"a\":{\"targets\":[\"Ghost\"]}}}",
        &errors);
    ASSERT_TRUE(errors_contain(&errors, "unknown interface"));
    ASSERT_TRUE(errors_contain(&errors, "unknown action type"));
    ASSERT_TRUE(errors_contain(&errors, "unknown function"));
    ASSERT_TRUE(errors_contain(&errors, "shared property not declared"));
    ASSERT_TRUE(errors_contain(&errors, "unknown object type"));
    cbm_ontology_def_free(def);
    cbm_ontology_errors_free(&errors);
    PASS();
}

/* ── executable declaration validation ─────────────────────────── */

TEST(ontology_exec_vs_builtin_exclusive) {
    cbm_ontology_errors_t errors = {0};
    cbm_ontology_def_t *def = parse(
        "{\"version\":1,\"action_types\":{\"a\":{\"exec\":{\"argv\":[\"x\"]},"
        "\"builtin\":\"reindex\"}}}",
        &errors);
    ASSERT_TRUE(errors_contain(&errors, "exec or builtin, not both"));
    cbm_ontology_def_free(def);
    cbm_ontology_errors_free(&errors);
    PASS();
}

TEST(ontology_unknown_builtin) {
    cbm_ontology_errors_t errors = {0};
    cbm_ontology_def_t *def = parse(
        "{\"version\":1,\"action_types\":{\"a\":{\"builtin\":\"rm_rf\"}}}", &errors);
    ASSERT_TRUE(errors_contain(&errors, "unknown builtin"));
    cbm_ontology_def_free(def);
    cbm_ontology_errors_free(&errors);
    PASS();
}

TEST(ontology_exec_validation) {
    cbm_ontology_errors_t errors = {0};
    cbm_ontology_def_t *def = parse(
        "{\"version\":1,\"action_types\":{\"a\":{\"exec\":{\"argv\":[]}}}}", &errors);
    ASSERT_TRUE(errors_contain(&errors, "non-empty"));
    cbm_ontology_def_free(def);
    cbm_ontology_errors_free(&errors);

    cbm_ontology_def_t *bad_cwd = parse(
        "{\"version\":1,\"action_types\":{\"a\":{\"exec\":{\"argv\":[\"x\"],"
        "\"cwd\":\"../escape\"}}}}", &errors);
    ASSERT_TRUE(errors_contain(&errors, "cwd"));
    cbm_ontology_def_free(bad_cwd);
    cbm_ontology_errors_free(&errors);
    PASS();
}

/* ── emit into the graph buffer ────────────────────────────────── */

TEST(ontology_emit_nodes_and_edges) {
    cbm_ontology_errors_t errors = {0};
    cbm_ontology_def_t *def = parse(FULL_DEF, &errors);
    ASSERT_NOT_NULL(def);

    cbm_gbuf_t *gb = cbm_gbuf_new(PROJECT, "/tmp/onto-repo");
    ASSERT_NOT_NULL(gb);
    /* the code node the Order object type binds to */
    cbm_gbuf_upsert_node(gb, "Class", "Order", PROJECT ".src.service.Order",
                         "src/service.py", 3, 40, "{}");

    cbm_ontology_unresolved_t u = {0};
    ASSERT_EQ(cbm_ontology_emit(gb, def, PROJECT, &u), 0);
    ASSERT_EQ(u.count, 1); /* order_total's bind target is not in the graph */
    ASSERT_STR_EQ(u.items[0].to_qn, PROJECT ".src.service.orderTotal");
    cbm_ontology_unresolved_free(&u);

    const cbm_gbuf_node_t *root = cbm_gbuf_find_by_qn(gb, PROJECT ".ontology");
    ASSERT_NOT_NULL(root);
    ASSERT_STR_EQ(root->label, "Ontology");
    ASSERT_STR_EQ(root->file_path, CBM_ONTOLOGY_FILENAME);

    const cbm_gbuf_node_t *order = cbm_gbuf_find_by_qn(gb, PROJECT ".ontology.object-type.Order");
    ASSERT_NOT_NULL(order);
    ASSERT_STR_EQ(order->label, "OntologyObjectType");
    ASSERT_NOT_NULL(cbm_gbuf_find_by_qn(gb, PROJECT ".ontology.object-type.Customer"));
    ASSERT_NOT_NULL(cbm_gbuf_find_by_qn(gb, PROJECT ".ontology.object.demo-order-1"));
    ASSERT_NOT_NULL(cbm_gbuf_find_by_qn(gb, PROJECT ".ontology.interface.Audited"));
    ASSERT_NOT_NULL(cbm_gbuf_find_by_qn(gb, PROJECT ".ontology.link-type.placed_by"));
    ASSERT_NOT_NULL(cbm_gbuf_find_by_qn(gb, PROJECT ".ontology.action-type.cancel_order"));
    ASSERT_NOT_NULL(cbm_gbuf_find_by_qn(gb, PROJECT ".ontology.function.order_total"));
    ASSERT_NOT_NULL(cbm_gbuf_find_by_qn(gb, PROJECT ".ontology.shared-property.tenant_id"));

    /* resolved binding: ONT_BINDS edge to the real code node */
    const cbm_gbuf_node_t *code = cbm_gbuf_find_by_qn(gb, PROJECT ".src.service.Order");
    ASSERT_NOT_NULL(code);
    const cbm_gbuf_edge_t **edges = NULL;
    int count = 0;
    ASSERT_EQ(cbm_gbuf_find_edges_by_source_type(gb, order->id, "ONT_BINDS", &edges, &count), 0);
    ASSERT_EQ(count, 1);
    ASSERT_EQ(edges[0]->target_id, code->id);

    /* links + instance + shared property edges exist */
    ASSERT_EQ(cbm_gbuf_find_edges_by_source_type(gb, order->id, "ONT_LINKS", &edges, &count), 0);
    ASSERT_GT(count, 0);
    ASSERT_EQ(cbm_gbuf_find_edges_by_source_type(gb, order->id, "ONT_HAS_ACTION", &edges, &count), 0);
    ASSERT_EQ(count, 1);
    ASSERT_EQ(cbm_gbuf_find_edges_by_source_type(gb, order->id, "ONT_HAS_PROPERTY", &edges, &count), 0);
    ASSERT_EQ(count, 1);
    ASSERT_EQ(cbm_gbuf_find_edges_by_source_type(gb, order->id, "ONT_IMPLEMENTS", &edges, &count), 0);
    ASSERT_EQ(count, 1);

    cbm_gbuf_free(gb);
    cbm_ontology_def_free(def);
    cbm_ontology_errors_free(&errors);
    PASS();
}

TEST(ontology_emit_stale_binding_reported_not_dropped) {
    cbm_ontology_def_t *def = parse(
        "{\"version\":1,\"object_types\":{\"Ghost\":{\"binds\":[\"" PROJECT ".src.missing.Nope\"]}}}",
        NULL);
    ASSERT_NOT_NULL(def);
    cbm_gbuf_t *gb = cbm_gbuf_new(PROJECT, "/tmp/onto-repo");
    cbm_ontology_unresolved_t u = {0};
    ASSERT_EQ(cbm_ontology_emit(gb, def, PROJECT, &u), 0);
    ASSERT_EQ(u.count, 1);
    ASSERT_STR_EQ(u.items[0].from_qn, PROJECT ".ontology.object-type.Ghost");
    ASSERT_STR_EQ(u.items[0].to_qn, PROJECT ".src.missing.Nope");
    /* the element still exists — stale is a report, not a delete */
    ASSERT_NOT_NULL(cbm_gbuf_find_by_qn(gb, PROJECT ".ontology.object-type.Ghost"));
    cbm_ontology_unresolved_free(&u);
    cbm_gbuf_free(gb);
    cbm_ontology_def_free(def);
    PASS();
}

SUITE(ontology_def) {
    RUN_TEST(ontology_parse_full);
    RUN_TEST(ontology_qn_format);
    RUN_TEST(ontology_rejects_non_object);
    RUN_TEST(ontology_rejects_bad_version);
    RUN_TEST(ontology_links_must_be_array);
    RUN_TEST(ontology_enum_needs_values);
    RUN_TEST(ontology_bad_cardinality);
    RUN_TEST(ontology_dangling_references);
    RUN_TEST(ontology_exec_vs_builtin_exclusive);
    RUN_TEST(ontology_unknown_builtin);
    RUN_TEST(ontology_exec_validation);
    RUN_TEST(ontology_emit_nodes_and_edges);
    RUN_TEST(ontology_emit_stale_binding_reported_not_dropped);
}
