import { describe, expect, it } from 'vitest';
import {
    elementConfirm, elementExec, elementParams, elementProperties, elementTargets,
    isExecutable, ONTOLOGY_LIST_KEYS, readOntologyAudit, readOntologyDefinition,
    readOntologyDetail, readOntologyExecution, readOntologyList, readOntologySummary,
    readOntologyValidation,
} from './ontology-model';
import type { OntologyElement } from './ontology-model';

const el = (over: Partial<OntologyElement> = {}): OntologyElement => ({
    name: 'x', label: 'L', qualified_name: 'q', ...over,
});

describe('readOntologySummary', () => {
    it('reads counts and presence, defaulting a malformed body', () => {
        const summary = readOntologySummary({ object_types: 4, present: true, definition_file: '.cbm-ontology.json' });
        expect(summary.present).toBe(true);
        expect(summary.object_types).toBe(4);
        expect(summary.objects).toBe(0);
        expect(readOntologySummary(undefined).present).toBe(false);
        expect(readOntologySummary('junk').definition_file).toBe('.cbm-ontology.json');
    });
});

describe('readOntologyList', () => {
    it('reads the list under the kind-specific key and merges properties', () => {
        const result = readOntologyList('object-type', {
            object_types: [{ name: 'Order', label: 'OntologyObjectType', qualified_name: 'p.ontology.object-type.Order', description: 'an order' }],
            total: 7, offset: 5, limit: 2,
        });
        expect(result.elements).toHaveLength(1);
        expect(result.elements[0].name).toBe('Order');
        expect(result.elements[0]['description']).toBe('an order');
        expect(result.total).toBe(7);
        expect(result.offset).toBe(5);
        // every kind maps to the server's list key
        expect(ONTOLOGY_LIST_KEYS['link-type']).toBe('link_types');
        expect(ONTOLOGY_LIST_KEYS['shared-property']).toBe('shared_properties');
    });
});

describe('readOntologyDetail', () => {
    it('reads resolved and stale bindings and edge neighbourhoods', () => {
        const detail = readOntologyDetail({
            name: 'Order', label: 'OntologyObjectType', qualified_name: 'p.ontology.object-type.Order',
            binds: [
                { qualified_name: 'p.a.Order', resolved: true, label: 'Class', file_path: 'a.ts', start_line: 3 },
                { qualified_name: 'p.missing.Nope', resolved: false, stale: true },
            ],
            links: [{ name: 'Customer', qualified_name: 'p.ontology.object-type.Customer', label: 'OntologyObjectType', edge: { type: 'placed_by', cardinality: 'many' } }],
            instances: [{ name: 'order-1', qualified_name: 'p.ontology.object.order-1', label: 'OntologyObject' }],
        });
        expect(detail.binds).toHaveLength(2);
        expect(detail.binds[0].file_path).toBe('a.ts');
        expect(detail.binds[1].stale).toBe(true);
        expect(detail.links[0].edge?.['type']).toBe('placed_by');
        expect(detail.instances).toHaveLength(1);
        expect(detail.linked_from).toEqual([]);
    });

    it('reads the single-object bind of objects and functions', () => {
        const detail = readOntologyDetail({
            name: 'order-1', bind: 'p.a.Order', bind_resolved: true, bind_file: 'a.ts', bind_line: 3,
        });
        expect(detail.bind).toBe('p.a.Order');
        expect(detail.bind_resolved).toBe(true);
        expect(detail.bind_line).toBe(3);
    });
});

describe('action element accessors', () => {
    const action = el({
        confirm: true,
        exec: { argv: ['cancel', '{{order_id}}'], cwd: '.', timeout_ms: 5000 },
        params: {
            order_id: { type: 'string', required: true, pattern: '^ORD-' },
            force: { type: 'boolean' },
            mode: { type: 'string', values: ['soft', 'hard'] },
        },
        targets: ['Order'],
    });
    it('reads the executable declaration, params, targets, and confirm flag', () => {
        expect(isExecutable(action)).toBe(true);
        expect(elementConfirm(action)).toBe(true);
        expect(elementExec(action)?.argv).toEqual(['cancel', '{{order_id}}']);
        expect(elementTargets(action)).toEqual(['Order']);
        const params = elementParams(action);
        expect(params.map(p => p.name)).toEqual(['order_id', 'force', 'mode']);
        expect(params[0].pattern).toBe('^ORD-');
        expect(params[2].values).toEqual(['soft', 'hard']);
    });
    it('treats actions without exec or builtin as not executable', () => {
        expect(isExecutable(el({ name: 'noop' }))).toBe(false);
        expect(isExecutable(el({ builtin: 'reindex' }))).toBe(true);
        expect(elementParams(el({ params: 'junk' }))).toEqual([]);
    });
});

describe('elementProperties', () => {
    it('reads the merged inline property map of object types and interfaces', () => {
        const props = elementProperties(el({ properties: { id: { type: 'string', required: true, values: ['a'] }, ts: { type: 'number' } } }));
        expect(props.map(p => p.name)).toEqual(['id', 'ts']);
        expect(props[0].required).toBe(true);
        expect(props[1].type).toBe('number');
        expect(elementProperties(el())).toEqual([]);
    });
});

describe('readOntologyAudit', () => {
    it('reads audit rows, tolerating absent optional fields', () => {
        const rows = readOntologyAudit({ audit: [{ id: 1, action: 'cancel_order', status: 'ok', exit_code: 0, duration_ms: 13 }], count: 1 });
        expect(rows).toHaveLength(1);
        expect(rows[0].action).toBe('cancel_order');
        expect(rows[0].actor).toBeUndefined();
        expect(readOntologyAudit({})).toEqual([]);
    });
});

describe('definition, validation, execution readers', () => {
    it('reads each management response shape', () => {
        expect(readOntologyDefinition({ exists: true, content: '{}' }).exists).toBe(true);
        expect(readOntologyDefinition(undefined).exists).toBe(false);
        const v = readOntologyValidation({ valid: false, diagnostics: ['x: bad', 7] });
        expect(v.valid).toBe(false);
        expect(v.diagnostics).toEqual(['x: bad']);
        const e = readOntologyExecution({ status: 'denied', error: 'confirmation required', hint: 'pass confirm:true' });
        expect(e.status).toBe('denied');
        expect(e.error).toBe('confirmation required');
        expect(readOntologyExecution(undefined).status).toBeUndefined();
    });
});
