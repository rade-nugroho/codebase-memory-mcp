import { callToolJson } from '../provider/rpc-transport';
import type { CallToolOptions } from '../provider/rpc-transport';

/*
 * The project's Palantir-style ontology, materialized from .cbm-ontology.json
 * into the graph as Ontology* nodes. The server reads it through three tools:
 * ontology_query (all reads), manage_ontology in get/validate mode (the
 * definition file and its diagnostics), and ontology_execute (reviewed
 * executable action types). Writes to the definition file stay outside this
 * surface: /rpc refuses the mutating modes.
 */

export type OntologyKind =
    | 'object-type'
    | 'object'
    | 'interface'
    | 'link-type'
    | 'action-type'
    | 'function'
    | 'shared-property';

export const ONTOLOGY_KINDS: OntologyKind[] = [
    'object-type',
    'object',
    'link-type',
    'action-type',
    'function',
    'interface',
    'shared-property',
];

/** JSON key the server uses for the element list of a kind. */
export const ONTOLOGY_LIST_KEYS: Record<OntologyKind, string> = {
    'object-type': 'object_types',
    object: 'objects',
    interface: 'interfaces',
    'link-type': 'link_types',
    'action-type': 'action_types',
    function: 'functions',
    'shared-property': 'shared_properties',
};

export interface OntologySummary {
    present: boolean;
    definition_file: string;
    object_types: number;
    objects: number;
    interfaces: number;
    link_types: number;
    action_types: number;
    functions: number;
    shared_properties: number;
}

/** One element node: identity plus the definition's properties merged in. */
export interface OntologyElement {
    name: string;
    label: string;
    qualified_name: string;
    [key: string]: unknown;
}

export interface OntologyListResult {
    elements: OntologyElement[];
    total: number;
    offset: number;
    limit: number;
}

/** A declared bind target and whether the graph still holds it. */
export interface OntologyBinding {
    qualified_name: string;
    resolved: boolean;
    stale: boolean;
    label?: string;
    file_path?: string;
    start_line?: number;
}

/** A node reachable over a typed ontology edge. */
export interface OntologyEdgeTarget {
    name: string;
    qualified_name: string;
    label: string;
    file_path?: string;
    start_line?: number;
    /** Edge properties (link type name, cardinality) when declared. */
    edge?: Record<string, unknown>;
}

/** The full object view of one element. */
export interface OntologyElementDetail extends OntologyElement {
    binds: OntologyBinding[];
    bind?: string;
    bind_resolved?: boolean;
    bind_label?: string;
    bind_file?: string;
    bind_line?: number;
    bindings: OntologyEdgeTarget[];
    implements: OntologyEdgeTarget[];
    links: OntologyEdgeTarget[];
    actions: OntologyEdgeTarget[];
    functions: OntologyEdgeTarget[];
    shared_properties: OntologyEdgeTarget[];
    instances: OntologyEdgeTarget[];
    linked_from: OntologyEdgeTarget[];
}

export interface OntologyAuditRow {
    id: number;
    action: string;
    actor?: string;
    status: string;
    exit_code: number;
    duration_ms: number;
    detail?: string;
    created_at?: string;
}

export interface OntologyDefinition {
    exists: boolean;
    content: string;
    hint?: string;
}

export interface OntologyValidation {
    valid: boolean;
    error?: string;
    diagnostics: string[];
}

export interface OntologyExecution {
    status?: 'ok' | 'timeout' | 'denied' | string;
    error?: string;
    hint?: string;
    param?: string;
    output?: string;
    exit_code?: number;
    duration_ms?: number;
}

/** A declared action parameter, from the definition's params object. */
export interface OntologyParamDecl {
    name: string;
    type: 'string' | 'number' | 'boolean' | string;
    required: boolean;
    pattern?: string;
    description?: string;
    values?: string[];
}

/** Everything the workspace asks the server, as functions so a test can stub them. */
export interface OntologySource {
    summary(project: string): Promise<OntologySummary>;
    list(project: string, kind: OntologyKind, page?: { offset?: number; limit?: number }): Promise<OntologyListResult>;
    detail(project: string, kind: OntologyKind, name: string): Promise<OntologyElementDetail>;
    audit(project: string, limit?: number): Promise<OntologyAuditRow[]>;
    definition(project: string): Promise<OntologyDefinition>;
    validate(project: string, content?: string): Promise<OntologyValidation>;
    execute(project: string, action: string, params: Record<string, unknown>, confirm: boolean): Promise<OntologyExecution>;
}

const isRecord = (value: unknown): value is Record<string, unknown> =>
    typeof value === 'object' && value !== null && !Array.isArray(value);

