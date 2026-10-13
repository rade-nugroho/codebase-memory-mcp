import { useCallback, useEffect, useRef, useState, type KeyboardEvent } from 'react';
import type { JSX, ReactNode } from 'react';
import {
    elementBuiltin, elementConfirm, elementDescription, elementExec, elementField,
    elementParams, elementProperties, elementTargets, isExecutable,
    ONTOLOGY_KINDS, ONTOLOGY_LIST_KEYS,
} from './ontology-model';
import type {
    OntologyAuditRow, OntologyDefinition, OntologyEdgeTarget, OntologyElement,
    OntologyElementDetail, OntologyExecution, OntologyKind, OntologyListResult,
    OntologySource, OntologySummary, OntologyValidation,
} from './ontology-model';
import { ontologyStrings as s } from './ontology-strings';
import './ontology.css';

export interface OntologyWorkspaceProps {
    project: string;
    active: boolean;
    source: OntologySource;
}

type OntologyTab = 'overview' | OntologyKind | 'definition' | 'audit';
const TABS: OntologyTab[] = ['overview', ...ONTOLOGY_KINDS, 'definition', 'audit'];

interface Loadable<T> {
    phase: 'idle' | 'loading' | 'ready' | 'error';
    data?: T;
    error: string;
}

const idle: Loadable<never> = { phase: 'idle', error: '' };
const loading: Loadable<never> = { phase: 'loading', error: '' };
const ready = <T,>(data: T): Loadable<T> => ({ phase: 'ready', data, error: '' });
const failed = <T,>(error: unknown): Loadable<T> => ({
    phase: 'error',
    error: error instanceof Error ? error.message : String(error),
});

function messageOf(error: unknown): string {
    return error instanceof Error ? error.message : String(error);
}

function kindCount(summary: OntologySummary, kind: OntologyKind): number {
    const value = summary[ONTOLOGY_LIST_KEYS[kind] as keyof OntologySummary];
    return typeof value === 'number' ? value : 0;
}

function edgeTitle(target: OntologyEdgeTarget): string {
    const type = target.edge && typeof target.edge['type'] === 'string' ? target.edge['type'] : undefined;
    const cardinality = target.edge && typeof target.edge['cardinality'] === 'string' ? target.edge['cardinality'] : '';
    return [type, cardinality].filter(Boolean).join(' · ');
}

function TargetChip({ target }: { target: OntologyEdgeTarget }) {
    const meta = edgeTitle(target);
    return <li className="ontology-target">
        <span className="ontology-target-name">{target.name}</span>
        {meta && <span className="ontology-target-meta">{meta}</span>}
        {target.label && <span className="ontology-tag">{target.label}</span>}
        {target.file_path && <code className="ontology-loc">{target.file_path}{target.start_line ? `:${target.start_line}` : ''}</code>}
    </li>;
}

function EdgeGroup({ title, targets }: { title: string; targets: OntologyEdgeTarget[] }) {
    if (targets.length === 0) return null;
    return <section className="ontology-group">
        <h4>{title} <span className="ontology-count">{targets.length}</span></h4>
        <ul className="ontology-targets">{targets.map(t => <TargetChip key={`${t.qualified_name}:${edgeTitle(t)}`} target={t} />)}</ul>
    </section>;
}

function PropertiesTable({ element }: { element: OntologyElement }) {
    const props = elementProperties(element);
    const values = element['values'];
    const signature = element['signature'];
    return <>
        {typeof signature === 'string' && signature && <p className="ontology-signature"><code>{signature}</code></p>}
        {props.length > 0 && <section className="ontology-group">
            <h4>{s.properties} <span className="ontology-count">{props.length}</span></h4>
            <div className="ontology-table-wrap"><table><thead>
                <tr><th>Name</th><th>{s.type}</th><th>{s.values}</th><th>{s.binds}</th></tr></thead><tbody>
                {props.map(p => <tr key={p.name}>
                    <th scope="row">{p.name}{p.required && <span className="ontology-required" title={s.propertyRequired}>*</span>}</th>
                    <td><code>{p.type}</code>{p.description && <p className="ontology-muted">{p.description}</p>}</td>
                    <td>{p.values ? p.values.join(', ') : '—'}</td>
                    <td>{p.bind ? <code>{p.bind}</code> : '—'}</td>
                </tr>)}
            </tbody></table></div>
        </section>}
        {props.length === 0 && values === undefined && typeof signature !== 'string' && <p className="ontology-muted">{s.noProperties}</p>}
        {values !== undefined && <section className="ontology-group">
            <h4>{s.values}</h4>
            <pre className="ontology-json">{JSON.stringify(values, null, 2)}</pre>
        </section>}
    </>;
}

