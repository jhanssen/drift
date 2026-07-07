import { openQuickForm } from '../ui.js';
import { pushFromGraph } from '../document.js';
import { nodePropDefs } from '../nodedefs.js';
import { activeSequenceId, setActiveSequenceId,
         refreshTimeline } from '../timeline.js';
import { graph, selectedNodeId } from './state.js';
import { parseParamValue, deleteSelectedNode } from './interact.js';

// ---- graph view: inspector -------------------------------------------------

export function renderInspector() {
  const section = document.getElementById('nodeSection');
  const node = graph?.byId.get(selectedNodeId);
  if (node?.source?.type === 'sequence' && activeSequenceId !== node.id) {
    setActiveSequenceId(node.id); // selecting a sequence focuses its timeline
    refreshTimeline();
  }
  section.hidden = !node;
  if (!node) {
    return;
  }
  const info = document.getElementById('nodeInfo');
  info.textContent = '';
  const row = (label, text, title) => {
    const div = document.createElement('div');
    div.className = 'row';
    const labelEl = document.createElement('label');
    labelEl.textContent = label;
    labelEl.title = label;
    const span = document.createElement('span');
    span.textContent = text;
    span.title = title ?? text;
    div.appendChild(labelEl);
    div.appendChild(span);
    info.appendChild(div);
  };
  const heading = (text) => {
    const h = document.createElement('h3');
    h.textContent = text;
    info.appendChild(h);
  };
  const fmt = (v) => {
    const text = JSON.stringify(v);
    return text.length > 36 ? text.slice(0, 35) + '…' : text;
  };
  row('id', node.id);
  row('type', node.type);
  const props = Object.entries(node.source)
      .filter(([key]) => key !== 'id' && key !== 'type' && key !== 'inputs');
  const unsetProps = nodePropDefs(node.source.type)
      .filter((p) => p.default !== undefined && !(p.name in node.source));
  if (props.length || unsetProps.length) {
    heading('properties');
    for (const [key, value] of props) {
      row(key, fmt(value), JSON.stringify(value));
    }
    for (const p of unsetProps) {
      row(p.name, `(default ${JSON.stringify(p.default)})`);
    }
  }
  const inputs = node.inputs.filter((input) => !input.append);
  if (inputs.length) {
    heading('inputs');
    for (const input of inputs) {
      row(input.name, input.conn
          ? `@${input.conn.node}.${input.conn.port ?? ''}`.replace(/\.$/, '') +
            (input.conn.previous ? ' (previous)' : '')
          : input.param ? '$' + input.param
          : input.unbound
              ? (input.def !== undefined
                     ? `(default ${JSON.stringify(input.def)})` : '(default)')
          : fmt(input.literal));
    }
  }
  if (node.outputs.length) {
    heading('outputs');
    for (const [name, category] of node.outputs) {
      row(name, category);
    }
  }
  if (node.param) {
    // The declaration (default/min/max) is document state, editable here;
    // the panel's slider edits the live value.
    const edit = document.createElement('button');
    edit.textContent = 'Edit declaration…';
    edit.style.marginTop = '10px';
    edit.onclick = (e) => {
      const decl = node.decl;
      const arity =
          { scalar: 1, vec2: 2, vec3: 3, vec4: 4 }[decl.type] ?? 1;
      const fields = [
        { key: 'default', label: 'default',
          value: Array.isArray(decl.default) ? decl.default.join(', ')
                                             : String(decl.default ?? 0) },
      ];
      if (arity === 1) {
        fields.push({ key: 'min', label: 'min',
                      value: decl.min !== undefined ? String(decl.min)
                                                    : '' });
        fields.push({ key: 'max', label: 'max',
                      value: decl.max !== undefined ? String(decl.max)
                                                    : '' });
        fields.push({ note: 'empty min/max removes the bounds' });
      }
      openQuickForm(e.target.getBoundingClientRect(), '$' + node.param,
                    fields, (v) => {
        const def = parseParamValue(v.default, arity);
        if (def === undefined) {
          return `default: expected ${arity} number(s)`;
        }
        decl.default = def;
        if (arity === 1) {
          if (v.min === '' && v.max === '') {
            delete decl.min;
            delete decl.max;
          } else {
            const min = Number(v.min), max = Number(v.max);
            if (Number.isNaN(min) || Number.isNaN(max) || !(max > min)) {
              return 'min/max must be numbers with max > min';
            }
            decl.min = min;
            decl.max = max;
          }
        }
        pushFromGraph();
        renderInspector();
        return null;
      });
    };
    info.appendChild(edit);
  }
  if (!node.implicit) {
    const del = document.createElement('button');
    del.textContent = 'Delete node';
    del.style.marginTop = '10px';
    del.onclick = deleteSelectedNode;
    info.appendChild(del);
  }
}