const text = (value: unknown): string | undefined => (typeof value === 'string' ? value : undefined);
const num = (value: unknown): number | undefined => (typeof value === 'number' && Number.isFinite(value) ? value : undefined);
const flag = (value: unknown): boolean | undefined => (typeof value === 'boolean' ? value : undefined);
const strings = (value: unknown): string[] | undefined =>
    Array.isArray(value) && value.every(item => typeof item === 'string') ? value : undefined;

export function readOntologySummary(raw: unknown): OntologySummary {
    const body = isRecord(raw) ? raw : {};
    const counts = (key: string): number => num(body[key]) ?? 0;
    return {
        present: flag(body['present']) ?? false,
        definition_file: text(body['definition_file']) ?? '.cbm-ontology.json',
        object_types: counts('object_types'),
        objects: counts('objects'),
        interfaces: counts('interfaces'),
        link_types: counts('link_types'),
        action_types: counts('action_types'),
        functions: counts('functions'),
        shared_properties: counts('shared_properties'),
    };
}

export function readOntologyElement(raw: unknown): OntologyElement {
    const body = isRecord(raw) ? raw : {};
    return {
        ...body,
        name: text(body['name']) ?? '',
        label: text(body['label']) ?? '',
        qualified_name: text(body['qualified_name']) ?? '',
    };
}

export function readOntologyList(kind: OntologyKind, raw: unknown): OntologyListResult {
    const body = isRecord(raw) ? raw : {};
    const rows = body[ONTOLOGY_LIST_KEYS[kind]];
    return {
        elements: Array.isArray(rows) ? rows.map(readOntologyElement) : [],
        total: num(body['total']) ?? 0,
        offset: num(body['offset']) ?? 0,
        limit: num(body['limit']) ?? 200,
    };
}

function readBinding(raw: unknown): OntologyBinding {
    const body = isRecord(raw) ? raw : {};
    return {
        qualified_name: text(body['qualified_name']) ?? '',
        resolved: flag(body['resolved']) ?? false,
        stale: flag(body['stale']) ?? false,
        label: text(body['label']),
        file_path: text(body['file_path']),
        start_line: num(body['start_line']),
    };
}

function readEdgeTarget(raw: unknown): OntologyEdgeTarget {
    const body = isRecord(raw) ? raw : {};
    return {
        name: text(body['name']) ?? '',
        qualified_name: text(body['qualified_name']) ?? '',
        label: text(body['label']) ?? '',
        file_path: text(body['file_path']),
        start_line: num(body['start_line']),
        edge: isRecord(body['edge']) ? body['edge'] : undefined,
    };
}

const edgeList = (body: Record<string, unknown>, key: string): OntologyEdgeTarget[] =>
    Array.isArray(body[key]) ? (body[key] as unknown[]).map(readEdgeTarget) : [];

export function readOntologyDetail(raw: unknown): OntologyElementDetail {
    const body = isRecord(raw) ? raw : {};
    return {
        ...readOntologyElement(body),
        binds: Array.isArray(body['binds']) ? (body['binds'] as unknown[]).map(readBinding) : [],
        bind: text(body['bind']),
        bind_resolved: flag(body['bind_resolved']),
        bind_label: text(body['bind_label']),
        bind_file: text(body['bind_file']),
        bind_line: num(body['bind_line']),
        bindings: edgeList(body, 'bindings'),
        implements: edgeList(body, 'implements'),
        links: edgeList(body, 'links'),
        actions: edgeList(body, 'actions'),
        functions: edgeList(body, 'functions'),
        shared_properties: edgeList(body, 'shared_properties'),
        instances: edgeList(body, 'instances'),
        linked_from: edgeList(body, 'linked_from'),
    };
}

export function readOntologyAudit(raw: unknown): OntologyAuditRow[] {
    const body = isRecord(raw) ? raw : {};
    const rows = body['audit'];
    if (!Array.isArray(rows)) return [];
    return rows.map((row): OntologyAuditRow => {
        const r = isRecord(row) ? row : {};
        return {
            id: num(r['id']) ?? 0,
            action: text(r['action']) ?? '',
            actor: text(r['actor']),
            status: text(r['status']) ?? '',
            exit_code: num(r['exit_code']) ?? -1,
            duration_ms: num(r['duration_ms']) ?? 0,
            detail: text(r['detail']),
            created_at: text(r['created_at']),
        };
    });
}

export function readOntologyDefinition(raw: unknown): OntologyDefinition {
    const body = isRecord(raw) ? raw : {};
    return {
        exists: flag(body['exists']) ?? false,
        content: text(body['content']) ?? '',
        hint: text(body['hint']),
    };
}

export function readOntologyValidation(raw: unknown): OntologyValidation {
    const body = isRecord(raw) ? raw : {};
    const diagnostics = body['diagnostics'];
    return {
        valid: flag(body['valid']) ?? false,
        error: text(body['error']),
        diagnostics: Array.isArray(diagnostics) ? diagnostics.filter((d): d is string => typeof d === 'string') : [],
    };
}

