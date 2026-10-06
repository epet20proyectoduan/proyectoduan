/* Catálogo de variables que muestra el centro de control.
 *
 * Solo se muestran las variables que el ESP32 efectivamente envía: si un sensor
 * no transmite, su tarjeta y su gráfico no aparecen.
 *
 * Cualquier campo que el ESP32 mande y no esté acá (se guarda en la columna
 * `extra`) también se muestra, con su nombre tal cual. Agregarlo a este catálogo
 * sirve para darle etiqueta, unidad, decimales y una explicación.
 *
 *   key      nombre del campo en el JSON
 *   label    texto que se muestra
 *   unit     unidad
 *   digits   decimales
 *   group    'ambiente' | 'posicion' | 'sistema' (orden de las tarjetas)
 *   color    color de la tarjeta y del gráfico
 *   info     explicación breve: qué mide y por qué importa en la montaña
 *   insight  función (lectura) → texto calculado a partir del dato real, o null
 *   chart    false para no graficar · card: false para no mostrar tarjeta
 */

// Cálculos físicos usados para interpretar las lecturas
window.PHYS = (() => {
  const P0 = 1013.25; // hPa, nivel del mar (atmósfera estándar)
  return {
    P0,
    // Presión estimada por altura (atmósfera estándar internacional)
    pressureAt: (alt) => P0 * Math.pow(1 - 2.25577e-5 * alt, 5.25588),
    // Punto de ebullición del agua según la presión (Clausius–Clapeyron)
    boilingPoint: (p) => 1 / (1 / 373.15 - (8.314 / 40660) * Math.log(p / P0)) - 273.15,
    // Punto de rocío (fórmula de Magnus)
    dewPoint: (t, rh) => {
      const a = 17.62, b = 243.12;
      const g = Math.log(rh / 100) + (a * t) / (b + t);
      return (b * g) / (a - g);
    },
  };
})();

(() => {
  const nf = (v, d) => v.toLocaleString('es-AR', { minimumFractionDigits: d, maximumFractionDigits: d });
  const { P0, pressureAt, boilingPoint, dewPoint } = window.PHYS;

  window.SENSORS = [
    {
      key: 'temperature', label: 'Temperatura', unit: '°C', digits: 1, group: 'ambiente', color: '#FF8A4C',
      info: 'Temperatura del aire alrededor de la estación. En la atmósfera desciende en promedio unos 6,5 °C por cada 1000 m de altura.',
      insight: (r) => (r.temperature == null ? null
        : r.temperature <= 0 ? 'Bajo cero: el agua se congela' : null),
    },
    {
      key: 'humidity', label: 'Humedad relativa', unit: '%', digits: 1, group: 'ambiente', color: '#38BDF8',
      info: 'Cuánto vapor de agua tiene el aire respecto del máximo que admite a esa temperatura. El punto de rocío es la temperatura a la que ese vapor se condensa (rocío, escarcha o niebla).',
      insight: (r) => (r.temperature != null && r.humidity > 0
        ? `Punto de rocío ${nf(dewPoint(r.temperature, r.humidity), 1)} °C` : null),
    },
    {
      key: 'pressure', label: 'Presión', unit: 'hPa', digits: 1, group: 'ambiente', color: '#A78BFA',
      info: 'Peso de la columna de aire sobre la estación. Disminuye al subir: con menos presión cada respiración aporta menos oxígeno y el agua hierve a menor temperatura.',
      insight: (r) => (r.pressure == null ? null
        : `${Math.round((r.pressure / P0) * 100)}% del nivel del mar · el agua hierve a ${nf(boilingPoint(r.pressure), 0)} °C`),
    },
    {
      key: 'altitude', label: 'Altitud', unit: 'm', digits: 0, group: 'posicion', color: '#34D399',
      info: 'Altura sobre el nivel del mar, medida por el GPS o calculada a partir de la presión atmosférica.',
      insight: (r) => (r.altitude == null || r.pressure != null ? null
        : `Presión estimada a esta altura: ${nf(pressureAt(r.altitude), 0)} hPa`),
    },
    {
      key: 'speed', label: 'Velocidad', unit: 'km/h', digits: 1, group: 'posicion', color: '#F472B6',
      info: 'Velocidad de desplazamiento calculada por el GPS a partir de posiciones sucesivas.',
    },
    { key: 'latitude', label: 'Latitud', unit: '°', digits: 5, group: 'posicion', chart: false, card: false },
    { key: 'longitude', label: 'Longitud', unit: '°', digits: 5, group: 'posicion', chart: false, card: false },
    {
      key: 'battery', label: 'Batería', unit: 'V', digits: 2, group: 'sistema', color: '#FACC15',
      info: 'Tensión de la batería de la estación. Con frío intenso las baterías entregan menos energía, por eso conviene vigilarla de cerca.',
    },
    {
      key: 'rssi', label: 'Señal WiFi', unit: 'dBm', digits: 0, group: 'sistema', color: '#22D3EE',
      info: 'Intensidad de la señal que recibe el ESP32, en decibel-milivatios. Cuanto más cerca de 0, mejor: −50 es excelente, −70 aceptable y por debajo de −85 la conexión se vuelve inestable.',
      insight: (r) => (r.rssi == null ? null
        : r.rssi >= -60 ? 'Señal excelente' : r.rssi >= -70 ? 'Señal buena' : r.rssi >= -80 ? 'Señal regular' : 'Señal débil'),
    },
  ];
})();
