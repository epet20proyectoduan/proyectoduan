/* Misión Domuyo · Titan ATLAS — centro de control */
(() => {
  'use strict';

  const $ = (s, el = document) => el.querySelector(s);
  const $$ = (s, el = document) => [...el.querySelectorAll(s)];
  const REDUCED = matchMedia('(prefers-reduced-motion: reduce)').matches;

  const RANGE_SECONDS = { '15m': 900, '1h': 3600, '6h': 21600, '24h': 86400, '7d': 604800, '30d': 2592000 };
  const RANGE_LABEL = { '15m': 'últimos 15 minutos', '1h': 'última hora', '6h': 'últimas 6 horas', '24h': 'últimas 24 horas', '7d': 'últimos 7 días', '30d': 'últimos 30 días' };
  const LIVE_APPEND = new Set(['15m', '1h', '6h']);
  const ONLINE_MS = 60_000;
  const CORE = new Set(['temperature', 'humidity', 'pressure', 'altitude', 'latitude', 'longitude', 'speed', 'battery', 'rssi']);
  const CATALOG = new Map((window.SENSORS || []).map((s, i) => [s.key, { ...s, order: i }]));
  const GROUP_ORDER = { ambiente: 0, posicion: 1, movimiento: 2, sistema: 3 };
  const PHYS = window.PHYS;
  const EXTRA_COLORS = ['#60A5FA', '#F87171', '#4ADE80', '#E879F9', '#FB923C', '#2DD4BF', '#C084FC', '#FDE047'];
  let extraColorIdx = 0;

  const state = {
    device: '',
    range: '1h',
    config: null,
    lastAt: null,
    lastRow: null,
    packets: 0,
    stats: {},
    totals: {},
    rows: [],
    track: [],
    fields: [],
    bucket: null,
    streamOk: false,
  };

  // ---------------------------------------------------------- formato
  const esc = (s) => String(s).replace(/[&<>"']/g, (c) => ({ '&': '&amp;', '<': '&lt;', '>': '&gt;', '"': '&quot;', "'": '&#39;' }[c]));
  const qs = (o) => new URLSearchParams(Object.entries(o).filter(([, v]) => v !== '' && v != null)).toString();
  const nf = (v, d = 0) => Number(v).toLocaleString('es-AR', { minimumFractionDigits: d, maximumFractionDigits: d });
  const time24 = { hour: '2-digit', minute: '2-digit', second: '2-digit', hour12: false };
  const timeFmt = new Intl.DateTimeFormat('es-AR', time24);
  const hmFmt = new Intl.DateTimeFormat('es-AR', { hour: '2-digit', minute: '2-digit', hour12: false });
  const dateTimeFmt = new Intl.DateTimeFormat('es-AR', { day: '2-digit', month: '2-digit', ...time24 });
  const fmtStamp = (d) => (Date.now() - d.getTime() < 86_400_000 ? timeFmt : dateTimeFmt).format(d);

  function fmt(v, f) {
    if (v === null || v === undefined || v === '') return '—';
    if (typeof v === 'number') return nf(v, f?.digits ?? (Number.isInteger(v) ? 0 : 2));
    if (typeof v === 'object') return JSON.stringify(v);
    return String(v);
  }

  function relTime(date) {
    if (!date) return '—';
    const s = Math.max(0, Math.round((Date.now() - date.getTime()) / 1000));
    if (s < 60) return `hace ${s} s`;
    const m = Math.floor(s / 60);
    if (m < 60) return `hace ${m} min`;
    const h = Math.floor(m / 60);
    if (h < 48) return `hace ${h} h`;
    return dateTimeFmt.format(date);
  }

  const bucketText = (b) => (b < 60 ? `${b} s` : b < 3600 ? `${Math.round(b / 60)} min` : `${nf(b / 3600, 1)} h`);

  async function api(path, params = {}) {
    const q = qs(params);
    const res = await fetch(`/api/${path}${q ? '?' + q : ''}`, { headers: { Accept: 'application/json' } });
    if (!res.ok) throw new Error(`${path}: HTTP ${res.status}`);
    return res.json();
  }

  function toast(msg) {
    const t = $('#toast');
    t.textContent = msg;
    t.classList.add('show');
    clearTimeout(toast.t);
    toast.t = setTimeout(() => t.classList.remove('show'), 3500);
  }

  /** Anima un número desde su valor anterior hasta el nuevo. */
  function tween(el, to, digits) {
    if (typeof to !== 'number') {
      el.textContent = fmt(to);
      delete el.dataset.v;
      return;
    }
    const from = parseFloat(el.dataset.v);
    el.dataset.v = to;
    cancelAnimationFrame(el._raf);
    if (REDUCED || !Number.isFinite(from) || from === to) {
      el.textContent = nf(to, digits);
      return;
    }
    const t0 = performance.now();
    const step = (now) => {
      const k = Math.min(1, (now - t0) / 350);
      const e = 1 - Math.pow(1 - k, 3);
      el.textContent = nf(from + (to - from) * e, digits);
      if (k < 1) el._raf = requestAnimationFrame(step);
    };
    el._raf = requestAnimationFrame(step);
  }

  // ---------------------------------------------------------- variables
  function describe(key, source) {
    const c = CATALOG.get(key);
    return {
      key,
      source, // 'col' = columna propia · 'extra' = JSONB
      label: c?.label || key,
      unit: c?.unit || '',
      digits: c?.digits,
      info: c?.info || (source === 'extra' ? `Campo adicional "${key}" enviado por la estación. Se puede describir en sensors.js.` : ''),
      insight: c?.insight,
      card: c?.card !== false,
      chart: c?.chart !== false,
      group: c?.group || 'zz',
      color: c?.color || EXTRA_COLORS[extraColorIdx++ % EXTRA_COLORS.length],
      minSpan: c?.minSpan,
      order: c ? c.order : 1000,
      numeric: true,
    };
  }

  const valueOf = (row, f) => (f.source === 'col' ? row?.[f.key] : row?.extra?.[f.key]);
  const digitsOf = (f, v) => f.digits ?? (Number.isInteger(v) ? 0 : 2);

  /** Detecta qué variables están llegando realmente. Devuelve true si cambió el conjunto. */
  function discover(rows) {
    const known = new Map(state.fields.map((f) => [f.key, f]));
    let changed = false;
    for (const r of rows) {
      for (const k of CORE) {
        if (r[k] != null && !known.has(k)) {
          known.set(k, describe(k, 'col'));
          changed = true;
        }
      }
      for (const [k, v] of Object.entries(r.extra || {})) {
        if (v === null || v === undefined) continue;
        let f = known.get(k);
        if (!f) {
          f = describe(k, 'extra');
          known.set(k, f);
          changed = true;
        }
        if (typeof v !== 'number' && f.numeric) {
          f.numeric = false;
          f.chart = false;
          changed = true;
        }
      }
    }
    state.fields = [...known.values()].sort(
      (a, b) => (GROUP_ORDER[a.group] ?? 9) - (GROUP_ORDER[b.group] ?? 9) || a.order - b.order || a.key.localeCompare(b.key),
    );
    return changed;
  }

  const hasField = (k) => state.fields.some((f) => f.key === k);

  // ---------------------------------------------------------- enlace
  function setLink(name, label) {
    const el = $('#linkStatus');
    el.dataset.state = name;
    $('.label', el).textContent = label;
  }

  function refreshLink() {
    if (!state.streamOk) return setLink('error', 'Sin conexión con el servidor');
    if (!state.lastAt) return setLink('idle', 'En línea · sin datos');
    const live = Date.now() - state.lastAt.getTime() < ONLINE_MS;
    setLink(live ? 'live' : 'idle', live ? `Recibiendo · ${relTime(state.lastAt)}` : `Último dato ${relTime(state.lastAt)}`);
  }

  // ---------------------------------------------------------- tarjetas
  function buildReadings() {
    $('#readings').innerHTML = state.fields
      .filter((f) => f.card)
      .map((f, i) => `
        <article class="reading" data-key="${esc(f.key)}" style="--i:${i};--c:${f.color}">
          <header>
            <span class="name">${esc(f.label)}</span>
            ${f.info ? `<button class="info-btn" type="button" aria-expanded="false" aria-controls="ex-${esc(f.key)}" title="¿Qué mide?">?</button>` : ''}
          </header>
          <div class="val"><span class="v">—</span>${f.unit ? `<small>${esc(f.unit)}</small>` : ''}<span class="trend"></span></div>
          ${f.numeric ? '<svg class="spark" viewBox="0 0 100 30" preserveAspectRatio="none" aria-hidden="true"><path class="area"/><path class="line"/></svg>' : ''}
          ${f.source === 'col' && f.numeric ? '<div class="range"></div>' : ''}
          <p class="insight"></p>
          ${f.info ? `<div class="explain" id="ex-${esc(f.key)}"><div><p>${esc(f.info)}</p></div></div>` : ''}
        </article>`)
      .join('');
  }

  function sparkSeries(f) {
    const out = [];
    for (const r of state.rows) {
      const v = valueOf(r, f);
      if (typeof v === 'number') out.push({ v, t: new Date(r.received_at) });
      if (out.length >= 40) break;
    }
    return out.reverse();
  }

  function renderSpark(el, pts) {
    const svg = $('.spark', el);
    if (!svg) return;
    if (pts.length < 2) {
      $('.line', svg).setAttribute('d', '');
      $('.area', svg).setAttribute('d', '');
      return;
    }
    const vals = pts.map((p) => p.v);
    const min = Math.min(...vals);
    const max = Math.max(...vals);
    const span = max - min || 1;
    const xy = pts.map((p, i) => [(i / (pts.length - 1)) * 100, 27 - ((p.v - min) / span) * 24]);
    const line = xy.map(([x, y], i) => `${i ? 'L' : 'M'}${x.toFixed(2)} ${y.toFixed(2)}`).join('');
    $('.line', svg).setAttribute('d', line);
    $('.area', svg).setAttribute('d', `${line}L100 30L0 30Z`);
  }

  function renderReadings({ bump = false } = {}) {
    const row = state.lastRow;
    const stale = !state.lastAt || Date.now() - state.lastAt.getTime() > ONLINE_MS;
    for (const f of state.fields.filter((x) => x.card)) {
      const el = $(`.reading[data-key="${CSS.escape(f.key)}"]`);
      if (!el) continue;
      const v = valueOf(row, f);
      tween($('.v', el), v ?? null, digitsOf(f, v));
      el.classList.toggle('stale', stale);

      const pts = f.numeric ? sparkSeries(f) : [];
      renderSpark(el, pts);

      const trend = $('.trend', el);
      if (pts.length >= 2) {
        const d = pts[pts.length - 1].v - pts[0].v;
        const dig = digitsOf(f, d);
        const flat = Math.abs(d) < Math.pow(10, -dig) / 2;
        trend.className = `trend ${flat ? '' : d > 0 ? 'up' : 'down'}`;
        trend.textContent = flat ? '= estable' : `${d > 0 ? '▲' : '▼'} ${nf(Math.abs(d), dig)}`;
        trend.title = `Cambio desde las ${hmFmt.format(pts[0].t)}`;
      } else {
        trend.textContent = '';
      }

      const range = $('.range', el);
      if (range) {
        const min = state.stats[`${f.key}_min`];
        const max = state.stats[`${f.key}_max`];
        range.textContent = min == null ? '' : `mín ${fmt(min, f)} · máx ${fmt(max, f)}`;
        range.title = `Mínimo y máximo de ${RANGE_LABEL[state.range]}`;
      }

      let text = '';
      try {
        text = (row && f.insight?.(Object.assign({}, row.extra, row))) || '';
      } catch { /* insight inválido: se ignora */ }
      $('.insight', el).textContent = text;

      if (bump && v != null && !REDUCED) {
        el.classList.remove('bump');
        void el.offsetWidth;
        el.classList.add('bump');
      }
    }
    if (row) $('#readingTime').textContent = `${row.device_id} · ${dateTimeFmt.format(new Date(row.received_at))}`;
    renderAltimeter();
  }

  function mergeStats(row) {
    for (const f of state.fields) {
      if (f.source !== 'col') continue;
      const v = row[f.key];
      if (v == null) continue;
      for (const s of [state.stats, state.totals]) {
        if (s[`${f.key}_min`] == null || v < s[`${f.key}_min`]) s[`${f.key}_min`] = v;
        if (s[`${f.key}_max`] == null || v > s[`${f.key}_max`]) s[`${f.key}_max`] = v;
      }
    }
  }

  // ---------------------------------------------------------- altímetro
  // Perfil de altitud dibujado con las lecturas reales (sin referencias fijas).
  const ALT = { x0: 36, x1: 312, y0: 16, y1: 224 };

  function niceRange(lo, hi) {
    if (hi - lo < 20) {
      const mid = (lo + hi) / 2;
      lo = mid - 10;
      hi = mid + 10;
    }
    const pad = (hi - lo) * 0.12;
    return [lo - pad, hi + pad];
  }

  function renderAltimeter() {
    const show = hasField('altitude');
    $('#altimeter').hidden = !show;
    updateSide();
    if (!show) return;

    const pts = state.rows.filter((r) => r.altitude != null).slice(0, 100).reverse();
    if (!pts.length) return;
    const vals = pts.map((r) => r.altitude);
    const lo = Math.min(...vals);
    const hi = Math.max(...vals);
    const [a, b] = niceRange(lo, hi);
    const X = (i) => ALT.x0 + (pts.length === 1 ? (ALT.x1 - ALT.x0) : (i / (pts.length - 1)) * (ALT.x1 - ALT.x0));
    const Y = (v) => ALT.y1 - ((v - a) / (b - a)) * (ALT.y1 - ALT.y0);

    // Grilla con 4 valores
    let g = '';
    for (let k = 0; k <= 3; k++) {
      const v = a + ((b - a) * k) / 3;
      const y = Y(v);
      g += `<line x1="${ALT.x0}" x2="${ALT.x1}" y1="${y.toFixed(1)}" y2="${y.toFixed(1)}"/><text x="${ALT.x0 - 5}" y="${(y + 3.5).toFixed(1)}" text-anchor="end">${nf(v)}</text>`;
    }
    $('#altGrid').innerHTML = g;

    const xy = pts.map((r, i) => [X(i), Y(r.altitude)]);
    if (xy.length === 1) xy.unshift([ALT.x0, xy[0][1]]);
    const line = xy.map(([x, y], i) => `${i ? 'L' : 'M'}${x.toFixed(1)} ${y.toFixed(1)}`).join('');
    $('#altLine').setAttribute('d', line);
    $('#altArea').setAttribute('d', `${line}L${ALT.x1} ${ALT.y1}L${xy[0][0].toFixed(1)} ${ALT.y1}Z`);

    const last = xy[xy.length - 1];
    $('#altMarker').style.transform = `translate(${last[0]}px, ${last[1]}px)`;
    $('#altMax').style.transform = `translate(0px, ${Y(hi)}px)`;

    const r = pts[pts.length - 1];
    tween($('#altNow'), r.altitude, 0);
    $('#altMin').textContent = `${nf(lo)} m`;
    $('#altMaxT').textContent = `${nf(hi)} m`;
    const d = r.altitude - pts[0].altitude;
    $('#altDelta').textContent = `${d > 0 ? '+' : ''}${nf(d)} m`;

    const measured = r.pressure != null;
    const p = measured ? r.pressure : PHYS.pressureAt(r.altitude);
    $('#altO2').textContent = `Cada respiración aporta ≈ ${Math.round((p / PHYS.P0) * 100)} % del oxígeno que a nivel del mar`
      + (measured ? '' : ' (estimado por altura)');
  }

  // ---------------------------------------------------------- gráficos
  const charts = new Map();

  // Etiquetas del eje de tiempo según cuánto abarca el gráfico
  function tickLabel(ms) {
    const span = this.max - this.min;
    const opts = span > 2 * 86400_000
      ? { day: '2-digit', month: '2-digit', hour: '2-digit', minute: '2-digit', hour12: false }
      : span < 20 * 60_000
        ? { hour: '2-digit', minute: '2-digit', second: '2-digit', hour12: false }
        : { hour: '2-digit', minute: '2-digit', hour12: false };
    return new Date(ms).toLocaleString('es-AR', opts);
  }

  // Línea vertical que sigue al cursor
  const crosshair = {
    id: 'crosshair',
    afterDatasetsDraw(chart) {
      const a = chart.tooltip?.getActiveElements?.();
      if (!a?.length) return;
      const { ctx, chartArea } = chart;
      const x = a[0].element.x;
      ctx.save();
      ctx.strokeStyle = 'rgba(163,177,198,.35)';
      ctx.setLineDash([3, 3]);
      ctx.beginPath();
      ctx.moveTo(x, chartArea.top);
      ctx.lineTo(x, chartArea.bottom);
      ctx.stroke();
      ctx.restore();
    },
  };

  // Relleno degradado que se desvanece hacia abajo
  const gradientFill = (color) => (ctx) => {
    const { chart } = ctx;
    const area = chart.chartArea;
    if (!area) return `${color}22`;
    const g = chart.ctx.createLinearGradient(0, area.top, 0, area.bottom);
    g.addColorStop(0, `${color}55`);
    g.addColorStop(0.6, `${color}14`);
    g.addColorStop(1, `${color}00`);
    return g;
  };

  function buildCharts() {
    charts.forEach((c) => c.destroy());
    charts.clear();
    const list = state.fields.filter((f) => f.chart && f.numeric);
    $('#charts').innerHTML = list
      .map((f, i) => `<figure class="panel chart" style="--i:${i};--c:${f.color}"><header><h3>${esc(f.label)}${f.unit ? `<span>${esc(f.unit)}</span>` : ''}</h3><span class="stat" data-stat="${esc(f.key)}"></span></header><div class="canvas"><canvas data-key="${esc(f.key)}"></canvas></div></figure>`)
      .join('');
    if (!window.Chart) return;

    Chart.defaults.font.family = "'IBM Plex Mono', monospace";
    Chart.defaults.font.size = 10.5;
    Chart.defaults.color = '#7286a3';
    for (const f of list) {
      const color = f.color;
      const canvas = $(`canvas[data-key="${CSS.escape(f.key)}"]`);
      charts.set(f.key, new Chart(canvas, {
        type: 'line',
        data: {
          datasets: [{
            data: [],
            borderColor: color,
            backgroundColor: gradientFill(color),
            fill: 'start',
            borderWidth: 2.2,
            borderCapStyle: 'round',
            borderJoinStyle: 'round',
            pointRadius: (ctx) => (ctx.dataset.data.length < 16 ? 2.5 : 0),
            pointBackgroundColor: color,
            pointBorderWidth: 0,
            pointHoverRadius: 5,
            pointHoverBackgroundColor: '#fff',
            pointHoverBorderColor: color,
            pointHoverBorderWidth: 2,
            tension: 0.4,
            cubicInterpolationMode: 'monotone',
          }],
        },
        plugins: [crosshair],
        options: {
          responsive: true,
          maintainAspectRatio: false,
          animation: REDUCED ? false : { duration: 450, easing: 'easeOutQuart' },
          parsing: false,
          normalized: true,
          layout: { padding: { top: 4, right: 6 } },
          interaction: { mode: 'nearest', axis: 'x', intersect: false },
          plugins: {
            legend: { display: false },
            tooltip: {
              backgroundColor: 'rgba(16,26,46,.96)', borderColor: color, borderWidth: 1, titleColor: '#e8eef8', bodyColor: color,
              titleFont: { weight: '400' }, bodyFont: { size: 13, weight: '500' },
              displayColors: false, cornerRadius: 6, padding: 10, caretSize: 5,
              callbacks: {
                title: (items) => dateTimeFmt.format(new Date(items[0].parsed.x)),
                label: (item) => `${fmt(item.parsed.y, f)} ${f.unit}`,
              },
            },
          },
          scales: {
            x: {
              type: 'linear',
              grid: { display: false },
              border: { color: '#1f2d48' },
              ticks: { maxTicksLimit: 5, callback: tickLabel, maxRotation: 0, autoSkipPadding: 18 },
            },
            y: {
              position: 'right',
              grid: { color: 'rgba(31,45,72,.55)', drawTicks: false },
              border: { display: false },
              ticks: {
                maxTicksLimit: 4,
                padding: 8,
                callback(v) {
                  const span = this.max - this.min;
                  return nf(v, span < 1 ? 2 : span < 10 ? 1 : 0);
                },
              },
            },
          },
        },
      }));
    }
  }

  // Eje X: se ajusta a los datos que hay (sin dejar la gráfica apretada contra un costado)
  function setChartWindow() {
    const to = Date.now();
    const rangeFrom = to - RANGE_SECONDS[state.range] * 1000;
    charts.forEach((c) => {
      const d = c.data.datasets[0].data;
      let from = rangeFrom;
      if (d.length) {
        const span = Math.max(to - d[0].x, 60_000);
        from = Math.max(rangeFrom, d[0].x - span * 0.03);
      }
      c.options.scales.x.min = from;
      c.options.scales.x.max = to;
    });
  }

  // Eje Y: amplitud mínima por variable para que el ruido del sensor no parezca un serrucho
  function fitY(c, f) {
    const ys = c.data.datasets[0].data.map((p) => p.y);
    if (!ys.length) return;
    let lo = Math.min(...ys);
    let hi = Math.max(...ys);
    const minSpan = f.minSpan ?? Math.max(Math.abs((lo + hi) / 2) * 0.02, 0.5);
    if (hi - lo < minSpan) {
      const mid = (lo + hi) / 2;
      lo = mid - minSpan / 2;
      hi = mid + minSpan / 2;
    }
    const pad = (hi - lo) * 0.14;
    // Variables que nunca son negativas (velocidad, satélites…) no muestran eje negativo
    c.options.scales.y.min = Math.min(...ys) >= 0 ? Math.max(0, lo - pad) : lo - pad;
    c.options.scales.y.max = hi + pad;
  }

  function updateCharts(mode) {
    for (const f of state.fields) {
      const c = charts.get(f.key);
      if (c) fitY(c, f);
    }
    setChartWindow();
    charts.forEach((c) => c.update(mode));
  }

  function renderChartStats() {
    for (const f of state.fields) {
      const el = $(`[data-stat="${CSS.escape(f.key)}"]`);
      const c = charts.get(f.key);
      if (!el || !c) continue;
      const d = c.data.datasets[0].data;
      if (!d.length) { el.textContent = ''; continue; }
      const avg = d.reduce((a, p) => a + p.y, 0) / d.length;
      el.textContent = `actual ${fmt(d[d.length - 1].y, f)} · prom. ${fmt(avg, f)}`;
    }
  }

  function renderSeries(points, bucket) {
    state.bucket = bucket;
    for (const f of state.fields) {
      const c = charts.get(f.key);
      if (!c) continue;
      c.data.datasets[0].data = points
        .map((p) => ({ x: new Date(p.t).getTime(), y: f.source === 'col' ? p[f.key] : p.extra?.[f.key], n: p.n || 1 }))
        .filter((p) => p.y != null);
    }
    updateCharts();
    $('#rangeHint').textContent = `${RANGE_LABEL[state.range]} · cada punto es el promedio de ${bucketText(bucket)}`;
    renderChartStats();
  }

  // Las lecturas en vivo se suman al intervalo que corresponde (misma escala que la serie)
  function appendToCharts(row) {
    const t = new Date(row.received_at).getTime();
    const bucketMs = (state.bucket || 1) * 1000;
    const x = Math.floor(t / bucketMs) * bucketMs;
    const from = Date.now() - RANGE_SECONDS[state.range] * 1000;
    for (const f of state.fields) {
      const c = charts.get(f.key);
      const y = valueOf(row, f);
      if (!c || typeof y !== 'number') continue;
      const data = c.data.datasets[0].data;
      const last = data[data.length - 1];
      if (last && last.x === x) {
        const n = last.n || 1;
        last.y = (last.y * n + y) / (n + 1);
        last.n = n + 1;
      } else {
        data.push({ x, y, n: 1 });
      }
      while (data.length && data[0].x < from) data.shift();
    }
    updateCharts();
    renderChartStats();
  }

  // ---------------------------------------------------------- mapa GPS
  let map, trackLine, liveMarker, startMarker, accuracyCircle;
  let follow = true;
  let movingProgrammatically = false;

  function ensureMap() {
    if (map || !window.L) return;
    map = L.map('map', { scrollWheelZoom: false, zoomControl: false, attributionControl: true })
      .setView([-38.4, -63.6], 4); // Argentina hasta tener la primera posición
    L.control.zoom({ position: 'bottomright' }).addTo(map);
    const esri = (name, maxNativeZoom) => L.tileLayer(`https://server.arcgisonline.com/ArcGIS/rest/services/${name}/MapServer/tile/{z}/{y}/{x}`, { maxZoom: 20, maxNativeZoom, attribution: '© Esri' });
    const osm = L.tileLayer('https://{s}.tile.openstreetmap.org/{z}/{x}/{y}.png', { maxZoom: 20, maxNativeZoom: 19, attribution: '© OpenStreetMap' });
    const sat = L.layerGroup([esri('World_Imagery', 19), esri('Reference/World_Boundaries_and_Places', 19)]);
    const dark = esri('Canvas/World_Dark_Gray_Base', 16);
    const topo = L.tileLayer('https://{s}.tile.opentopomap.org/{z}/{x}/{y}.png', { maxZoom: 20, maxNativeZoom: 17, attribution: '© OpenStreetMap · © OpenTopoMap (CC-BY-SA)' });
    osm.addTo(map);
    L.control.layers({ Calles: osm, Satélite: sat, Oscuro: dark, Topográfico: topo }, null, { position: 'topright' }).addTo(map);
    L.control.scale({ imperial: false, position: 'bottomleft' }).addTo(map);

    trackLine = L.polyline([], { color: '#ff6b35', weight: 3.5, opacity: 0.9, lineCap: 'round', lineJoin: 'round' }).addTo(map);
    accuracyCircle = L.circle([0, 0], { radius: 1, color: '#38bdf8', weight: 1, fillColor: '#38bdf8', fillOpacity: 0.12, interactive: false });

    // Si el usuario mueve el mapa, deja de seguir al ESP32 hasta que toque "Centrar"
    map.on('movestart', () => {
      if (movingProgrammatically) return;
      follow = false;
      $('#gpsCenter').hidden = !liveMarker;
    });
    map.on('click', () => map.scrollWheelZoom.enable());
    map.on('mouseout', () => map.scrollWheelZoom.disable());
    $('#gpsCenter').addEventListener('click', () => {
      follow = true;
      $('#gpsCenter').hidden = true;
      renderGps(false);
    });
  }

  function flyTo(ll, zoom) {
    movingProgrammatically = true;
    map.once('moveend', () => { movingProgrammatically = false; });
    if (zoom) map.flyTo(ll, zoom, { duration: REDUCED ? 0 : 1.2 });
    else map.panTo(ll, { animate: !REDUCED, duration: 0.6 });
  }

  // Rumbo (0° = norte) entre dos puntos
  function bearing(a, b) {
    const toRad = (d) => (d * Math.PI) / 180;
    const y = Math.sin(toRad(b[1] - a[1])) * Math.cos(toRad(b[0]));
    const x = Math.cos(toRad(a[0])) * Math.sin(toRad(b[0])) - Math.sin(toRad(a[0])) * Math.cos(toRad(b[0])) * Math.cos(toRad(b[1] - a[1]));
    return ((Math.atan2(y, x) * 180) / Math.PI + 360) % 360;
  }

  function gpsMarkerHtml(stale) {
    return `<div class="gps-marker${stale ? ' stale' : ''}"><span class="pulse"></span><span class="heading"></span><span class="dot"></span></div>`;
  }

  function renderGps(fit) {
    $('#mapa').hidden = $('#dashboard').hidden;
    if ($('#mapa').hidden) return;
    ensureMap();
    if (!map) return;
    map.invalidateSize();

    const row = state.lastRow;
    const sats = row?.extra?.satellites;
    const hdop = row?.extra?.hdop;
    const hasFix = row?.latitude != null && row?.longitude != null;
    const pts = state.track.map((p) => [p.latitude, p.longitude]);
    const last = state.track[state.track.length - 1];

    // Aviso mientras no hay señal
    $('#gpsWait').hidden = hasFix;
    if (!hasFix) {
      $('#gpsWaitTitle').textContent = last ? 'Señal GPS perdida' : 'Buscando satélites…';
      $('#gpsWaitText').textContent = (sats != null ? `${sats} satélite${sats === 1 ? '' : 's'} a la vista. ` : '')
        + (last ? `Se muestra la última posición conocida (${relTime(new Date(last.received_at))}).` : 'Llevá el GPS a cielo abierto: el primer fix puede tardar de 1 a 5 minutos.');
    }

    // Estado arriba del mapa
    const chips = [];
    chips.push(`<span class="chip ${hasFix ? 'ok' : 'warn'}"><i></i>${hasFix ? 'GPS con señal' : 'Sin señal GPS'}</span>`);
    if (sats != null) chips.push(`<span class="chip">${sats} satélites</span>`);
    if (hasFix && hdop != null) chips.push(`<span class="chip">± ${nf(Math.max(2, hdop * 5), 0)} m</span>`);
    if (hasFix && row.speed != null) chips.push(`<span class="chip">${nf(row.speed, 1)} km/h</span>`);
    $('#gpsHud').innerHTML = chips.join('');

    trackLine.setLatLngs(pts);
    if (!last) {
      $('#gpsInfo').textContent = 'Sin posiciones registradas todavía';
      return;
    }

    const ll = [last.latitude, last.longitude];
    if (!liveMarker) {
      liveMarker = L.marker(ll, { icon: L.divIcon({ className: '', html: gpsMarkerHtml(!hasFix), iconSize: [44, 44], iconAnchor: [22, 22] }), zIndexOffset: 1000 }).addTo(map);
      startMarker = L.marker(pts[0], { icon: L.divIcon({ className: '', html: '<div class="pin-start"></div>', iconSize: [12, 12] }), title: 'Primera posición' })
        .addTo(map)
        .bindPopup('<b>Primera posición registrada</b>');
      fit = true;
    } else {
      liveMarker.setLatLng(ll);
      startMarker.setLatLng(pts[0]);
    }
    liveMarker.getElement()?.querySelector('.gps-marker')?.classList.toggle('stale', !hasFix);

    // Flecha de rumbo cuando se está moviendo
    const el = liveMarker.getElement()?.querySelector('.heading');
    let prev = null;
    for (let i = pts.length - 2; i >= 0; i--) {
      if (map.distance(pts[i], ll) > 3) { prev = pts[i]; break; }
    }
    const moving = hasFix && prev && (row.speed == null || row.speed > 1);
    if (el) {
      el.style.opacity = moving ? 1 : 0;
      if (moving) el.style.transform = `rotate(${bearing(prev, ll)}deg)`;
    }

    // Círculo de precisión
    if (hasFix && hdop != null) {
      accuracyCircle.setLatLng(ll).setRadius(Math.max(2, hdop * 5));
      if (!map.hasLayer(accuracyCircle)) accuracyCircle.addTo(map);
    } else if (map.hasLayer(accuracyCircle)) {
      accuracyCircle.remove();
    }

    let km = 0;
    for (let i = 1; i < pts.length; i++) km += map.distance(pts[i - 1], pts[i]) / 1000;
    $('#gpsInfo').textContent = `${nf(last.latitude, 6)}, ${nf(last.longitude, 6)} · recorrido ${km < 1 ? `${nf(km * 1000, 0)} m` : `${nf(km, 2)} km`}`;
    liveMarker.bindPopup(`<b>${hasFix ? 'Posición actual' : 'Última posición conocida'}</b><br>${nf(last.latitude, 6)}, ${nf(last.longitude, 6)}${last.altitude != null ? `<br>${nf(last.altitude)} m s.n.m.` : ''}<br>${dateTimeFmt.format(new Date(last.received_at))}`);

    if (fit) flyTo(ll, Math.max(map.getZoom(), 17));
    else if (follow) flyTo(ll);
  }

  // ---------------------------------------------------------- tabla
  function renderTableHead() {
    $('#rowsHead').innerHTML = `<tr><th>Hora</th><th class="l">Dispositivo</th>${state.fields
      .map((f) => `<th${f.numeric ? '' : ' class="l"'}>${esc(f.label)}${f.unit ? ` (${esc(f.unit)})` : ''}</th>`)
      .join('')}</tr>`;
  }

  function rowHtml(r, isNew) {
    return `<tr${isNew ? ' class="new enter"' : ''}><td class="l">${esc(fmtStamp(new Date(r.received_at)))}</td><td class="l">${esc(r.device_id)}</td>${state.fields
      .map((f) => {
        const v = valueOf(r, f);
        return `<td class="${f.numeric ? '' : 'l'}${v == null ? ' n' : ''}">${esc(fmt(v, f))}</td>`;
      })
      .join('')}</tr>`;
  }

  function renderTable() {
    renderTableHead();
    $('#rows').innerHTML = state.rows.map((r) => rowHtml(r)).join('');
  }

  // ---------------------------------------------------------- visibilidad
  function renderVisibility() {
    const hasData = state.rows.length > 0;
    $('#waiting').hidden = hasData;
    $('#dashboard').hidden = !hasData;
    $('.dash-main').hidden = charts.size === 0;
  }

  // La columna derecha (altímetro y mapa) solo ocupa lugar si tiene algo que mostrar
  function updateSide() {
    const empty = $('#altimeter').hidden;
    $('#dashSide').hidden = empty;
    $('#dashGrid').classList.toggle('no-side', empty);
  }

  // ---------------------------------------------------------- dispositivos
  async function loadDevices() {
    const list = await api('devices');
    const sel = $('#deviceSelect');
    const current = sel.value;
    sel.innerHTML = '<option value="">Todos</option>' + list.map((d) => `<option value="${esc(d.id)}">${esc(d.name || d.id)}${d.online ? '' : ' (sin señal)'}</option>`).join('');
    sel.value = list.some((d) => d.id === current) ? current : '';
  }

  // ---------------------------------------------------------- carga
  let loadSeq = 0;
  async function loadAll({ fit = true } = {}) {
    const seq = ++loadSeq;
    const p = { device: state.device };
    $('#exportCsv').href = `/api/export.csv?${qs({ ...p, range: state.range })}`;
    try {
      const [series, stats, rows, track, totals] = await Promise.all([
        api('series', { ...p, range: state.range }),
        api('stats', { ...p, range: state.range }),
        api('telemetry', { ...p, limit: 100 }),
        api('track', { ...p, limit: 2000 }),
        api('stats', p),
      ]);
      if (seq !== loadSeq) return;
      state.stats = stats;
      state.totals = totals;
      state.rows = rows;
      state.track = track;
      state.packets = totals.count;
      state.lastRow = rows[0] || null;
      state.lastAt = state.lastRow ? new Date(state.lastRow.received_at) : null;
      state.fields = [];
      discover(rows);
      // Las secciones se muestran antes de crear los gráficos para que tengan tamaño
      $('#dashboard').hidden = !rows.length;
      $('#waiting').hidden = !!rows.length;
      buildReadings();
      buildCharts();
      renderVisibility();
      renderReadings();
      renderSeries(series.points, series.bucket);
      renderTable();
      renderGps(fit);
      refreshLink();
    } catch (err) {
      console.error(err);
      toast('No se pudieron cargar los datos.');
    }
  }

  // ---------------------------------------------------------- tiempo real
  let refreshTimer;
  function connectStream() {
    if (!window.EventSource) return;
    const es = new EventSource('/api/stream');
    es.onopen = () => {
      state.streamOk = true;
      refreshLink();
    };
    es.onerror = () => {
      state.streamOk = false;
      refreshLink();
    };
    es.addEventListener('telemetry', (ev) => {
      const row = JSON.parse(ev.data);
      state.packets += 1;
      if (![...$('#deviceSelect').options].some((o) => o.value === row.device_id)) loadDevices().catch(() => {});
      if (state.device && row.device_id !== state.device) return;

      // Una variable nueva obliga a rearmar tarjetas y gráficos
      if (discover([row])) return loadAll({ fit: false });

      state.lastRow = row;
      state.lastAt = new Date(row.received_at);
      state.rows.unshift(row);
      state.rows.length = Math.min(state.rows.length, 100);
      mergeStats(row);
      renderReadings({ bump: true });

      if (LIVE_APPEND.has(state.range)) {
        appendToCharts(row);
      } else {
        clearTimeout(refreshTimer);
        refreshTimer = setTimeout(() => loadAll({ fit: false }), 30_000);
      }

      const tbody = $('#rows');
      tbody.insertAdjacentHTML('afterbegin', rowHtml(row, true));
      while (tbody.rows.length > 100) tbody.deleteRow(-1);
      const added = tbody.rows[0];
      setTimeout(() => added.classList.remove('new', 'enter'), 1600);

      if (row.latitude != null && row.longitude != null) state.track.push(row);
      renderVisibility();
      renderGps(false);
      refreshLink();
    });
  }

  // ---------------------------------------------------------- UI
  function moveThumb() {
    const b = $('#rangeSelect button[aria-checked="true"]');
    const thumb = $('.seg-thumb');
    if (!b || !thumb) return;
    thumb.style.width = `${b.offsetWidth}px`;
    thumb.style.transform = `translateX(${b.offsetLeft}px)`;
  }

  function bindUi() {

    $('#deviceSelect').addEventListener('change', (e) => {
      state.device = e.target.value;
      loadAll();
    });
    $$('#rangeSelect button').forEach((b) =>
      b.addEventListener('click', () => {
        $$('#rangeSelect button').forEach((x) => x.setAttribute('aria-checked', String(x === b)));
        moveThumb();
        state.range = b.dataset.range;
        loadAll({ fit: false });
      }),
    );
    addEventListener('resize', moveThumb);
    document.fonts?.ready.then(moveThumb);
    moveThumb();

    // Botones "?" de las tarjetas y el altímetro
    document.addEventListener('click', (e) => {
      const btn = e.target.closest('.info-btn');
      if (!btn) return;
      const panel = document.getElementById(btn.getAttribute('aria-controls'));
      const open = btn.getAttribute('aria-expanded') !== 'true';
      btn.setAttribute('aria-expanded', String(open));
      panel?.classList.toggle('open', open);
    });

    $('#year').textContent = new Date().getFullYear();
    $('#endpointUrl').textContent = `${location.origin}/api/telemetry`;

    setInterval(() => {
      refreshLink();
      if (LIVE_APPEND.has(state.range) && charts.size) updateCharts('none');
    }, 1000);
    setInterval(() => renderReadings(), 15_000);
    setInterval(() => loadDevices().catch(() => {}), 30_000);
  }

  async function boot() {
    bindUi();
    try {
      state.config = await api('config');
    } catch {
      state.config = {};
    }
    await loadDevices().catch(() => {});
    await loadAll();
    connectStream();
  }

  boot();
})();
