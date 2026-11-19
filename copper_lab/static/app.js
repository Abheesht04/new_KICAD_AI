const $ = (id) => document.getElementById(id);
let activeRun = null;
let activeManifest = null;
let toastTimer = null;

function esc(value) {
  return String(value ?? '').replace(/[&<>"']/g, (c) => ({'&':'&amp;','<':'&lt;','>':'&gt;','"':'&quot;',"'":'&#39;'}[c]));
}
function prettyStatus(value) {
  return String(value || 'unknown').replaceAll('_',' ').toUpperCase();
}
function showToast(message) {
  const node = $('toast'); node.textContent = message; node.classList.add('show');
  clearTimeout(toastTimer); toastTimer = setTimeout(() => node.classList.remove('show'), 2600);
}
function setStatusCell(id, available, detail) {
  const node = $(id); node.classList.toggle('ok', !!available); node.classList.toggle('bad', !available);
  node.querySelector('b').textContent = detail || (available ? 'Connected' : 'Unavailable');
}
async function refreshStatus() {
  $('statusNote').textContent = 'Refreshing local tools…';
  try {
    const r = await fetch('/api/status', {cache:'no-store'}); const s = await r.json();
    setStatusCell('designStatus', true, 'Manus');
    setStatusCell('copperStatus', s.copper.available, s.copper.available ? 'Release found' : 'Not found');
    setStatusCell('kicadStatus', s.kicad.available, s.kicad.available ? 'CLI ready' : 'Not found');
    $('statusNote').textContent = (s.copper.available && s.kicad.available) ? 'Ready: paste a design intent from Manus' : 'Copper or KiCad CLI is missing';
  } catch (e) {
    setStatusCell('copperStatus', false, 'Unknown'); setStatusCell('kicadStatus', false, 'Unknown');
    $('statusNote').textContent = 'Could not reach Copper Lab API';
  }
}
async function refreshRuns() {
  try {
    const r=await fetch('/api/runs'); const data=await r.json(); renderHistory(data.runs || []);
  } catch(e) { $('historyList').innerHTML='<div class="history-empty">Run history unavailable.</div>'; }
}
function renderHistory(runs) {
  $('historyCount').textContent = runs.length;
  if (!runs.length) { $('historyList').innerHTML='<div class="history-empty">No runs yet.</div>'; return; }
  $('historyList').innerHTML = runs.map(run => {
    const active = activeRun === run.run_id ? ' active' : '';
    const label = (run.run_id || '').split('_').slice(0,2).join(' · ');
    return `<button class="history-item${active}" data-run="${esc(run.run_id)}"><strong>${esc(run.author ? 'Design by ' + run.author : 'Copper run')}</strong><small>${esc(label)}${run.elapsed_seconds ? ` · ${run.elapsed_seconds}s` : ''}</small><span class="history-state">${esc(prettyStatus(run.status))}</span></button>`;
  }).join('');
  document.querySelectorAll('[data-run]').forEach(btn => btn.addEventListener('click', () => loadRun(btn.dataset.run)));
}
function bytesLabel(n) {
  if (n < 1024) return `${n} B`; if (n < 1024*1024) return `${(n/1024).toFixed(1)} KB`; return `${(n/1024/1024).toFixed(2)} MB`;
}
function urlFor(name) { return `/api/file?id=${encodeURIComponent(activeRun)}&name=${encodeURIComponent(name)}`; }
async function fileText(name) {
  const r = await fetch(urlFor(name)); if (!r.ok) throw new Error(`Could not open ${name}`); return await r.text();
}
function findFile(regex) { return (activeManifest?.files || []).find(x => regex.test(x.name)); }
async function loadCodePreview(regex, target, infoTarget, imageTarget, previewName) {
  const file = findFile(regex);
  const imageFile = findFile(new RegExp(`^${previewName}\\.svg$`, 'i'));
  const image = $(imageTarget), code = $(target);
  if (imageFile) {
    image.src = urlFor(imageFile.name); image.classList.remove('hidden'); code.classList.add('hidden');
    $(infoTarget).textContent = `${bytesLabel(imageFile.size)} · visual preview`;
    return;
  }
  image.classList.add('hidden'); code.classList.remove('hidden');
  if (!file) { code.textContent = 'This artifact was not produced for this run.'; $(infoTarget).textContent = 'Not available'; return; }
  try {
    const content = await fileText(file.name); code.textContent = content.slice(0, 18000) + (content.length > 18000 ? '\n\n… preview truncated · open full file to inspect the rest …' : '');
    $(infoTarget).textContent = `${bytesLabel(file.size)} · ${file.name}`;
  } catch(e) { code.textContent = e.message; }
}
function renderArtifacts(files) {
  $('artifactCount').textContent = `${files.length} FILES`;
  if (!files.length) { $('artifactList').innerHTML='<div class="history-empty">No output files were saved.</div>'; return; }
  $('artifactList').innerHTML = files.map(f => `<div class="artifact-item"><span class="artifact-name" title="${esc(f.name)}">${esc(f.name)}</span><span class="artifact-actions"><button data-view="${esc(f.name)}">VIEW</button><a href="${urlFor(f.name)}" download="${esc(f.name)}">↓</a></span></div>`).join('');
  document.querySelectorAll('[data-view]').forEach(btn => btn.addEventListener('click', () => openArtifact(btn.dataset.view)));
}
function renderChecks(checks) {
  $('checksEmpty').classList.toggle('hidden', checks.length > 0);
  $('checksList').classList.toggle('hidden', checks.length === 0);
  $('checksList').innerHTML = checks.map(c => {
    const state = c.status || 'unknown'; const glyph = state === 'passed' ? '✓' : (state === 'skipped' ? '–' : (state === 'findings' ? '!' : '×')); 
    return `<div class="check-row"><span class="check-icon ${esc(state)}">${glyph}</span><span class="check-name">${esc(c.name)}</span><span class="check-state">${esc(state)}</span>${c.output ? `<div class="check-output">${esc(c.output.slice(0,900))}</div>` : ''}</div>`;
  }).join('');
}
function renderRun(manifest) {
  activeManifest=manifest; activeRun=manifest.run_id;
  $('runLabel').textContent = manifest.run_id || 'UNKNOWN RUN';
  $('previewTitle').textContent = manifest.status === 'generation_failed' ? 'Generation attempt' : 'Generated project';
  $('emptyState').classList.add('hidden'); $('previewContent').classList.remove('hidden');
  renderArtifacts(manifest.files || []); renderChecks(manifest.checks || []);
  const failures = (manifest.checks || []).filter(c => ['failed','error','timeout'].includes(c.status)).length;
  const findings = (manifest.checks || []).filter(c => c.status === 'findings').length;
  const skipped = (manifest.checks || []).filter(c => c.status === 'skipped').length;
  const overall=$('overallStatus');
  overall.textContent = failures ? `${failures} FAILED` : ((findings || skipped) ? `${findings + skipped} FINDING(S)` : prettyStatus(manifest.status));
  overall.className = `result-tag ${failures ? 'bad' : ((findings || skipped) ? 'warn' : (manifest.status === 'completed' ? 'ok' : 'warn'))}`;
  $('gateStatus').textContent='NOT APPROVED';
  const schFile=findFile(/\.kicad_sch$/i), pcbFile=findFile(/\.kicad_pcb$/i);
  loadCodePreview(/\.kicad_sch$/i,'schematicPreview','schematicInfo','schematicImage','schematic_preview');
  loadCodePreview(/\.kicad_pcb$/i,'boardPreview','boardInfo','boardImage','board_preview');
  if (!schFile || !pcbFile) {
    $('schematicPreview').textContent = schFile ? $('schematicPreview').textContent : (manifest.error || 'Schematic file was not generated. See candidate/error files below.');
    $('boardPreview').textContent = pcbFile ? $('boardPreview').textContent : (manifest.error || 'PCB file was not generated. See candidate/error files below.');
  }
  refreshRuns();
}
async function loadRun(id) {
  try { const r=await fetch(`/api/run?id=${encodeURIComponent(id)}`); if (!r.ok) throw new Error('Run not found'); renderRun(await r.json()); }
  catch(e) { showToast(e.message); }
}
async function openArtifact(name) {
  try {
    const content=await fileText(name); $('modalTitle').textContent=name; $('modalCode').textContent=content;
    $('modalMeta').textContent=bytesLabel(new Blob([content]).size); $('modalDownload').href=urlFor(name); $('modalDownload').download=name;
    $('fileModal').classList.remove('hidden');
  } catch(e) { showToast(e.message); }
}
async function generate() {
  const prompt=$('promptInput').value.trim();
  const intentText=$('intentInput').value.trim();
  if (!intentText) { showToast('Paste the design intent JSON from Manus first'); $('intentInput').focus(); return; }
  let intent;
  try { intent = JSON.parse(intentText); } catch (e) { showToast('The design intent is not valid JSON: ' + e.message); $('intentInput').focus(); return; }
  const button=$('generateButton'); button.disabled=true; button.querySelector('span:nth-child(2)').textContent='Compiling · validating…';
  $('statusNote').textContent='Copper is compiling the design intent into a native KiCad project, then running validation.';
  $('emptyState').classList.remove('hidden'); $('previewContent').classList.add('hidden'); $('emptyState').innerHTML='<div class="empty-glyph">◌</div><h3>Compiling the project</h3><p>Copper is compiling the native KiCad schematic and PCB from the design intent before validation.</p>';
  try {
    const r=await fetch('/api/compile',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify({prompt,intent})});
    const result=await r.json(); if (!r.ok) throw new Error(result.error || 'Compilation failed');
    renderRun(result);
    if (result.error) showToast(result.error); else showToast('Run finished. Review the generated files and checks.');
    $('statusNote').textContent=`Last run ${result.elapsed_seconds ?? '—'}s · ${prettyStatus(result.status)}`;
  } catch(e) { $('statusNote').textContent='Compile request failed'; showToast(e.message); $('emptyState').innerHTML=`<div class="empty-glyph">!</div><h3>Run could not finish</h3><p>${esc(e.message)}</p>`; }
  finally { button.disabled=false; button.querySelector('span:nth-child(2)').textContent='Compile & validate'; refreshRuns(); }
}
$('generateButton').addEventListener('click', generate);
$('refreshStatus').addEventListener('click', () => {refreshStatus(); showToast('Tool status refreshed');});
$('promptInput').addEventListener('input', () => $('charCount').textContent=`${$('promptInput').value.length.toLocaleString()} / 12,000`);
$('intentInput').addEventListener('input', () => $('intentCount').textContent=`${new Blob([$('intentInput').value]).size.toLocaleString()} B`);
$('exampleButton').addEventListener('click', () => {
  $('promptInput').value='5 V red LED indicator: two-pin power input, 1k current-limiting resistor, red 0603 LED.';
  $('intentInput').value='{\n  "schema": "copper.design-intent",\n  "schema_version": 1,\n  "project": {"name": "LedIndicator", "board": {"width_mm": 40, "height_mm": 25, "layers": 2}},\n  "components": [\n    {"reference": "J1", "value": "5V_IN", "symbol_id": "Connector_Generic:Conn_01x02",\n     "footprint_id": "Connector_PinHeader_2.54mm:PinHeader_1x02_P2.54mm_Vertical",\n     "pins": [{"symbol_pin": "1", "pad": "1", "net": "VIN"}, {"symbol_pin": "2", "pad": "2", "net": "GND"}],\n     "position": {"x_mm": 8, "y_mm": 12.5, "rotation_deg": 0}},\n    {"reference": "R1", "value": "1k", "symbol_id": "Device:R", "footprint_id": "Resistor_SMD:R_0603_1608Metric",\n     "pins": [{"symbol_pin": "1", "pad": "1", "net": "VIN"}, {"symbol_pin": "2", "pad": "2", "net": "LED_A"}],\n     "position": {"x_mm": 20, "y_mm": 12.5, "rotation_deg": 0}},\n    {"reference": "D1", "value": "RED", "symbol_id": "Device:LED", "footprint_id": "LED_SMD:LED_0603_1608Metric",\n     "pins": [{"symbol_pin": "1", "pad": "1", "net": "GND"}, {"symbol_pin": "2", "pad": "2", "net": "LED_A"}],\n     "position": {"x_mm": 31, "y_mm": 12.5, "rotation_deg": 0}}\n  ]\n}';
  $('charCount').textContent=`${$('promptInput').value.length} / 12,000`;
  $('intentCount').textContent=`${new Blob([$('intentInput').value]).size.toLocaleString()} B`; $('intentInput').focus();
});
document.querySelectorAll('[data-open-source]').forEach(btn => btn.addEventListener('click', () => {
  const f=findFile(btn.dataset.openSource==='sch'?/\.kicad_sch$/i:/\.kicad_pcb$/i); if (f) openArtifact(f.name); else showToast('Native project file is not available in this run');
}));
$('closeModal').addEventListener('click', () => $('fileModal').classList.add('hidden'));
$('fileModal').addEventListener('click', e => { if (e.target === $('fileModal')) $('fileModal').classList.add('hidden'); });
refreshStatus(); refreshRuns();
