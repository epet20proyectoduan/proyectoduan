-- ============================================================
--  Misión Domuyo · Titan ATLAS
--  Esquema PostgreSQL (se aplica automáticamente al iniciar)
-- ============================================================

-- Dispositivos (cada ESP32 se registra solo al enviar su primer dato)
CREATE TABLE IF NOT EXISTS devices (
  id          TEXT PRIMARY KEY,
  name        TEXT,
  first_seen  TIMESTAMPTZ NOT NULL DEFAULT now(),
  last_seen   TIMESTAMPTZ NOT NULL DEFAULT now(),
  packets     BIGINT      NOT NULL DEFAULT 0
);

-- Lecturas de telemetría
CREATE TABLE IF NOT EXISTS telemetry (
  id           BIGSERIAL PRIMARY KEY,
  device_id    TEXT        NOT NULL REFERENCES devices(id) ON DELETE CASCADE,
  received_at  TIMESTAMPTZ NOT NULL DEFAULT now(),  -- hora del servidor
  device_ts    TIMESTAMPTZ,                         -- hora del ESP32 (si tiene RTC/NTP/GPS)
  seq          INTEGER,                             -- n° de paquete del ESP32

  temperature  REAL,   -- °C
  humidity     REAL,   -- %
  pressure     REAL,   -- hPa
  altitude     REAL,   -- m s.n.m.
  latitude     DOUBLE PRECISION,
  longitude    DOUBLE PRECISION,
  speed        REAL,   -- km/h
  battery      REAL,   -- V
  rssi         INTEGER,-- dBm

  extra        JSONB   NOT NULL DEFAULT '{}'::jsonb  -- cualquier sensor adicional
);

CREATE INDEX IF NOT EXISTS idx_telemetry_device_time
  ON telemetry (device_id, received_at DESC);

CREATE INDEX IF NOT EXISTS idx_telemetry_time
  ON telemetry (received_at DESC);