function FieldRow({ label, children }: { label: string; children: ReactNode }) {
    return <tr><th scope="row">{label}</th><td>{children}</td></tr>;
}

function BindsList({ detail }: { detail: OntologyElementDetail }) {
    const single = detail.bind !== undefined;
    if (detail.binds.length === 0 && !single) return null;
    return <section className="ontology-group">
        <h4>{s.binds}</h4>
        <ul className="ontology-binds">
            {detail.binds.map(b => <li key={b.qualified_name} className={b.resolved ? 'ontology-bind-ok' : 'ontology-bind-stale'}>
                <code>{b.qualified_name}</code>
                <span className="ontology-tag">{b.resolved ? s.resolved : s.stale}</span>
                {b.file_path && <code className="ontology-loc">{b.file_path}{b.start_line ? `:${b.start_line}` : ''}</code>}
                {b.label && <span className="ontology-tag">{b.label}</span>}
            </li>)}
            {single && <li className={detail.bind_resolved ? 'ontology-bind-ok' : 'ontology-bind-stale'}>
                <code>{detail.bind}</code>
                <span className="ontology-tag">{detail.bind_resolved ? s.resolved : s.unresolved}</span>
                {detail.bind_file && <code className="ontology-loc">{detail.bind_file}{detail.bind_line ? `:${detail.bind_line}` : ''}</code>}
                {detail.bind_label && <span className="ontology-tag">{detail.bind_label}</span>}
            </li>}
        </ul>
    </section>;
}

interface RunState {
    values: Record<string, string>;
    confirmed: boolean;
    running: boolean;
    result?: OntologyExecution;
    error: string;
}

function ActionRunner({ element, source, project, onExecuted }: {
    element: OntologyElementDetail;
    source: OntologySource;
    project: string;
    onExecuted: () => void;
}) {
    const params = elementParams(element);
    const needsConfirm = elementConfirm(element);
    const exec = elementExec(element);
    const builtin = elementBuiltin(element);
    const [run, setRun] = useState<RunState>({ values: {}, confirmed: false, running: false, error: '' });
    const executable = isExecutable(element);

    const update = (patch: Partial<RunState>) => setRun(prev => ({ ...prev, ...patch }));
    const submit = async () => {
        const values: Record<string, unknown> = {};
        for (const decl of params) {
            const raw = run.values[decl.name] ?? '';
            if (raw === '' && !decl.required) continue;
            if (decl.type === 'boolean') values[decl.name] = raw === 'true';
            else if (decl.type === 'number') {
                const parsed = Number(raw);
                if (!Number.isFinite(parsed)) {
                    update({ error: `${decl.name}: expected a number` });
                    return;
                }
                values[decl.name] = parsed;
            } else values[decl.name] = raw;
        }
        update({ running: true, error: '', result: undefined });
        try {
            const result = await source.execute(project, element.name, values, needsConfirm ? run.confirmed : false);
            update({ running: false, result });
            onExecuted();
        } catch (error) {
            update({ running: false, error: messageOf(error) });
        }
    };

    if (!executable) return <p className="ontology-muted">{s.notExecutable}</p>;
    const blocked = (needsConfirm && !run.confirmed) || run.running;
    return <section className="ontology-group ontology-runner">
        <h4>{s.runTitle}</h4>
        {exec && <p className="ontology-muted"><code>{exec.argv.join(' ')}</code> · cwd <code>{exec.cwd}</code> · timeout {exec.timeout_ms}ms</p>}
        {builtin && <p className="ontology-muted">builtin: <code>{builtin}</code></p>}
        {params.map(decl => <label key={decl.name} className="ontology-field">
            <span>{decl.name}{decl.required && <span className="ontology-required" title={s.paramRequired}>*</span>}
                <small> {decl.type}{decl.description ? ` · ${decl.description}` : ''}{decl.pattern ? ` · /${decl.pattern}/` : ''}</small></span>
            {decl.values ? <select value={run.values[decl.name] ?? ''} onChange={e => update({ values: { ...run.values, [decl.name]: e.currentTarget.value } })}>
                <option value="">—</option>
                {decl.values.map(v => <option key={v} value={v}>{v}</option>)}
            </select> : decl.type === 'boolean' ? <select value={run.values[decl.name] ?? 'false'} onChange={e => update({ values: { ...run.values, [decl.name]: e.currentTarget.value } })}>
                <option value="false">false</option><option value="true">true</option>
            </select> : <input type="text" value={run.values[decl.name] ?? ''} placeholder={s.paramPlaceholder(decl)}
                onChange={e => update({ values: { ...run.values, [decl.name]: e.currentTarget.value } })} />}
        </label>)}
        {needsConfirm && <label className="ontology-confirm">
            <input type="checkbox" checked={run.confirmed} onChange={e => update({ confirmed: e.currentTarget.checked })} />
            {s.confirmLabel}
        </label>}
        <div className="ontology-run-actions">
            <button type="button" className="ontology-primary" disabled={blocked} onClick={() => void submit()}>
                {run.running ? s.executing : s.execute}
            </button>
        </div>
        {run.error && <p className="ontology-error" role="alert">{run.error}</p>}
        {run.result && <div className="ontology-result" role="status">
            <h5>{s.execResult}</h5>
            {run.result.status && <p><span className={`ontology-tag ontology-status-${run.result.status}`}>{run.result.status}</span>
                {run.result.exit_code !== undefined && <span className="ontology-muted"> {s.exitCode} {run.result.exit_code}</span>}
                {run.result.duration_ms !== undefined && <span className="ontology-muted"> · {run.result.duration_ms}ms</span>}</p>}
            {run.result.error && <p className="ontology-error">{run.result.error}{run.result.param ? ` (${run.result.param})` : ''}</p>}
            {run.result.hint && <p className="ontology-muted">{run.result.hint}</p>}
            {run.result.output && <pre className="ontology-json">{run.result.output}</pre>}
        </div>}
    </section>;
}

