// users-logincount-pie.js
// Loads only the users -> logincount doughnut, creates dynamic legend in #usersLegend,
// and removes the header dropdown (.dropdown no-arrow) so the old demo menu won't show.

console.log('users-logincount-pie (inline) starting...');

(async function main() {
  // wait for Chart to be present
  if (typeof Chart === 'undefined') {
    console.log('Chart.js not available yet, waiting up to 5s...');
    await new Promise(res => {
      const t = setInterval(() => { if (typeof Chart !== 'undefined') { clearInterval(t); res(); } }, 50);
      setTimeout(() => { clearInterval(t); res(); }, 5000);
    });
  }
  if (typeof Chart === 'undefined') {
    console.error('Chart.js not found; aborting pie module.');
    return;
  }

  // Remove the header dropdown (three-dot menu) if present
  try {
    const dropToggle = document.getElementById('dropdownMenuLink');
    if (dropToggle) {
      const dropParent = dropToggle.closest('.dropdown');
      if (dropParent) {
        dropParent.parentNode && dropParent.parentNode.removeChild(dropParent);
        console.log('users-logincount-pie: removed header dropdown.');
      } else {
        // fallback: hide
        dropToggle.style.display = 'none';
        console.log('users-logincount-pie: hid dropdown toggle (no parent dropdown found).');
      }
    } else {
      // also support selectors if id not present
      const dd = document.querySelector('.card-header .dropdown.no-arrow');
      if (dd) { dd.remove(); console.log('users-logincount-pie: removed header dropdown (by selector)'); }
    }
  } catch (e) {
    console.warn('users-logincount-pie: error removing dropdown:', e);
  }

  // Replace canvas (clean slate) and destroy any existing Chart instance
  const existingCanvas = document.getElementById('myPieChart');
  if (!existingCanvas) {
    console.error('users-logincount-pie: canvas #myPieChart not found. Aborting.');
    return;
  }
  const parent = existingCanvas.parentNode;
  const canvas = document.createElement('canvas');
  canvas.id = 'myPieChart';
  parent.replaceChild(canvas, existingCanvas);
  const ctx = canvas.getContext('2d');

  try {
    if (typeof Chart.getChart === 'function') {
      const inst = Chart.getChart(canvas);
      if (inst && typeof inst.destroy === 'function') { inst.destroy(); console.log('Destroyed existing Chart (v3).'); }
    } else if (Chart.instances) {
      Object.values(Chart.instances).forEach(i => { if (i && i.canvas && i.canvas.id === 'myPieChart' && typeof i.destroy === 'function') i.destroy(); });
    }
    if (window.userLoginPieChart && typeof window.userLoginPieChart.destroy === 'function') {
      window.userLoginPieChart.destroy();
      window.userLoginPieChart = null;
    }
  } catch (e) {
    console.warn('users-logincount-pie: error destroying existing chart', e);
  }

  // Try importing firebase-config from a few likely paths (this file is ./ relative to dashboardadmin.html)
  let cfgModule = null;
  const candidatePaths = ['./firebase-config.js','../firebase-config.js','/firebase-config.js','./js/firebase-config.js','../js/firebase-config.js','../../firebase-config.js'];
  for (const p of candidatePaths) {
    try {
      const m = await import(p);
      // prefer named export `db`, else default
      if (m && (m.db || (m.default && m.default.db))) {
        cfgModule = m.db ? m : (m.default ? m.default : null);
        console.log('users-logincount-pie: loaded firebase-config from', p);
        break;
      }
    } catch (_) { /* ignore and try next */ }
  }
  if (!cfgModule || !cfgModule.db) {
    console.error('users-logincount-pie: could not import firebase-config.js. Tried:', candidatePaths.join(', '));
    renderPlaceholder(['Config missing'], [1]);
    return;
  }
  const db = cfgModule.db;

  // Import Firestore runtime functions
  let fs = null;
  try {
    fs = await import('https://www.gstatic.com/firebasejs/12.1.0/firebase-firestore.js');
  } catch (err) {
    console.error('users-logincount-pie: failed to import firestore SDK:', err);
    renderPlaceholder(['SDK error'], [1]);
    return;
  }
  const { collection, getDocs, onSnapshot } = fs;

  // palette (keeps consistent with earlier chart colors)
  const palette = ['#4e73df','#1cc88a','#36b9cc','#f6c23e','#e74a3b','#858796','#6f42c1','#20c997'];
  const paletteHover = ['#2e59d9','#17a673','#2c9faf','#d4a117','#c43c2a','#6c6c6c','#4e2ea8','#1a9b75'];

  // build labels/values from docs
  function buildFromDocs(docs) {
    const arr = [];
    docs.forEach(d => {
      const data = (typeof d.data === 'function') ? d.data() : (d || {});
      const uid = d.id || 'unknown';
      const username = data && data.username ? data.username : uid;
      let cnt = 0;
      if (data && typeof data.logincount === 'number') cnt = data.logincount;
      else if (data && data.logincount) {
        const p = Number(data.logincount); cnt = Number.isFinite(p) ? p : 0;
      }
      arr.push({ username, cnt });
    });
    arr.sort((a,b) => b.cnt - a.cnt);

    const MAX_SLICES = 8;
    const labels = [], values = [];
    let other = 0;
    for (let i=0;i<arr.length;i++) {
      if (i < MAX_SLICES - 1) { labels.push(arr[i].username); values.push(arr[i].cnt); }
      else other += arr[i].cnt;
    }
    if (arr.length > 0 && other > 0) { labels.push('Other'); values.push(other); }

    const total = values.reduce((s,v)=>s+v,0);
    if (arr.length === 0 || total === 0) return { labels: ['No logins yet'], values: [1], isPlaceholder:true };
    return { labels, values, isPlaceholder:false };
  }

  // chart variable
  let chart = null;

function createOrUpdate(data) {
  const bg = data.isPlaceholder ? ['#e9ecef'] : data.labels.map((_,i) => palette[i % palette.length]);
  const bgHover = data.isPlaceholder ? ['#d1d3d8'] : data.labels.map((_,i) => paletteHover[i % paletteHover.length]);

  const chartCfg = {
    type: 'doughnut',
    data: { labels: data.labels, datasets: [{ data: data.values, backgroundColor: bg, hoverBackgroundColor: bgHover, hoverBorderColor: 'rgba(234,236,244,1)' }] },
    options: {
      maintainAspectRatio: false,
      // support both Chart v2 and v3 names for cutout
      cutout: '80%',
      cutoutPercentage: 80,
      // disable Chart's built-in legend for both v2 and v3
      legend: { display: false },            // Chart.js v2
      plugins: { legend: { display: false } }, // Chart.js v3
      tooltips: {
        backgroundColor: "rgb(255,255,255)",
        bodyFontColor: "#858796",
        borderColor: '#dddfeb',
        borderWidth: 1,
        xPadding: 15, yPadding: 15,
        displayColors: false, caretPadding: 10,
        callbacks: {
          label: function(tooltipItem, data) {
            const label = data.labels[tooltipItem.index] || '';
            const value = data.datasets[0].data[tooltipItem.index] || 0;
            return label + ': ' + value;
          }
        }
      }
    }
  };

  if (chart) {
    chart.data.labels = data.labels;
    chart.data.datasets[0].data = data.values;
    chart.data.datasets[0].backgroundColor = bg;
    chart.data.datasets[0].hoverBackgroundColor = bgHover;
    chart.update();
  } else {
    chart = new Chart(ctx, chartCfg);
  }

  window.userLoginPieChart = chart;
  renderLegend(data.labels, bg, data.values, data.isPlaceholder);
}


  function renderLegend(labels, bgColors, values, isPlaceholder=false) {
    const el = document.getElementById('usersLegend');
    if (!el) return;
    el.innerHTML = '';
    if (!labels || labels.length === 0) return;
    labels.forEach((label, idx) => {
      const li = document.createElement('span');
      li.className = 'legend-item';
      const color = bgColors[idx] || '#ccc';
      const count = (values && values[idx] !== undefined) ? values[idx] : '';
      li.innerHTML = `<span class="legend-dot" style="background:${color}"></span><span class="legend-label">${escapeHtml(String(label))}</span><span class="legend-count">${count !== '' ? ' ' + escapeHtml(String(count)) : ''}</span>`;
      el.appendChild(li);
    });
  }

  // small sanitize for label insertion
  function escapeHtml(str) {
    return str.replace(/[&<>"']/g, (c) => ({'&':'&amp;','<':'&lt;','>':'&gt;','"':'&quot;',"'":'&#39;'})[c]);
  }

  // helper to render placeholder single-slice chart
  function renderPlaceholder(labels, values) {
    createOrUpdate({ labels, values, isPlaceholder:true });
  }

  // initial load
  try {
    console.log('users-logincount-pie: fetching initial users...');
    const usersCol = collection(db, 'users'); // if your collection is singular, change 'users' -> 'user'
    const snap = await getDocs(usersCol);
    const docs = [];
    snap.forEach(d => docs.push(d));
    const payload = buildFromDocs(docs);
    createOrUpdate(payload);
    console.log('users-logincount-pie: initial render complete. users=', docs.length);
  } catch (err) {
    console.error('users-logincount-pie: initial getDocs failed:', err);
    renderPlaceholder(['Fetch error'], [1]);
  }

  // realtime updates
  try {
    const usersCol = collection(db, 'users');
    onSnapshot(usersCol, (qs) => {
      const docs = []; qs.forEach(d => docs.push(d));
      const payload = buildFromDocs(docs);
      createOrUpdate(payload);
      console.log('users-logincount-pie: snapshot update users=', docs.length, ' total=', payload.values.reduce((s,v)=>s+v,0));
    }, (err) => {
      console.error('users-logincount-pie: onSnapshot error:', err);
    });
    console.log('users-logincount-pie: realtime listener attached.');
  } catch (err) {
    console.warn('users-logincount-pie: failed to attach realtime listener:', err);
  }
})().catch(e => console.error('users-logincount-pie: unexpected error', e));
