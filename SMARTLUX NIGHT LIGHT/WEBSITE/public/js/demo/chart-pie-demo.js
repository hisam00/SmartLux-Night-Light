// js/demo/chart-pie-demo.js
// Robust module: destroys any existing example chart and then renders users' logincounts.
// Load this with: <script type="module" src="js/demo/chart-pie-demo.js"></script>

console.log('chart-pie-demo module start');

(async () => {
  // --- Helpers to destroy any existing chart attached to the canvas ---
  const canvas = document.getElementById('myPieChart');
  if (!canvas) {
    console.error('chart-pie-demo: canvas #myPieChart not found. Aborting.');
    return;
  }

  // If previous code created a global myPieChart, destroy it
  try {
    if (window.myPieChart && typeof window.myPieChart.destroy === 'function') {
      window.myPieChart.destroy();
      console.log('chart-pie-demo: destroyed window.myPieChart');
    }
  } catch (e) {
    console.warn('chart-pie-demo: error destroying window.myPieChart', e);
  }

  // If Chart.js has an existing instance for this canvas, destroy it
  try {
    if (typeof Chart !== 'undefined') {
      // v3: Chart.getChart(canvas)
      const existing = (typeof Chart.getChart === 'function') ? Chart.getChart(canvas) :
                       (Chart.instances ? Object.values(Chart.instances).find(c => c && c.canvas && c.canvas.id === 'myPieChart') : null);
      if (existing && typeof existing.destroy === 'function') {
        existing.destroy();
        console.log('chart-pie-demo: destroyed Chart instance attached to canvas');
      }
    }
  } catch (e) {
    console.warn('chart-pie-demo: error destroying Chart instance', e);
  }

  // --- Dynamic import of firebase-config (try a few possible relative paths) ---
  let firebaseCfg = null;
  const candidatePaths = ['../../firebase-config.js', '../firebase-config.js', './firebase-config.js'];
  for (const p of candidatePaths) {
    try {
      firebaseCfg = await import(p);
      if (firebaseCfg && (firebaseCfg.db || firebaseCfg.default || firebaseCfg.auth)) {
        console.log('chart-pie-demo: imported firebase-config from', p);
        break;
      }
    } catch (err) {
      // ignore and try next
    }
  }

  if (!firebaseCfg || !firebaseCfg.db) {
    console.error('chart-pie-demo: could not import firebase-config.js (looked in', candidatePaths.join(', '), ').');
    // render a visible placeholder chart so UI isn't blank
    renderPlaceholder(['Config missing'], [1], true);
    return;
  }

  const db = firebaseCfg.db;

  // load Firestore functions dynamically (same SDK version your other imports use)
  let fstore = null;
  try {
    fstore = await import('https://www.gstatic.com/firebasejs/12.1.0/firebase-firestore.js');
  } catch (err) {
    console.error('chart-pie-demo: failed to import firebase-firestore SDK:', err);
    renderPlaceholder(['SDK error'], [1], true);
    return;
  }

  const { collection, getDocs, onSnapshot } = fstore;

  const ctx = canvas.getContext('2d');
  let doughnut = null;
  window.myPieChart = null; // ensure global reference cleared

  const MAX_SLICES = 8;

  function buildChartDataFromDocs(docsArray) {
    const items = [];
    docsArray.forEach(docSnap => {
      const data = (typeof docSnap.data === 'function') ? docSnap.data() : (docSnap || {});
      const uid = docSnap.id || (data && data.uid) || 'unknown';
      const username = (data && data.username) ? data.username : uid;
      let cnt = 0;
      if (data && typeof data.logincount === 'number') cnt = data.logincount;
      else if (data && data.logincount) {
        const p = Number(data.logincount);
        cnt = Number.isFinite(p) ? p : 0;
      }
      items.push({ username, cnt });
    });

    items.sort((a,b) => b.cnt - a.cnt);

    const labels = [];
    const values = [];
    let other = 0;
    for (let i = 0; i < items.length; i++) {
      if (i < MAX_SLICES - 1) {
        labels.push(items[i].username);
        values.push(items[i].cnt);
      } else {
        other += items[i].cnt;
      }
    }
    if (items.length > 0 && other > 0) {
      labels.push('Other');
      values.push(other);
    }

    const total = values.reduce((s,v) => s+v, 0);
    if (items.length === 0 || total === 0) {
      return { labels: ['No logins yet'], values: [1], isPlaceholder: true };
    }
    return { labels, values, isPlaceholder: false };
  }

  function createOrUpdateChart(data) {
    const palette = ['#4e73df','#1cc88a','#36b9cc','#f6c23e','#e74a3b','#858796','#6f42c1','#20c997'];
    const hover = ['#2e59d9','#17a673','#2c9faf','#d4a117','#c43c2a','#6c6c6c','#4e2ea8','#1a9b75'];

    const bg = data.isPlaceholder ? ['#e9ecef'] : data.labels.map((_,i) => palette[i % palette.length]);
    const bgHover = data.isPlaceholder ? ['#d1d3d8'] : data.labels.map((_,i) => hover[i % hover.length]);

    if (doughnut) {
      doughnut.data.labels = data.labels;
      doughnut.data.datasets[0].data = data.values;
      doughnut.data.datasets[0].backgroundColor = bg;
      doughnut.data.datasets[0].hoverBackgroundColor = bgHover;
      doughnut.update();
      window.myPieChart = doughnut;
      return;
    }

    doughnut = new Chart(ctx, {
      type: 'doughnut',
      data: {
        labels: data.labels,
        datasets: [{
          data: data.values,
          backgroundColor: bg,
          hoverBackgroundColor: bgHover,
          hoverBorderColor: "rgba(234, 236, 244, 1)",
        }]
      },
      options: {
        maintainAspectRatio: false,
        plugins: {
          tooltip: {
            padding: 8,
            callbacks: {
              label: function(context) {
                const label = context.label || '';
                const val = context.raw || 0;
                return `${label}: ${val}`;
              }
            }
          },
          legend: { display: false }
        },
        cutout: '80%',
      }
    });

    window.myPieChart = doughnut;
  }

  function renderPlaceholder(labels, values, isPlaceholder=false) {
    try {
      createOrUpdateChart({ labels, values, isPlaceholder });
    } catch (e) {
      console.error('chart-pie-demo: failed to render placeholder chart', e);
    }
  }

  // --- initial fetch ---
  try {
    console.log('chart-pie-demo: fetching initial users collection...');
    const usersCol = collection(db, 'users'); // change to 'user' if you use singular
    const snap = await getDocs(usersCol);
    const docs = [];
    snap.forEach(d => docs.push(d));
    const chartData = buildChartDataFromDocs(docs);
    createOrUpdateChart(chartData);
    console.log('chart-pie-demo: initial chart done. users:', docs.length);
  } catch (err) {
    console.error('chart-pie-demo: initial fetch failed:', err);
    renderPlaceholder(['Error fetching users'], [1], true);
  }

  // --- realtime updates ---
  try {
    const usersCol = collection(db, 'users');
    onSnapshot(usersCol, (querySnapshot) => {
      const docs = [];
      querySnapshot.forEach(d => docs.push(d));
      const chartData = buildChartDataFromDocs(docs);
      createOrUpdateChart(chartData);
      const total = chartData.values.reduce((s,v) => s+v, 0);
      console.log('chart-pie-demo: snapshot update: users=', docs.length, ' total logins=', total);
    }, (err) => {
      console.error('chart-pie-demo: onSnapshot error:', err);
    });
    console.log('chart-pie-demo: realtime listener attached.');
  } catch (err) {
    console.warn('chart-pie-demo: realtime listener failed:', err);
  }
})();
