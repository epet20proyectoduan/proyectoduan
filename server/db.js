// Capa de datos: PostgreSQL en producción (Railway) y memoria para desarrollo local
// cuando no hay DATABASE_URL.

const fs = require('fs');
const path = require('path');
const { COLUMNS } = require('./telemetry');

// received_at = momento de la medición (la hora del ESP32 si es confiable; si no, la de llegada)
const ALL_COLS = ['received_at', 'device_id', 'device_ts', 'seq', ...COLUMNS, 'extra'];
const STAT_COLS = ['temperature', 'humidity', 'pressure', 'altitude', 'speed', 'battery', 'rssi'];

function mergeExtras(rows, extras) {
  const byT = new Map(rows.map((r) => [new Date(r.t).getTime(), Object.assign(r, { extra: {} })]));
  for (const e of extras) {
    const row = byT.get(new Date(e.t).getTime());
    if (row) row.extra[e.key] = e.v;
  }
  return rows;
}

// ---------------------------------------------------------------- PostgreSQL
function createPg(connectionString) {
  const { Pool, types } = require('pg');
  types.setTypeParser(20, (v) => Number(v)); // BIGINT -> number
  types.setTypeParser(1700, (v) => Number(v)); // NUMERIC -> number

  const isLocal = /localhost|127\.0\.0\.1|\.railway\.internal/.test(connectionString);
  const pool = new Pool({
    connectionString,
    ssl: isLocal ? false : { rejectUnauthorized: false },
    max: Number(process.env.PG_POOL_MAX || 10),
  });

  return {
    kind: 'postgresql',

    async init() {
      const sql = fs.readFileSync(path.join(__dirname, '..', 'db', 'schema.sql'), 'utf8');
      await pool.query(sql);
    },

    async ping() {
      await pool.query('SELECT 1');
    },

    async insertMany(readings) {
      const client = await pool.connect();
      try {
        await client.query('BEGIN');

        const counts = new Map();
        for (const r of readings) counts.set(r.device_id, (counts.get(r.device_id) || 0) + 1);
        for (const [id, n] of counts) {
          await client.query(
            `INSERT INTO devices (id, name, packets) VALUES ($1, $1, $2)
             ON CONFLICT (id) DO UPDATE SET last_seen = now(), packets = devices.packets + $2`,
            [id, n],
          );
        }

        const values = [];
        const rowsSql = readings.map((r, i) => {
          ALL_COLS.forEach((c) => values.push(c === 'extra' ? JSON.stringify(r.extra || {}) : c === 'received_at' ? r.received_at || new Date() : r[c] ?? null));
          const base = i * ALL_COLS.length;
          return `(${ALL_COLS.map((_, j) => `$${base + j + 1}`).join(',')})`;
        });
        const { rows } = await client.query(
          `INSERT INTO telemetry (${ALL_COLS.join(',')}) VALUES ${rowsSql.join(',')} RETURNING *`,
          values,
        );

        await client.query('COMMIT');
        return rows;
      } catch (err) {
        await client.query('ROLLBACK');
        throw err;
      } finally {
        client.release();
      }
    },

    async list({ device, from, to, limit }) {
      const where = [];
      const params = [];
      if (device) where.push(`device_id = $${params.push(device)}`);
      if (from) where.push(`received_at >= $${params.push(from)}`);
      if (to) where.push(`received_at <= $${params.push(to)}`);
      params.push(limit);
      const { rows } = await pool.query(
        `SELECT * FROM telemetry ${where.length ? 'WHERE ' + where.join(' AND ') : ''}
         ORDER BY received_at DESC, id DESC LIMIT $${params.length}`,
        params,
      );
      return rows;
    },

    async series({ device, from, to, bucket }) {
      const where = ['received_at >= $1', 'received_at <= $2'];
      const params = [from, to, bucket];
      if (device) where.push(`device_id = $${params.push(device)}`);
      const { rows } = await pool.query(
        `SELECT to_timestamp(floor(extract(epoch FROM received_at) / $3) * $3) AS t,
                count(*)::int AS n,
                ${STAT_COLS.map((c) => `avg(${c})::real AS ${c}`).join(', ')}
           FROM telemetry
          WHERE ${where.join(' AND ')}
          GROUP BY 1 ORDER BY 1`,
        params,
      );
      // Campos numéricos adicionales (columna JSONB "extra"), agregados por el mismo intervalo
      const { rows: extras } = await pool.query(
        `SELECT to_timestamp(floor(extract(epoch FROM received_at) / $3) * $3) AS t,
                e.key, avg((e.value)::text::double precision)::real AS v
           FROM telemetry, jsonb_each(extra) e
          WHERE ${where.join(' AND ')} AND jsonb_typeof(e.value) = 'number'
          GROUP BY 1, 2`,
        params,
      );
      return mergeExtras(rows, extras);
    },

    async latest(device) {
      const { rows } = await pool.query(
        device
          ? 'SELECT * FROM telemetry WHERE device_id = $1 ORDER BY received_at DESC, id DESC LIMIT 1'
          : 'SELECT * FROM telemetry ORDER BY received_at DESC, id DESC LIMIT 1',
        device ? [device] : [],
      );
      return rows[0] || null;
    },

    async stats({ device, from }) {
      const where = [];
      const params = [];
      if (device) where.push(`device_id = $${params.push(device)}`);
      if (from) where.push(`received_at >= $${params.push(from)}`);
      const { rows } = await pool.query(
        `SELECT count(*)::int AS count,
                min(received_at) AS first_at,
                max(received_at) AS last_at,
                ${STAT_COLS.map((c) => `min(${c}) AS ${c}_min, max(${c}) AS ${c}_max, avg(${c})::real AS ${c}_avg`).join(', ')}
           FROM telemetry ${where.length ? 'WHERE ' + where.join(' AND ') : ''}`,
        params,
      );
      return rows[0];
    },

    async devices() {
      const { rows } = await pool.query('SELECT * FROM devices ORDER BY last_seen DESC');
      return rows;
    },

    async track({ device, limit }) {
      const params = [limit];
      if (device) params.push(device);
      const { rows } = await pool.query(
        `SELECT received_at, latitude, longitude, altitude FROM telemetry
          WHERE latitude IS NOT NULL AND longitude IS NOT NULL ${device ? 'AND device_id = $2' : ''}
          ORDER BY received_at DESC LIMIT $1`,
        params,
      );
      return rows.reverse();
    },

    async close() {
      await pool.end();
    },
  };
}

