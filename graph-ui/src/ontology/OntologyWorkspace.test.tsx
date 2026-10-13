// @vitest-environment jsdom
import { act } from 'react';
import { createRoot, type Root } from 'react-dom/client';
import { afterEach, beforeEach, expect, it, vi } from 'vitest';
import OntologyWorkspace, { type OntologyWorkspaceProps } from './OntologyWorkspace';
import type {
    OntologyAuditRow, OntologyElement, OntologyElementDetail, OntologyKind, OntologySource,
} from './ontology-model';

let host: HTMLDivElement;
let root: Root;
const project = 'onto-test';

beforeEach(() => {
    (globalThis as unknown as Record<string, unknown>).IS_REACT_ACT_ENVIRONMENT = true;
    host = document.createElement('div');
    document.body.append(host);
    root = createRoot(host);
});
afterEach(async () => {
    await act(async () => root.unmount());
    host.remove();
    vi.restoreAllMocks();
});

const detail = (over: Partial<OntologyElementDetail> = {}): OntologyElementDetail => ({
    name: 'Order',
    label: 'OntologyObjectType',
    qualified_name: 'onto-test.ontology.object-type.Order',
    binds: [], bindings: [], implements: [], links: [], actions: [], functions: [],
    shared_properties: [], instances: [], linked_from: [],
    ...over,
});

const element = (name: string, over: Partial<OntologyElement> = {}): OntologyElement => ({
    name,
    label: 'OntologyObjectType',
    qualified_name: `onto-test.ontology.object-type.${name}`,
    ...over,
});

function source(over: Partial<OntologySource> = {}): OntologySource {
    return {
        summary: vi.fn(async () => ({
            present: true, definition_file: '.cbm-ontology.json',
            object_types: 2, objects: 1, interfaces: 1, link_types: 1,
            action_types: 1, functions: 1, shared_properties: 0,
        })),
        list: vi.fn(async (_p: string, kind: OntologyKind) => ({
            elements: kind === 'object-type' ? [element('Order'), element('Customer')] : [],
            total: kind === 'object-type' ? 2 : 0, offset: 0, limit: 500,
        })),
        detail: vi.fn(async () => detail()),
        audit: vi.fn(async () => [] as OntologyAuditRow[]),
        definition: vi.fn(async () => ({ exists: true, content: '{"version":1}' })),
        validate: vi.fn(async () => ({ valid: true, diagnostics: [] })),
        execute: vi.fn(async () => ({ status: 'ok', exit_code: 0, duration_ms: 5, output: 'done' })),
        ...over,
    };
}

async function render(api: OntologySource, overrides: Partial<OntologyWorkspaceProps> = {}) {
    await act(async () => root.render(<OntologyWorkspace project={project} active source={api} {...overrides} />));
}

function button(name: string): HTMLButtonElement {
    const found = [...host.querySelectorAll('button')].find(c => c.textContent === name);
    if (!found) throw new Error(`Missing button: ${name}`);
    return found;
}
async function click(name: string) { await act(async () => button(name).click()); }
function tabEl(name: string): HTMLButtonElement {
    const found = [...host.querySelectorAll<HTMLButtonElement>('[role="tab"]')].find(c => c.textContent === name);
    if (!found) throw new Error(`Missing tab: ${name}`);
    return found;
}
async function openTab(name: string) { await act(async () => tabEl(name).click()); }
function text(): string { return host.textContent ?? ''; }

it('reads the summary on activation and shows kind counts on the overview', async () => {
    const api = source();
    await render(api);
    expect(api.summary).toHaveBeenCalledWith(project);
    expect(text()).toContain('Object types');
    expect(host.querySelector('.ontology-metrics')?.textContent).toContain('2');
});

it('does not call the server while inactive', async () => {
    const api = source();
    await act(async () => root.render(<OntologyWorkspace project={project} active={false} source={api} />));
    expect(api.summary).not.toHaveBeenCalled();
});

it('shows the absent state when the project has no ontology', async () => {
    const api = source({
        summary: vi.fn(async () => ({
            present: false, definition_file: '.cbm-ontology.json',
            object_types: 0, objects: 0, interfaces: 0, link_types: 0,
            action_types: 0, functions: 0, shared_properties: 0,
        })),
    });
    await render(api);
    expect(text()).toContain('No ontology defined for this project.');
});

