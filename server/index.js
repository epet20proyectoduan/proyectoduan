// ============================================================
//  Misión Domuyo · Titan ATLAS — Servidor de telemetría
//  ESP32 ──HTTP/JSON──▶ Express ──▶ PostgreSQL (Railway)
// ============================================================

const path = require('path');
const crypto = require('crypto');
const express = require('express');
const helmet = require('helmet');
const compression = require('compression');
const { createDb } = require('./db');
const { normalize, COLUMNS } = require('./telemetry');

const PORT = Number(process.env.PORT || 3000);
const API_KEY = process.env.API_KEY || '';
const ON_RAILWAY = Boolean(process.env.RAILWAY_ENVIRONMENT || process.env.RAILWAY_PROJECT_ID);
const MAX_BATCH = 200;

const db = createDb();
const app = express();
app.set('trust proxy', 1);
app.disable('x-powered-by');

app.use(
  helmet({
    contentSecurityPolicy: {
      directives: {
        defaultSrc: ["'self'"],
        scriptSrc: ["'self'", 'https://cdn.jsdelivr.net', 'https://unpkg.com'],
        styleSrc: ["'self'", "'unsafe-inline'", 'https://fonts.googleapis.com', 'https://unpkg.com'],
        fontSrc: ["'self'", 'https://fonts.gstatic.com'],
        imgSrc: ["'self'", 'data:', 'https://unpkg.com', 'https://*.tile.openstreetmap.org', 'https://*.tile.opentopomap.org', 'https://server.arcgisonline.com'],
        connectSrc: ["'self'"],
      },
    },
    crossOriginEmbedderPolicy: false,
  }),
);
app.use(compression({ filter: (req, res) => req.path !== '/api/stream' && compression.filter(req, res) }));
app.use(express.json({ limit: '256kb' }));
app.use(express.text({ type: 'text/plain', limit: '256kb' })); // algunos clientes ESP32 no ponen Content-Type

// ---------------------------------------------------------------- utilidades
const asyncH = (fn) => (req, res, next) => fn(req, res, next).catch(next);

function safeEqual(a, b) {
  const ha = crypto.createHash('sha256').update(a).digest();
  const hb = crypto.createHash('sha256').update(b).digest();
  return crypto.timingSafeEqual(ha, hb);
}

function requireKey(req, res, next) {
  if (!API_KEY) {
    if (ON_RAILWAY) {
      return res.status(503).json({ error: 'El servidor no tiene API_KEY configurada. Defínala en Railway → Variables.' });
    }
    return next(); // desarrollo local sin clave
  }
  const header = req.get('x-api-key') || (req.get('authorization') || '').replace(/^Bearer\s+/i, '');
  if (!header || !safeEqual(header, API_KEY)) {
    return res.status(401).json({ error: 'API key inválida o ausente (header X-API-Key)' });
  }
  next();
}

// Límite simple de peticiones por IP para la ingesta
const buckets = new Map();
function rateLimit(req, res, next) {
  const now = Date.now();
  const b = buckets.get(req.ip) || { tokens: 30, ts: now };
  b.tokens = Math.min(30, b.tokens + ((now - b.ts) / 1000) * 10); // 10 req/s sostenido, ráfaga de 30
  b.ts = now;
  if (b.tokens < 1) return res.status(429).json({ error: 'Demasiadas peticiones' });
  b.tokens -= 1;
  buckets.set(req.ip, b);
  next();
}
setInterval(() => {
  const limit = Date.now() - 60_000;
  for (const [ip, b] of buckets) if (b.ts < limit) buckets.delete(ip);
}, 60_000).unref();

function parseDate(v) {
  if (!v) return null;
  const d = new Date(v);
  return Number.isNaN(d.getTime()) ? null : d;
}

function clampInt(v, def, min, max) {
  const n = parseInt(v, 10);
  return Number.isFinite(n) ? Math.min(max, Math.max(min, n)) : def;
}