function ElementDetail({ detail, source, project, onExecuted }: {
    detail: OntologyElementDetail;
    source: OntologySource;
    project: string;
    onExecuted: () => void;
}) {
    const description = elementDescription(detail);
    const targets = elementTargets(detail);
    const linkSource = elementField(detail, 'source');
    const linkTarget = elementField(detail, 'target');
    const cardinality = elementField(detail, 'cardinality');
    const objectType = elementField(detail, 'type');
    return <article className="ontology-detail">
        <header>
            <h3>{detail.name}</h3>
            <code className="ontology-qn">{detail.qualified_name}</code>
            {description && <p className="ontology-desc">{description}</p>}
        </header>
        {(typeof linkSource === 'string' || typeof objectType === 'string' || targets.length > 0) && <section className="ontology-group">
            <div className="ontology-table-wrap"><table><tbody>
                {typeof objectType === 'string' && <FieldRow label={s.type}>{objectType}</FieldRow>}
                {typeof linkSource === 'string' && <FieldRow label={s.source}>{linkSource}</FieldRow>}
                {typeof linkTarget === 'string' && <FieldRow label={s.target}>{linkTarget}</FieldRow>}
                {typeof cardinality === 'string' && cardinality && <FieldRow label={s.cardinality}>{cardinality}</FieldRow>}
                {targets.length > 0 && <FieldRow label={s.target}>{targets.join(', ')}</FieldRow>}
            </tbody></table></div>
        </section>}
        <BindsList detail={detail} />
        <PropertiesTable element={detail} />
        <EdgeGroup title={s.implements} targets={detail.implements} />
        <EdgeGroup title={s.links} targets={detail.links} />
        <EdgeGroup title={s.linkedFrom} targets={detail.linked_from} />
        <EdgeGroup title={s.actions} targets={detail.actions} />
        <EdgeGroup title={s.functions} targets={detail.functions} />
        <EdgeGroup title={s.sharedProperties} targets={detail.shared_properties} />
        <EdgeGroup title={s.instances} targets={detail.instances} />
        <EdgeGroup title={s.bindings} targets={detail.bindings} />
        {detail.label === 'OntologyActionType' &&
            <ActionRunner element={detail} source={source} project={project} onExecuted={onExecuted} />}
    </article>;
}