it('lists elements of a kind and opens the object view of a selection', async () => {
    const api = source({
        detail: vi.fn(async () => detail({
            binds: [{ qualified_name: 'onto-test.src.service.Order', resolved: true, stale: false, file_path: 'src/service.ts', start_line: 12 }],
            properties: { id: { type: 'string', required: true } },
        })),
    });
    await render(api);
    await openTab('Object types');
    expect(api.list).toHaveBeenCalledWith(project, 'object-type', { limit: 500 });
    await act(async () => host.querySelector<HTMLButtonElement>('.ontology-item')!.click());
    expect(api.detail).toHaveBeenCalledWith(project, 'object-type', 'Order');
    expect(text()).toContain('onto-test.src.service.Order');
    expect(text()).toContain('resolved');
    expect(text()).toContain('src/service.ts:12');
    expect(text()).toContain('id');
});

it('marks bindings that no longer resolve as stale', async () => {
    const api = source({
        detail: vi.fn(async () => detail({
            binds: [{ qualified_name: 'onto-test.src.missing.Nope', resolved: false, stale: true }],
        })),
    });
    await render(api);
    await openTab('Object types');
    await act(async () => host.querySelector<HTMLButtonElement>('.ontology-item')!.click());
    expect(text()).toContain('stale');
    expect(host.querySelector('.ontology-bind-stale')).not.toBeNull();
});

it('shows the element-not-found hint when the server reports one', async () => {
    const api = source({ detail: vi.fn(async () => detail({ error: 'element not found' })) });
    await render(api);
    await openTab('Object types');
    await act(async () => host.querySelector<HTMLButtonElement>('.ontology-item')!.click());
    expect(text()).toContain('Element not found.');
});

it('runs an executable action only after confirmation and shows the result', async () => {
    const api = source({
        list: vi.fn(async (_p: string, kind: OntologyKind) => ({
            elements: kind === 'action-type'
                ? [element('cancel_order', { label: 'OntologyActionType', confirm: true, exec: { argv: ['cancel'], cwd: '.', timeout_ms: 5000 }, params: { order_id: { type: 'string', required: true } } })]
                : [],
            total: 0, offset: 0, limit: 500,
        })),
        detail: vi.fn(async () => detail({
            name: 'cancel_order', label: 'OntologyActionType',
            confirm: true,
            exec: { argv: ['cancel', '{{order_id}}'], cwd: '.', timeout_ms: 5000 },
            params: { order_id: { type: 'string', required: true } },
        })),
    });
    await render(api);
    await openTab('Action types');
    await act(async () => host.querySelector<HTMLButtonElement>('.ontology-item')!.click());
    const execButton = [...host.querySelectorAll('button')].find(b => b.textContent === 'Execute')!;
    expect(execButton.disabled).toBe(true);
    await act(async () => {
        const box = host.querySelector<HTMLInputElement>('.ontology-confirm input')!;
        box.click();
    });
    await act(async () => {
        const input = host.querySelector<HTMLInputElement>('.ontology-field input')!;
        Object.getOwnPropertyDescriptor(HTMLInputElement.prototype, 'value')!.set!.call(input, 'ORD-42');
        input.dispatchEvent(new Event('input', { bubbles: true }));
    });
    await click('Execute');
    expect(api.execute).toHaveBeenCalledWith(project, 'cancel_order', { order_id: 'ORD-42' }, true);
    expect(text()).toContain('done');
    expect(text()).toContain('ok');
});

it('renders the audit table with recorded executions', async () => {
    const api = source({
        audit: vi.fn(async () => [{
            id: 3, action: 'cancel_order', actor: 'mcp', status: 'ok',
            exit_code: 0, duration_ms: 13, detail: 'cancelling ORD-42', created_at: '2026-10-03T16:06:00Z',
        }]),
    });
    await render(api);
    await openTab('Audit');
    expect(api.audit).toHaveBeenCalledWith(project);
    expect(text()).toContain('cancel_order');
    expect(text()).toContain('cancelling ORD-42');
});

it('shows the definition file and reports validation results', async () => {
    const api = source({
        validate: vi.fn(async () => ({ valid: false, error: 'parse failed', diagnostics: ['object_types.X: expected an array'] })),
    });
    await render(api);
    await openTab('Definition');
    expect(text()).toContain('{"version":1}');
    await click('Validate on disk');
    expect(api.validate).toHaveBeenCalledWith(project);
    expect(text()).toContain('object_types.X: expected an array');
});

it('surfaces read failures as an alert', async () => {
    const api = source({ summary: vi.fn(async () => { throw new Error('HTTP 500'); }) });
    await render(api);
    expect(host.querySelector('[role="alert"]')?.textContent).toContain('HTTP 500');
});