const RANGES = { '15m': 900, '1h': 3600, '6h': 21600, '24h': 86400, '7d': 604800, '30d': 2592000 };

// ---------------------------------------------------------------- tiempo real (SSE)
const clients = new Set();
function broadcast(event, data) {
  const msg = `event: ${event}\ndata: ${JSON.stringify(data)}\n\n`;
  for (const res of clients) res.write(msg);
}
setInterval(() => {
  for (const res of clients) res.write(': ping\n\n');
}, 25_000).unref();

// ---------------------------------------------------------------- API
const api = express.Router();

api.get('/health', asyncH(async (req, res) => {
  await db.ping();
  res.json({ ok: true, db: db.kind, uptime: Math.round(process.uptime()) });
}));

api.get('/config', (req, res) => {
  res.json({
    mission: 'Misión Domuyo',
    team: 'Titan ATLAS',
    db: db.kind,
    secured: Boolean(API_KEY),
    fields: COLUMNS,
  });
});

// Ingesta desde el ESP32: un objeto o un arreglo de objetos (envío por lotes)
api.post('/telemetry', rateLimit, requireKey, asyncH(async (req, res) => {
  let body = req.body;
  if (typeof body === 'string') {
    try {
      body = JSON.parse(body);
    } catch {
      return res.status(400).json({ error: 'JSON inválido' });
    }
  }

  const items = Array.isArray(body) ? body : Array.isArray(body?.readings) ? body.readings : [body];
  if (!items.length) return res.status(400).json({ error: 'No se recibieron lecturas' });
  if (items.length > MAX_BATCH) return res.status(413).json({ error: `Máximo ${MAX_BATCH} lecturas por envío` });

  const fallbackId = req.get('x-device-id') || body?.device_id;
  const readings = [];
  const warnings = [];
  for (let i = 0; i < items.length; i++) {
    const r = normalize(items[i], fallbackId);
    if (!r.ok) return res.status(400).json({ error: `Lectura ${i}: ${r.error}` });
    readings.push(r.reading);
    r.warnings.forEach((w) => warnings.push(`Lectura ${i}: ${w}`));
  }

  const rows = await db.insertMany(readings);
  rows.forEach((row) => broadcast('telemetry', row));

  res.status(201).json({ ok: true, stored: rows.length, ids: rows.map((r) => r.id), warnings });
}));

api.get('/telemetry', asyncH(async (req, res) => {
  const rows = await db.list({
    device: req.query.device || null,
    from: parseDate(req.query.from),
    to: parseDate(req.query.to),
    limit: clampInt(req.query.limit, 100, 1, 5000),
  });
  res.json(rows);
}));

api.get('/telemetry/latest', asyncH(async (req, res) => {
  res.json(await db.latest(req.query.device || null));
}));

api.get('/series', asyncH(async (req, res) => {
  const span = RANGES[req.query.range] || RANGES['1h'];
  const to = new Date();
  const from = new Date(to.getTime() - span * 1000);
  const bucket = Math.max(1, Math.ceil(span / 300)); // ~300 puntos por gráfico
  const rows = await db.series({ device: req.query.device || null, from, to, bucket });
  res.json({ from, to, bucket, points: rows });
}));

api.get('/stats', asyncH(async (req, res) => {
  const span = RANGES[req.query.range];
  const from = span ? new Date(Date.now() - span * 1000) : null;
  res.json(await db.stats({ device: req.query.device || null, from }));
}));

api.get('/track', asyncH(async (req, res) => {
  res.json(await db.track({ device: req.query.device || null, limit: clampInt(req.query.limit, 1000, 1, 10000) }));
}));

api.get('/devices', asyncH(async (req, res) => {
  const list = await db.devices();
  const now = Date.now();
  res.json(list.map((d) => ({ ...d, online: now - new Date(d.last_seen).getTime() < 60_000 })));
}));