function DefinitionPanel({ definition, source, project }: {
    definition: Loadable<OntologyDefinition>;
    source: OntologySource;
    project: string;
}) {
    const [validation, setValidation] = useState<Loadable<OntologyValidation>>(idle);
    const run = async () => {
        setValidation(loading);
        try {
            setValidation(ready(await source.validate(project)));
        } catch (error) {
            setValidation(failed(error));
        }
    };
    return <div className="ontology-panel">
        <p className="ontology-muted">{s.definitionReadOnly}</p>
        <div className="ontology-run-actions">
            <button type="button" onClick={() => void run()} disabled={validation.phase === 'loading'}>
                {validation.phase === 'loading' ? s.validating : s.validate}
            </button>
        </div>
        {validation.phase === 'error' && <p className="ontology-error" role="alert">{validation.error}</p>}
        {validation.phase === 'ready' && validation.data && <div className="ontology-result" role="status">
            {validation.data.valid
                ? <p><span className="ontology-tag ontology-status-ok">{s.validationOk}</span></p>
                : <>
                    {validation.data.error && <p className="ontology-error">{validation.data.error}</p>}
                    {validation.data.diagnostics.length > 0
                        ? <ul className="ontology-diagnostics">{validation.data.diagnostics.map((d, i) => <li key={i}><code>{d}</code></li>)}</ul>
                        : !validation.data.error && <p className="ontology-muted">{s.validationNone}</p>}
                </>}
        </div>}
        {definition.phase === 'ready' && definition.data && (definition.data.exists
            ? <pre className="ontology-json ontology-definition" data-testid="ontology-definition">{definition.data.content || '{}'}</pre>
            : <p className="ontology-muted">{definition.data.hint ?? s.definitionEmpty}</p>)}
    </div>;
}

function AuditPanel({ audit }: { audit: Loadable<OntologyAuditRow[]> }) {
    const rows = audit.data ?? [];
    return <div className="ontology-panel">
        <p className="ontology-muted">{s.auditIntro}</p>
        {audit.phase === 'ready' && rows.length === 0 && <p className="ontology-muted">{s.auditEmpty}</p>}
        {rows.length > 0 && <div className="ontology-table-wrap"><table><thead>
            <tr><th>Action</th><th>Status</th><th>{s.exitCode}</th><th>{s.duration}</th><th>Actor</th><th>Time</th><th>Detail</th></tr></thead><tbody>
            {rows.map(row => <tr key={row.id}>
                <th scope="row"><code>{row.action}</code></th>
                <td><span className={`ontology-tag ontology-status-${row.status}`}>{row.status}</span></td>
                <td>{row.exit_code}</td>
                <td>{row.duration_ms}ms</td>
                <td>{row.actor ?? '—'}</td>
                <td>{row.created_at ? <time dateTime={row.created_at}>{row.created_at}</time> : '—'}</td>
                <td>{row.detail ? <code className="ontology-loc">{row.detail.length > 200 ? `${row.detail.slice(0, 200)}…` : row.detail}</code> : '—'}</td>
            </tr>)}
        </tbody></table></div>}
    </div>;
}

function OverviewPanel({ summary, onOpen }: { summary: OntologySummary; onOpen: (kind: OntologyKind) => void }) {
    return <div className="ontology-panel">
        <div className="ontology-metrics">
            {ONTOLOGY_KINDS.map(kind => <button key={kind} type="button" className="ontology-metric" onClick={() => onOpen(kind)}>
                <span>{s.tabs[kind]}</span>
                <strong>{kindCount(summary, kind)}</strong>
            </button>)}
        </div>
        <p className="ontology-muted">{s.definitionFile}: <code>{summary.definition_file}</code></p>
    </div>;
}