// ---------------------------------------------------------------- Memoria
function createMemory() {
  const telemetry = [];
  const devices = new Map();
  let nextId = 1;
  const MAX = 50000;

  const filter = ({ device, from, to }) =>
    telemetry.filter(
      (r) =>
        (!device || r.device_id === device) &&
        (!from || r.received_at >= from) &&
        (!to || r.received_at <= to),
    );

  const agg = (rows, col) => {
    const v = rows.map((r) => r[col]).filter((x) => x !== null && x !== undefined);
    if (!v.length) return { min: null, max: null, avg: null };
    return { min: Math.min(...v), max: Math.max(...v), avg: v.reduce((a, b) => a + b, 0) / v.length };
  };

  return {
    kind: 'memory',
    async init() {},
    async ping() {},

    async insertMany(readings) {
      const now = new Date();
      return readings.map((r) => {
        const row = { id: nextId++ };
        ALL_COLS.forEach((c) => (row[c] = c === 'extra' ? r.extra || {} : r[c] ?? null));
        row.received_at = r.received_at || now;
        telemetry.push(row);
        if (telemetry.length > MAX) telemetry.shift();
        const d = devices.get(r.device_id) || { id: r.device_id, name: r.device_id, first_seen: now, packets: 0 };
        d.last_seen = now;
        d.packets += 1;
        devices.set(r.device_id, d);
        return row;
      });
    },

    async list(q) {
      return filter(q).slice(-q.limit).reverse();
    },

    async series({ device, from, to, bucket }) {
      const groups = new Map();
      for (const r of filter({ device, from, to })) {
        const t = Math.floor(r.received_at.getTime() / 1000 / bucket) * bucket * 1000;
        if (!groups.has(t)) groups.set(t, []);
        groups.get(t).push(r);
      }
      return [...groups.entries()]
        .sort((a, b) => a[0] - b[0])
        .map(([t, rows]) => {
          const out = { t: new Date(t), n: rows.length };
          STAT_COLS.forEach((c) => (out[c] = agg(rows, c).avg));
          const sums = {};
          for (const r of rows) {
            for (const [k, v] of Object.entries(r.extra || {})) {
              if (typeof v !== 'number') continue;
              (sums[k] ||= []).push(v);
            }
          }
          out.extra = Object.fromEntries(Object.entries(sums).map(([k, v]) => [k, v.reduce((a, b) => a + b, 0) / v.length]));
          return out;
        });
    },

    async latest(device) {
      const rows = filter({ device });
      return rows[rows.length - 1] || null;
    },

    async stats(q) {
      const rows = filter(q);
      const out = {
        count: rows.length,
        first_at: rows[0]?.received_at || null,
        last_at: rows[rows.length - 1]?.received_at || null,
      };
      STAT_COLS.forEach((c) => {
        const a = agg(rows, c);
        out[`${c}_min`] = a.min;
        out[`${c}_max`] = a.max;
        out[`${c}_avg`] = a.avg;
      });
      return out;
    },

    async devices() {
      return [...devices.values()].sort((a, b) => b.last_seen - a.last_seen);
    },

    async track({ device, limit }) {
      return filter({ device })
        .filter((r) => r.latitude !== null && r.longitude !== null)
        .slice(-limit)
        .map(({ received_at, latitude, longitude, altitude }) => ({ received_at, latitude, longitude, altitude }));
    },

    async close() {},
  };
}

function createDb() {
  const url = process.env.DATABASE_URL;
  if (url) return createPg(url);
  console.warn('⚠  DATABASE_URL no definida: usando almacenamiento en MEMORIA (solo para desarrollo).');
  return createMemory();
}

module.exports = { createDb };
