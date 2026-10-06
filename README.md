# Misión Domuyo · Titan ATLAS

Plataforma de telemetría del equipo Titan ATLAS para la competencia Misión Domuyo.
Un ESP32 envía lecturas por HTTPS → servidor Node.js en Railway → PostgreSQL → centro de control web en tiempo real.

```
ESP32 ──POST /api/telemetry (JSON)──▶ Express ──▶ PostgreSQL
                                         │
                                         └─ SSE /api/stream ──▶ Navegador (panel en vivo)
```

## Base de datos: PostgreSQL

Se eligió **PostgreSQL** sobre MySQL por:

| Criterio | PostgreSQL | MySQL |
|---|---|---|
| Sensores que todavía no están definidos | Columna `JSONB` indexable: cualquier campo nuevo del ESP32 se guarda sin cambiar el esquema | `JSON` existe pero con menos operadores e indexado |
| Series de tiempo | `TIMESTAMPTZ`, agregación por intervalos, migración directa a TimescaleDB si crece el volumen | Funciona, menos herramientas |
| Railway | Plantilla oficial, se vincula con `DATABASE_URL` | También disponible |

## Estructura

```
server/          API (Express) y capa de datos
  index.js       rutas, autenticación, tiempo real (SSE), archivos estáticos
  db.js          PostgreSQL (producción) / memoria (desarrollo sin BD)
  telemetry.js   normalización y validación de lo que envía el ESP32
db/schema.sql    esquema (se aplica solo al iniciar)
public/          frontend
  js/sensors.js  catálogo de sensores (etiquetas, unidades, decimales)
  marca.html     kit de marca con descargas
  assets/brand/  logotipos SVG y PNG
firmware/        sketch Arduino para el ESP32
scripts/build-brand.js  genera todos los logos (npm run brand)
```

## Despliegue en Railway

1. **New Project → Deploy from GitHub repo** y elegir este repositorio.
2. En el mismo proyecto: **New → Database → PostgreSQL**.
3. En el servicio web → **Variables**:
   - `DATABASE_URL` → *Add Reference* → Postgres → `DATABASE_URL`
   - `API_KEY` → una clave larga y aleatoria (la misma que va en el firmware)
4. **Settings → Networking → Generate Domain**. Esa URL es la del panel y la del ESP32.

La tabla se crea automáticamente al iniciar. El health check está en `/api/health`.
Sin `API_KEY`, en Railway la ingesta se rechaza (503) para no dejarla abierta.

## Desarrollo local

```bash
npm install
npm start
```

Sin `DATABASE_URL` usa almacenamiento en memoria (se borra al reiniciar). Para PostgreSQL local:

```bash
docker compose up -d
```

y definir `DATABASE_URL=postgresql://postgres:postgres@localhost:5432/domuyo` (ver `.env.example`).

## API

### `POST /api/telemetry` — ingesta desde el ESP32

Headers: `Content-Type: application/json`, `X-API-Key: <API_KEY>`

Un objeto o un arreglo de hasta 200 lecturas (para enviar lo acumulado sin conexión):

```json
{
  "device_id": "esp32-01",
  "ts": 1791300000,
  "seq": 42,
  "temperature": 12.4,
  "humidity": 38.1,
  "pressure": 612.3,
  "altitude": 4120,
  "latitude": -36.6401,
  "longitude": -70.4412,
  "battery": 3.92,
  "rssi": -71,
  "co2_ppm": 415
}
```

- Todos los campos son opcionales salvo que conviene enviar `device_id`.
- Columnas propias: `temperature`, `humidity`, `pressure`, `altitude`, `latitude`, `longitude`, `speed`, `battery`, `rssi`. También acepta alias (`temp`, `hum`, `presion`, `alt`, `lat`, `lon`, `bat`…).
- **Cualquier otro campo** (p. ej. `co2_ppm`) se guarda en `extra` (JSONB) y aparece en el panel automáticamente, con tarjeta, gráfico y columna en la tabla.
- `ts` es opcional (epoch en segundos o ms, o ISO 8601); el servidor siempre registra su propia hora de recepción.
- Valores fuera de rango físico se guardan como `NULL` y se informan en `warnings`.

Respuesta `201`: `{ "ok": true, "stored": 1, "ids": [123], "warnings": [] }`

### Consulta (públicas, solo lectura)

| Ruta | Descripción |
|---|---|
| `GET /api/telemetry?device=&from=&to=&limit=` | Lecturas crudas (máx. 5000) |
| `GET /api/telemetry/latest?device=` | Última lectura |
| `GET /api/series?range=1h&device=` | Serie agregada (~300 puntos). `range`: 15m, 1h, 6h, 24h, 7d, 30d |
| `GET /api/stats?range=&device=` | Conteo, mín/máx/promedio |
| `GET /api/track?device=` | Recorrido GPS |
| `GET /api/devices` | Dispositivos y si están en línea |
| `GET /api/export.csv?range=&device=` | Exportación CSV |
| `GET /api/stream` | Eventos en tiempo real (SSE) |
| `GET /api/health` | Estado del servidor y la base |

## Firmware ESP32

`firmware/esp32_mision_domuyo/esp32_mision_domuyo.ino`. Completar WiFi, `SERVER_URL`, `API_KEY` y `DEVICE_ID`.
Guarda hasta 120 lecturas si se pierde la conexión y las envía por lotes al reconectar.
Los bloques de sensores (`USE_BME280`, `USE_GPS`) son plantillas: se ajustan cuando se definan los sensores finales.

## Agregar un sensor

1. En el firmware, agregar el campo al JSON (`o["nombre"] = valor;`).
2. Listo: el servidor lo guarda y el panel lo muestra.
3. Opcional: en `public/js/sensors.js`, agregar
   `{ key: 'nombre', label: '…', unit: '…', digits: 1, info: 'qué mide', insight: (r) => 'interpretación' }`
   para darle etiqueta, unidad, una explicación (botón **?** de la tarjeta) y una interpretación calculada del dato.

## Identidad visual

Logos en `public/assets/brand/` y en la página `/marca`. Para regenerarlos: `npm run brand`.