export function readOntologyExecution(raw: unknown): OntologyExecution {
    const body = isRecord(raw) ? raw : {};
    return {
        status: text(body['status']),
        error: text(body['error']),
        hint: text(body['hint']),
        param: text(body['param']),
        output: text(body['output']),
        exit_code: num(body['exit_code']),
        duration_ms: num(body['duration_ms']),
    };
}

/* ── element accessors: the definition's own fields ride along merged ── */

/** The merged `description` string, when the definition declared one. */
export function elementDescription(element: OntologyElement): string {
    return text(element['description']) ?? '';
}

/** The merged inline `properties` object (object types, interfaces). */
export function elementProperties(element: OntologyElement): { name: string; type: string; required: boolean; description?: string; bind?: string; values?: string[] }[] {
    const props = element['properties'];
    if (!isRecord(props)) return [];
    return Object.entries(props).map(([name, value]) => {
        const p = isRecord(value) ? value : {};
        return {
            name,
            type: text(p['type']) ?? 'string',
            required: flag(p['required']) ?? false,
            description: text(p['description']),
            bind: text(p['bind']),
            values: strings(p['values']),
        };
    });
}

/** The declared params of an action type, in definition order. */
export function elementParams(element: OntologyElement): OntologyParamDecl[] {
    const params = element['params'];
    if (!isRecord(params)) return [];
    return Object.entries(params).map(([name, value]) => {
        const p = isRecord(value) ? value : {};
        return {
            name,
            type: text(p['type']) ?? 'string',
            required: flag(p['required']) ?? false,
            pattern: text(p['pattern']),
            description: text(p['description']),
            values: strings(p['values']),
        };
    });
}

/** Whether the action type is marked confirm:true. */
export function elementConfirm(element: OntologyElement): boolean {
    return flag(element['confirm']) ?? false;
}

/** The executable declaration, when the action type carries one. */
export function elementExec(element: OntologyElement): { argv: string[]; cwd: string; timeout_ms: number } | undefined {
    const exec = element['exec'];
    if (!isRecord(exec)) return undefined;
    return {
        argv: strings(exec['argv']) ?? [],
        cwd: text(exec['cwd']) ?? '.',
        timeout_ms: num(exec['timeout_ms']) ?? 0,
    };
}

/** The builtin name (e.g. "reindex"), when the action uses one instead of exec. */
export function elementBuiltin(element: OntologyElement): string | undefined {
    return text(element['builtin']);
}

/** Object-type target names an action applies to. */
export function elementTargets(element: OntologyElement): string[] {
    return strings(element['targets']) ?? [];
}

/** Link-type endpoints, or an object's values / a function's signature. */
export function elementField(element: OntologyElement, key: string): unknown {
    return element[key];
}

/** Can this action type be invoked at all (exec argv or builtin)? */
export function isExecutable(element: OntologyElement): boolean {
    return elementExec(element) !== undefined || elementBuiltin(element) !== undefined;
}

/* ── RPC-backed source ── */

/**
 * An OntologySource that speaks to POST /rpc in the MCP format. Only the
 * server's UI allowlist is used: ontology_query, manage_ontology in
 * get/validate mode, and ontology_execute (the definition file remains the
 * authorization boundary, and confirm:true actions still need confirm).
 */
export function rpcOntologySource(options: CallToolOptions = {}): OntologySource {
    const query = (args: Record<string, unknown>) =>
        callToolJson<unknown>('ontology_query', { ...args, format: 'json' }, options);
    const manage = (args: Record<string, unknown>) =>
        callToolJson<unknown>('manage_ontology', { ...args, format: 'json' }, options);
    return {
        summary: async (project) => readOntologySummary(await query({ project, kind: 'summary' })),
        list: async (project, kind, page) => readOntologyList(kind, await query({
            project,
            kind,
            ...(page?.offset !== undefined ? { offset: page.offset } : {}),
            ...(page?.limit !== undefined ? { limit: page.limit } : {}),
        })),
        detail: async (project, kind, name) => readOntologyDetail(await query({ project, kind, name })),
        audit: async (project, limit = 200) => readOntologyAudit(await query({ project, kind: 'audit', limit })),
        definition: async (project) => readOntologyDefinition(await manage({ project, mode: 'get' })),
        validate: async (project, content) => readOntologyValidation(await manage({
            project,
            mode: 'validate',
            ...(content !== undefined ? { content } : {}),
        })),
        execute: async (project, action, params, confirm) => readOntologyExecution(
            await callToolJson<unknown>('ontology_execute', {
                project,
                action,
                params: JSON.stringify(params),
                confirm,
                format: 'json',
            }, options)),
    };
}
