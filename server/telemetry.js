// Normaliza el JSON que envía el ESP32 a las columnas de la tabla `telemetry`.
// Acepta nombres en español/inglés y abreviados; lo que no reconoce va a `extra`.

const FIELDS = {
  temperature: ['temperature', 'temperatura', 'temp', 't'],
  humidity: ['humidity', 'humedad', 'hum', 'h'],
  pressure: ['pressure', 'presion', 'presión', 'pres', 'p'],
  altitude: ['altitude', 'altitud', 'alt'],
  latitude: ['latitude', 'latitud', 'lat'],
  longitude: ['longitude', 'longitud', 'lon', 'lng'],
  speed: ['speed', 'velocidad', 'vel', 'spd'],
  battery: ['battery', 'bateria', 'batería', 'bat', 'vbat'],
  rssi: ['rssi', 'senal', 'señal'],
};

const META = {
  device_id: ['device_id', 'deviceId', 'device', 'id', 'dispositivo'],
  device_ts: ['ts', 'timestamp', 'time', 'fecha'],
  seq: ['seq', 'packet', 'paquete', 'n'],
};

const RANGES = {
  temperature: [-90, 90],
  humidity: [0, 100],
  pressure: [100, 1200],
  altitude: [-500, 50000],
  latitude: [-90, 90],
  longitude: [-180, 180],
  speed: [0, 2000],
  battery: [0, 60],
  rssi: [-150, 20],
};

const ALIAS = new Map();
for (const [col, names] of Object.entries({ ...FIELDS, ...META })) {
  for (const n of names) ALIAS.set(n.toLowerCase(), col);
}

function toNumber(v) {
  if (v === null || v === undefined || v === '') return null;
  const n = typeof v === 'number' ? v : Number(String(v).replace(',', '.'));
  return Number.isFinite(n) ? n : null;
}

function toDate(v) {
  if (v === null || v === undefined || v === '') return null;
  // Epoch en segundos (ESP32 con NTP) o milisegundos
  if (typeof v === 'number' || /^\d+$/.test(String(v))) {
    const n = Number(v);
    const ms = n < 1e12 ? n * 1000 : n;
    const d = new Date(ms);
    // Ignorar relojes sin sincronizar (p. ej. millis() desde el arranque)
    return d.getFullYear() >= 2020 ? d : null;
  }
  const d = new Date(v);
  return Number.isNaN(d.getTime()) ? null : d;
}

/**
 * @returns {{ ok: true, reading: object } | { ok: false, error: string }}
 */
function normalize(input, fallbackDeviceId) {
  if (!input || typeof input !== 'object' || Array.isArray(input)) {
    return { ok: false, error: 'Cada lectura debe ser un objeto JSON' };
  }

  const reading = { extra: {} };
  const warnings = [];

  for (const [rawKey, value] of Object.entries(input)) {
    const col = ALIAS.get(rawKey.toLowerCase());

    if (!col) {
      if (rawKey.length <= 64 && value !== undefined) reading.extra[rawKey] = value;
      continue;
    }
    if (col === 'device_id') {
      reading.device_id = String(value).trim().slice(0, 64);
    } else if (col === 'device_ts') {
      reading.device_ts = toDate(value);
    } else if (col === 'seq') {
      const n = toNumber(value);
      reading.seq = n === null ? null : Math.trunc(n);
    } else {
      const n = toNumber(value);
      const [min, max] = RANGES[col];
      if (n !== null && (n < min || n > max)) {
        warnings.push(`${col} fuera de rango (${n})`);
        reading[col] = null;
      } else {
        reading[col] = col === 'rssi' && n !== null ? Math.round(n) : n;
      }
    }
  }

  // GPS sin fix suele mandar 0,0
  if (reading.latitude === 0 && reading.longitude === 0) {
    reading.latitude = null;
    reading.longitude = null;
  }

  reading.device_id = reading.device_id || fallbackDeviceId || 'esp32-01';
  if (!/^[\w.:-]{1,64}$/.test(reading.device_id)) {
    return { ok: false, error: 'device_id inválido (use letras, números, - _ . :)' };
  }

  if (JSON.stringify(reading.extra).length > 4096) {
    return { ok: false, error: 'Campos extra demasiado grandes (máx. 4 KB)' };
  }

  return { ok: true, reading, warnings };
}

const COLUMNS = Object.keys(FIELDS);

module.exports = { normalize, COLUMNS };
