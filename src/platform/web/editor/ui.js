// UI primitives shared by every panel: the anchored quick form, the
// status-line spans, and the §3 identifier validator every naming dialog
// applies.

// ---- quick form (inline prompt() replacement) -------------------------------

// A small anchored form: all fields at once, Enter submits, Escape or a
// click elsewhere cancels. onSubmit returns an error string to show inline
// (keeping the form open) or null to accept.
const quickForm = document.getElementById('quickForm');
let quickFormClose = null;

export function openQuickForm(anchor, title, fields, onSubmit) {
  quickForm.textContent = '';
  const heading = document.createElement('h4');
  heading.textContent = title;
  quickForm.appendChild(heading);
  const inputs = new Map();
  for (const field of fields) {
    if (field.note) {
      const note = document.createElement('div');
      note.className = 'note';
      note.textContent = field.note;
      quickForm.appendChild(note);
      continue;
    }
    const row = document.createElement('div');
    row.className = 'row';
    const label = document.createElement('label');
    label.textContent = field.label;
    row.appendChild(label);
    let input;
    if (field.options) {
      input = document.createElement('select');
      for (const option of field.options) {
        const el = document.createElement('option');
        el.value = el.textContent = option;
        input.appendChild(el);
      }
      input.value = field.value ?? field.options[0];
    } else {
      input = document.createElement('input');
      input.value = field.value ?? '';
      input.spellcheck = false;
    }
    inputs.set(field.key, input);
    row.appendChild(input);
    quickForm.appendChild(row);
  }
  const error = document.createElement('div');
  error.className = 'err';
  error.hidden = true;
  quickForm.appendChild(error);
  const buttons = document.createElement('div');
  buttons.className = 'buttons';
  const cancel = document.createElement('button');
  cancel.textContent = 'Cancel';
  const ok = document.createElement('button');
  ok.textContent = 'OK';
  buttons.appendChild(cancel);
  buttons.appendChild(ok);
  quickForm.appendChild(buttons);

  const close = () => {
    quickForm.hidden = true;
    quickFormClose = null;
  };
  quickFormClose = close;
  cancel.onclick = close;
  const submit = () => {
    const values = Object.fromEntries(
        [...inputs].map(([key, input]) => [key, input.value.trim()]));
    const problem = onSubmit(values);
    if (problem) {
      error.textContent = problem;
      error.hidden = false;
      return;
    }
    close();
  };
  ok.onclick = submit;
  quickForm.onkeydown = (e) => {
    if (e.key === 'Enter') {
      e.preventDefault();
      submit();
    } else if (e.key === 'Escape') {
      close();
    }
    e.stopPropagation();
  };
  quickForm.hidden = false;
  quickForm.style.left =
      Math.min(anchor.left, innerWidth - 270) + 'px';
  quickForm.style.top = Math.min(anchor.bottom + 4, innerHeight - 220) + 'px';
  const first = inputs.values().next().value;
  first?.focus();
  first?.select?.();
}

window.addEventListener('mousedown', (e) => {
  if (quickFormClose && !quickForm.contains(e.target)) {
    quickFormClose();
  }
}, true);

export function setStatus(id, text, cls) {
  const el = document.getElementById(id);
  el.textContent = text;
  el.className = cls || '';
}

export const isIdentifier = (name) => /^[A-Za-z_][A-Za-z0-9_]*$/.test(name);