export default function OntologyWorkspace({ project, active, source }: OntologyWorkspaceProps): JSX.Element {
    const [tab, setTab] = useState<OntologyTab>('overview');
    const [selected, setSelected] = useState<{ kind: OntologyKind; name: string }>();
    const [revision, setRevision] = useState(0);
    const [summary, setSummary] = useState<Loadable<OntologySummary>>(idle);
    const [lists, setLists] = useState<Partial<Record<OntologyKind, Loadable<OntologyListResult>>>>({});
    const [detail, setDetail] = useState<Loadable<OntologyElementDetail>>(idle);
    const [definition, setDefinition] = useState<Loadable<OntologyDefinition>>(idle);
    const [audit, setAudit] = useState<Loadable<OntologyAuditRow[]>>(idle);
    const mounted = useRef(true);
    const ticket = useRef(0);
    useEffect(() => { mounted.current = true; return () => { mounted.current = false; ticket.current += 1; }; }, []);

    const isKindTab = (value: OntologyTab): value is OntologyKind => ONTOLOGY_KINDS.includes(value as OntologyKind);

    const loadSummary = useCallback(async () => {
        const t = ++ticket.current;
        setSummary(loading);
        try {
            const data = await source.summary(project);
            if (mounted.current && ticket.current === t) setSummary(ready(data));
        } catch (error) {
            if (mounted.current && ticket.current === t) setSummary(failed(error));
        }
    }, [project, source]);

    const loadList = useCallback(async (kind: OntologyKind) => {
        const t = ++ticket.current;
        setLists(prev => ({ ...prev, [kind]: loading }));
        try {
            const data = await source.list(project, kind, { limit: 500 });
            if (mounted.current && ticket.current === t) setLists(prev => ({ ...prev, [kind]: ready(data) }));
        } catch (error) {
            if (mounted.current && ticket.current === t) setLists(prev => ({ ...prev, [kind]: failed(error) }));
        }
    }, [project, source]);

    const loadDetail = useCallback(async (kind: OntologyKind, name: string) => {
        const t = ++ticket.current;
        setDetail(loading);
        try {
            const data = await source.detail(project, kind, name);
            if (mounted.current && ticket.current === t) {
                setDetail(data['error'] ? { phase: 'error', error: s.notFound } : ready(data));
            }
        } catch (error) {
            if (mounted.current && ticket.current === t) setDetail(failed(error));
        }
    }, [project, source]);

    const loadDefinition = useCallback(async () => {
        const t = ++ticket.current;
        setDefinition(loading);
        try {
            const data = await source.definition(project);
            if (mounted.current && ticket.current === t) setDefinition(ready(data));
        } catch (error) {
            if (mounted.current && ticket.current === t) setDefinition(failed(error));
        }
    }, [project, source]);

    const loadAudit = useCallback(async () => {
        const t = ++ticket.current;
        setAudit(loading);
        try {
            const data = await source.audit(project);
            if (mounted.current && ticket.current === t) setAudit(ready(data));
        } catch (error) {
            if (mounted.current && ticket.current === t) setAudit(failed(error));
        }
    }, [project, source]);

    const refreshAll = useCallback(() => {
        setLists({});
        setDetail(idle);
        setDefinition(idle);
        setAudit(idle);
        void loadSummary();
        if (isKindTab(tab)) void loadList(tab);
        if (tab === 'definition') void loadDefinition();
        if (tab === 'audit') void loadAudit();
        if (selected && isKindTab(tab) && selected.kind === tab) void loadDetail(selected.kind, selected.name);
    }, [tab, selected, loadSummary, loadList, loadDefinition, loadAudit, loadDetail]);

    /* First activation: load the summary; further visits refresh lazily. */
    const started = useRef(false);
    useEffect(() => {
        if (!active || !project || started.current) return;
        started.current = true;
        void loadSummary();
    }, [active, project, loadSummary]);
    useEffect(() => { started.current = false; }, [project]);

    /* Lazy per-tab loading. */
    useEffect(() => {
        if (!active || !project) return;
        if (isKindTab(tab) && lists[tab] === undefined) void loadList(tab);
        if (tab === 'definition' && definition.phase === 'idle') void loadDefinition();
        if (tab === 'audit' && audit.phase === 'idle') void loadAudit();
    }, [active, project, tab, lists, definition.phase, audit.phase, loadList, loadDefinition, loadAudit]);

    /* Detail follows the selected element. */
    useEffect(() => {
        if (!selected || !active || !project) return;
        void loadDetail(selected.kind, selected.name);
    }, [selected, active, project, loadDetail]);

    const select = (kind: OntologyKind, name: string) => {
        setSelected({ kind, name });
        if (tab !== kind) setTab(kind);
    };

    const moveTab = (event: KeyboardEvent<HTMLButtonElement>, current: OntologyTab) => {
        const index = TABS.indexOf(current);
        const target = event.key === 'ArrowDown' ? TABS[(index + 1) % TABS.length]
            : event.key === 'ArrowUp' ? TABS[(index + TABS.length - 1) % TABS.length]
            : event.key === 'Home' ? TABS[0] : event.key === 'End' ? TABS[TABS.length - 1] : undefined;
        if (!target) return;
        event.preventDefault();
        setTab(target);
        document.getElementById(`ontology-tab-${target}`)?.focus();
    };

    const list = isKindTab(tab) ? lists[tab] : undefined;
    const present = summary.data?.present ?? true;
    const errorText = summary.phase === 'error' ? summary.error
        : list?.phase === 'error' ? list.error
        : detail.phase === 'error' ? detail.error : '';

    return <section className="ontology-workspace" aria-label="Ontology" hidden={!active} data-testid="ontology-workspace">
        <header className="ontology-heading">
            <div><p className="ontology-eyebrow">{s.eyebrow}</p><h1>{s.title}</h1><p>{s.subtitle} <span className="ontology-muted">{project}</span></p></div>
            <div className="ontology-heading-actions">
                <button type="button" onClick={() => { setRevision(v => v + 1); refreshAll(); }} disabled={summary.phase === 'loading'}>
                    {summary.phase === 'loading' ? s.refreshing : s.refresh}
                </button>
            </div>
        </header>
        <div className="ontology-layout">
            <div className="ontology-tabs" role="tablist" aria-label="Ontology views" aria-orientation="vertical">
                {TABS.map(name => <button key={name} id={`ontology-tab-${name}`} type="button" role="tab"
                    aria-selected={tab === name} aria-controls={`ontology-panel-${name}`}
                    tabIndex={tab === name ? 0 : -1}
                    onKeyDown={event => moveTab(event, name)}
                    onClick={() => setTab(name)}>{s.tabs[name]}</button>)}
            </div>
            <div className="ontology-view" data-revision={revision}>
                {summary.phase === 'loading' && <p className="ontology-muted">{s.loading}</p>}
                {summary.phase === 'ready' && summary.data && !present && tab !== 'definition' && <div className="ontology-panel ontology-empty">
                    <h3>{s.absentTitle}</h3><p>{s.absentBody}</p>
                </div>}
                {errorText && <p className="ontology-error" role="alert">{errorText}</p>}
                {present && <div id="ontology-panel-overview" role="tabpanel" aria-labelledby="ontology-tab-overview" hidden={tab !== 'overview'}>
                    {summary.data && <OverviewPanel summary={summary.data} onOpen={kind => setTab(kind)} />}
                </div>}
                {present && ONTOLOGY_KINDS.map(kind => <div key={kind} id={`ontology-panel-${kind}`} role="tabpanel" aria-labelledby={`ontology-tab-${kind}`} hidden={tab !== kind}>
                    <div className="ontology-split">
                        <nav className="ontology-list" aria-label={s.tabs[kind]}>
                            {list?.phase === 'loading' && <p className="ontology-muted">{s.loading}</p>}
                            {list?.phase === 'ready' && list.data && <>
                                {list.data.elements.length === 0 && <p className="ontology-muted">{s.empty}</p>}
                                <ul>{list.data.elements.map(el => <li key={el.qualified_name || el.name}>
                                    <button type="button"
                                        className={selected?.kind === kind && selected.name === el.name ? 'ontology-item ontology-item-active' : 'ontology-item'}
                                        onClick={() => select(kind, el.name)}>
                                        <span className="ontology-item-name">{el.name}</span>
                                        <span className="ontology-item-desc">{elementDescription(el)}</span>
                                    </button>
                                </li>)}</ul>
                                <p className="ontology-muted">{s.total(list.data.total)}</p>
                            </>}
                        </nav>
                        <div className="ontology-detail-host">
                            {detail.phase === 'loading' && <p className="ontology-muted">{s.loadingDetail}</p>}
                            {detail.phase === 'ready' && detail.data && selected?.kind === kind &&
                                <ElementDetail detail={detail.data} source={source} project={project}
                                    onExecuted={() => { setAudit(idle); if (tab === 'audit') void loadAudit(); }} />}
                            {detail.phase !== 'ready' && detail.phase !== 'loading' && !detail.error &&
                                <p className="ontology-muted">{s.selectElement}</p>}
                        </div>
                    </div>
                </div>)}
                <div id="ontology-panel-definition" role="tabpanel" aria-labelledby="ontology-tab-definition" hidden={tab !== 'definition'}>
                    <DefinitionPanel definition={definition} source={source} project={project} />
                </div>
                <div id="ontology-panel-audit" role="tabpanel" aria-labelledby="ontology-tab-audit" hidden={tab !== 'audit'}>
                    <AuditPanel audit={audit} />
                </div>
            </div>
        </div>
    </section>;
}
