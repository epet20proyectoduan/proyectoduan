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
      info: 'Altura sobre el nivel del mar calculada por el BMP390 a partir de la presión atmosférica. Puede variar unos metros si cambia el clima.',
      insight: (r) => (r.altitude == null || r.pressure != null ? null
        : `Presión estimada a esta altura: ${nf(pressureAt(r.altitude), 0)} hPa`),
    },
    {
      key: 'alt_rel', label: 'Altura relativa', unit: 'm', digits: 1, group: 'posicion', color: '#86EFAC',
      info: 'Cuánto subió o bajó la estación desde que se encendió, según el barómetro. Es más precisa que la altitud absoluta para medir cambios de altura.',
      insight: (r) => (r.alt_rel == null ? null : r.alt_rel >= 0 ? `${nf(r.alt_rel, 1)} m por encima del punto de inicio` : `${nf(-r.alt_rel, 1)} m por debajo del punto de inicio`),
    },
    {
      key: 'speed', label: 'Velocidad', unit: 'km/h', digits: 1, group: 'posicion', color: '#F472B6',
      info: 'Velocidad medida por el velocímetro: cuenta los pulsos del sensor en cada vuelta y los convierte a km/h según la circunferencia configurada en el firmware.',
      insight: (r) => (r.speed == null ? null : `${nf(r.speed / 3.6, 2)} m/s`),
    },
    {
      key: 'gps_speed', label: 'Velocidad GPS', unit: 'km/h', digits: 1, group: 'posicion', color: '#F9A8D4',
      info: 'Velocidad calculada por el GPS a partir de posiciones sucesivas. Sirve para comparar con el velocímetro; a baja velocidad el GPS es menos preciso.',
    },
    {
      key: 'gps_altitude', label: 'Altitud GPS', unit: 'm', digits: 0, group: 'posicion', color: '#5EEAD4',
      info: 'Altura sobre el nivel del mar según el GPS. Suele tener un error de 10 a 20 m; se usa para contrastar con la del barómetro.',
      insight: (r) => (r.gps_altitude == null || r.altitude == null ? null : `Diferencia con el barómetro: ${nf(r.gps_altitude - r.altitude, 0)} m`),
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
    {
      key: 'satellites', label: 'Satélites GPS', unit: '', digits: 0, group: 'sistema', color: '#93C5FD', chart: false,
      info: 'Cantidad de satélites que usa el GPS. Con 4 o más calcula posición y altura; con 6 o más la posición es confiable. Necesita cielo abierto.',
      insight: (r) => (r.satellites == null ? null
        : r.satellites >= 6 ? 'Posición confiable' : r.satellites >= 4 ? 'Posición aceptable' : 'Sin posición: buscando satélites'),
    },
    {
      key: 'hdop', label: 'Precisión GPS (HDOP)', unit: '', digits: 1, group: 'sistema', color: '#C4B5FD', chart: false,
      info: 'Indica qué tan buena es la geometría de los satélites. Cuanto más bajo, mejor: menos de 1 es ideal, de 1 a 2 excelente, de 2 a 5 buena y más de 5 pobre.',
      insight: (r) => (r.hdop == null ? null
        : r.hdop < 1 ? 'Precisión ideal' : r.hdop <= 2 ? 'Precisión excelente' : r.hdop <= 5 ? 'Precisión buena' : 'Precisión pobre'),
    },
    { key: 'uptime_s', label: 'Tiempo encendido', unit: 's', digits: 0, group: 'sistema', color: '#94A3B8', chart: false, card: false },
    { key: 'seq', label: 'Paquete', unit: '', digits: 0, card: false, chart: false },
  ];
})();