api.get('/export.csv', asyncH(async (req, res) => {
  const span = RANGES[req.query.range];
  const rows = await db.list({
    device: req.query.device || null,
    from: span ? new Date(Date.now() - span * 1000) : parseDate(req.query.from),
    to: parseDate(req.query.to),
    limit: 100000,
  });
  const cols = ['id', 'device_id', 'received_at', 'device_ts', 'seq', ...COLUMNS, 'extra'];
  const esc = (v) => {
    if (v === null || v === undefined) return '';
    const s = v instanceof Date ? v.toISOString() : typeof v === 'object' ? JSON.stringify(v) : String(v);
    return /[",\n;]/.test(s) ? `"${s.replace(/"/g, '""')}"` : s;
  };
  const stamp = new Date().toISOString().slice(0, 19).replace(/[:T]/g, '-');
  res.setHeader('Content-Type', 'text/csv; charset=utf-8');
  res.setHeader('Content-Disposition', `attachment; filename="mision-domuyo-${stamp}.csv"`);
  res.write('﻿' + cols.join(',') + '\n');
  for (const r of rows.reverse()) res.write(cols.map((c) => esc(r[c])).join(',') + '\n');
  res.end();
}));

api.get('/stream', (req, res) => {
  res.set({
    'Content-Type': 'text/event-stream',
    'Cache-Control': 'no-cache, no-transform',
    Connection: 'keep-alive',
    'X-Accel-Buffering': 'no',
  });
  res.flushHeaders();
  res.write('retry: 5000\n\n');
  clients.add(res);
  req.on('close', () => clients.delete(res));
});

app.use('/api', api);
app.get('/health', (req, res) => res.redirect(307, '/api/health'));

// ---------------------------------------------------------------- frontend
// HTML/JS/CSS se revalidan siempre (ETag) para que cada deploy se vea al instante;
// las imágenes de marca se cachean una semana.
app.use(
  express.static(path.join(__dirname, '..', 'public'), {
    extensions: ['html'],
    setHeaders: (res, file) => {
      res.setHeader('Cache-Control', /[\/]assets[\/]/.test(file) ? 'public, max-age=604800' : 'no-cache');
    },
  }),
);

app.use('/api', (req, res) => res.status(404).json({ error: 'Ruta no encontrada' }));
app.use((req, res) => res.status(404).sendFile(path.join(__dirname, '..', 'public', '404.html')));

// eslint-disable-next-line no-unused-vars
app.use((err, req, res, next) => {
  if (err.type === 'entity.parse.failed') return res.status(400).json({ error: 'JSON inválido' });
  if (err.type === 'entity.too.large') return res.status(413).json({ error: 'Cuerpo demasiado grande' });
  console.error(err);
  res.status(500).json({ error: 'Error interno del servidor' });
});

// ---------------------------------------------------------------- arranque
async function start() {
  for (let attempt = 1; ; attempt++) {
    try {
      await db.init();
      break;
    } catch (err) {
      if (attempt >= 10) throw err;
      console.error(`No se pudo conectar a la base de datos (intento ${attempt}): ${err.message}`);
      await new Promise((r) => setTimeout(r, 3000));
    }
  }
  if (!API_KEY) console.warn('⚠  API_KEY no definida: la ingesta está abierta (solo aceptable en local).');

  const server = app.listen(PORT, () => {
    console.log(`🛰  Misión Domuyo · Titan ATLAS escuchando en :${PORT} (BD: ${db.kind})`);
  });

  const shutdown = () => {
    console.log('Cerrando…');
    for (const res of clients) res.end();
    server.close(() => db.close().finally(() => process.exit(0)));
    setTimeout(() => process.exit(0), 5000).unref();
  };
  process.on('SIGTERM', shutdown);
  process.on('SIGINT', shutdown);
}

start().catch((err) => {
  console.error('Error fatal al iniciar:', err);
  process.exit(1);
});
