// drift editor — entry point. Modules wire their own DOM handlers as they
// evaluate; whatever must run only after every module is initialized (view
// deep links, auto-connect, the shared render ticker) lives here.

import { query } from './config.js';
import { connectLive } from './live.js';
import { tickTimelines } from './timeline.js';
import { updateGraphChrome } from './graph/state.js';
import './preview.js';
import './params.js';
import './document.js';
import './project.js';
import './transport.js';
import './graph/model.js';
import './graph/build.js';
import './graph/wiring.js';
import './graph/subgraphs.js';
import './graph/interact.js';
import './graph/draw.js';
import './graph/inspector.js';

function setView(showGraph) {
  document.body.classList.toggle('graph-mode', showGraph);
  document.getElementById('tabPreview').classList.toggle('active', !showGraph);
  document.getElementById('tabGraph').classList.toggle('active', showGraph);
  document.getElementById('graphHint').hidden = !showGraph;
  document.getElementById('addNodeBtn').hidden = !showGraph;
  updateGraphChrome();
}
document.getElementById('tabPreview').onclick = () => setView(false);
document.getElementById('tabGraph').onclick = () => setView(true);
if (query.get('view') === 'graph') {
  setView(true);
}

// ?connect=PORT auto-connects (handy for tests and a fixed dev setup).
if (query.get('connect')) {
  connectLive(query.get('connect'));
}

tickTimelines();
