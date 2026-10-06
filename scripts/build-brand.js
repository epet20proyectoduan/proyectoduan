// Genera la identidad visual de Titan ATLAS · Misión Domuyo.
// Todos los textos se convierten a trazos vectoriales (no dependen de fuentes instaladas).
// Uso: npm run brand   → escribe en public/assets/brand/

const fs = require('fs');
const path = require('path');
const opentype = require('opentype.js');
const sharp = require('sharp');

const OUT = path.join(__dirname, '..', 'public', 'assets', 'brand');
const FONTS = path.join(__dirname, '..', 'node_modules', '@fontsource');
fs.mkdirSync(OUT, { recursive: true });

const loadFont = (pkg, file) => {
  const buf = fs.readFileSync(path.join(FONTS, pkg, 'files', file));
  return opentype.parse(buf.buffer.slice(buf.byteOffset, buf.byteOffset + buf.byteLength));
};
const GROTESK = loadFont('space-grotesk', 'space-grotesk-latin-700-normal.woff');
const MONO = loadFont('jetbrains-mono', 'jetbrains-mono-latin-500-normal.woff');

// ---------------------------------------------------------------- paleta
const C = {
  night: '#050B16',
  navy: '#0A1628',
  deep: '#11233D',
  volcano: '#FF5A1F',
  ember: '#FF8A3D',
  glacier: '#38BDF8',
  ice: '#7DD3FC',
  snow: '#F8FAFC',
  mist: '#B8C7DA',
  rockLit: '#4A6FA0',
  rockLit2: '#2C4A72',
  rockDark: '#22395C',
  rockDark2: '#142640',
  ink: '#0A1628',
};

// ---------------------------------------------------------------- texto → trazos
const r2 = (n) => Math.round(n * 100) / 100;

function cmdsToD(cmds) {
  return cmds
    .map((c) => {
      switch (c.type) {
        case 'M': return `M${r2(c.x)} ${r2(c.y)}`;
        case 'L': return `L${r2(c.x)} ${r2(c.y)}`;
        case 'Q': return `Q${r2(c.x1)} ${r2(c.y1)} ${r2(c.x)} ${r2(c.y)}`;
        case 'C': return `C${r2(c.x1)} ${r2(c.y1)} ${r2(c.x2)} ${r2(c.y2)} ${r2(c.x)} ${r2(c.y)}`;
        case 'Z': return 'Z';
        default: return '';
      }
    })
    .join('');
}

function layout(font, text, size, tracking = 0) {
  const glyphs = font.stringToGlyphs(text);
  const scale = size / font.unitsPerEm;
  const items = [];
  let x = 0;
  glyphs.forEach((g, i) => {
    const adv = g.advanceWidth * scale;
    items.push({ g, x, adv });
    x += adv + tracking * size;
    if (glyphs[i + 1]) x += font.getKerningValue(g, glyphs[i + 1]) * scale;
  });
  const width = x - tracking * size; // sin tracking después de la última letra
  return { items, width };
}

/** Texto en línea recta. anchor: start | middle | end */
function textPath(font, text, x, y, size, { tracking = 0, anchor = 'start' } = {}) {
  const { items, width } = layout(font, text, size, tracking);
  const x0 = anchor === 'middle' ? x - width / 2 : anchor === 'end' ? x - width : x;
  const d = items.map((it) => cmdsToD(it.g.getPath(x0 + it.x, y, size).commands)).join('');
  return { d, width };
}

/** Texto sobre un arco. position: top (lee por arriba) | bottom (lee por abajo) */
function arcText(font, text, cx, cy, r, size, { tracking = 0, position = 'top' } = {}) {
  const { items, width } = layout(font, text, size, tracking);
  const top = position === 'top';
  const start = (top ? -Math.PI / 2 : Math.PI / 2) + (top ? -1 : 1) * (width / 2 / r);
  return items
    .map((it) => {
      const s = (it.x + it.adv / 2) / r;
      const theta = top ? start + s : start - s;
      const rot = top ? theta + Math.PI / 2 : theta - Math.PI / 2;
      const px = cx + r * Math.cos(theta);
      const py = cy + r * Math.sin(theta);
      const cos = Math.cos(rot);
      const sin = Math.sin(rot);
      const tf = (X, Y) => [px + X * cos - Y * sin, py + X * sin + Y * cos];
      const cmds = it.g.getPath(-it.adv / 2, 0, size).commands.map((c) => {
        const o = { type: c.type };
        for (const [kx, ky] of [['x', 'y'], ['x1', 'y1'], ['x2', 'y2']]) {
          if (c[kx] !== undefined) [o[kx], o[ky]] = tf(c[kx], c[ky]);
        }
        return o;
      });
      return cmdsToD(cmds);
    })
    .join('');
}

// ---------------------------------------------------------------- isotipo
// Volcán Domuyo + órbita (ATLAS) + señal transmitiendo desde la cumbre.
// Sistema de coordenadas 512×512.
const ORBIT = { cx: 256, cy: 290, rx: 214, ry: 64, rot: -16 };
const PEAK = [256, 150];
const MOUNTAIN_LEFT = 'M256 150 L206 222 L184 236 L98 382 L298 382 Z';
const MOUNTAIN_RIGHT = 'M256 150 L298 382 L420 382 Z';
const SNOW_LEFT = 'M256 150 L215 209 L231 203 L243 217 L256 205 L267 213 Z';
const SNOW_RIGHT = 'M256 150 L267 213 L279 204 L289 211 L295 205 Z';
const SNOWLINE = 'M215 209 L231 203 L243 217 L256 205 L267 213 L279 204 L289 211 L295 205';
const RIDGE = 'M267 213 L298 382';
const ORBIT_FRONT = `M${ORBIT.cx - ORBIT.rx} ${ORBIT.cy} A${ORBIT.rx} ${ORBIT.ry} 0 0 0 ${ORBIT.cx + ORBIT.rx} ${ORBIT.cy}`;
const SAT = (() => {
  const a = (40 * Math.PI) / 180;
  return [ORBIT.cx + ORBIT.rx * Math.cos(a), ORBIT.cy + ORBIT.ry * Math.sin(a)];
})();
const arc = (r) => {
  const k = r * Math.SQRT1_2;
  return `M${r2(PEAK[0] - k)} ${r2(PEAK[1] - k)} A${r} ${r} 0 0 1 ${r2(PEAK[0] + k)} ${r2(PEAK[1] - k)}`;
};
const orbitG = (inner) => `<g transform="rotate(${ORBIT.rot} ${ORBIT.cx} ${ORBIT.cy})">${inner}</g>`;

/** Isotipo a color. `id` evita colisiones de defs cuando se incrusta varias veces. */
function markColor(id = 'm') {
  return `
  <defs>
    <linearGradient id="${id}L" x1="0" y1="0" x2="0" y2="1"><stop offset="0" stop-color="${C.rockLit}"/><stop offset="1" stop-color="${C.rockLit2}"/></linearGradient>
    <linearGradient id="${id}R" x1="0" y1="0" x2="0" y2="1"><stop offset="0" stop-color="${C.rockDark}"/><stop offset="1" stop-color="${C.rockDark2}"/></linearGradient>
    <linearGradient id="${id}O" x1="0" y1="0" x2="1" y2="0"><stop offset="0" stop-color="${C.ice}"/><stop offset="1" stop-color="#0EA5E9"/></linearGradient>
    <linearGradient id="${id}S" x1="0" y1="0" x2="1" y2="0"><stop offset="0" stop-color="${C.ember}"/><stop offset="1" stop-color="${C.volcano}"/></linearGradient>
    <mask id="${id}K" maskUnits="userSpaceOnUse" x="0" y="0" width="512" height="512">
      <rect width="512" height="512" fill="#fff"/>
      ${orbitG(`<path d="${ORBIT_FRONT}" fill="none" stroke="#000" stroke-width="34"/>`)}
    </mask>
  </defs>
  ${orbitG(`<ellipse cx="${ORBIT.cx}" cy="${ORBIT.cy}" rx="${ORBIT.rx}" ry="${ORBIT.ry}" fill="none" stroke="url(#${id}O)" stroke-width="14" opacity=".45"/>`)}
  <g mask="url(#${id}K)">
    <path d="${MOUNTAIN_LEFT}" fill="url(#${id}L)"/>
    <path d="${MOUNTAIN_RIGHT}" fill="url(#${id}R)"/>
    <path d="${SNOW_LEFT}" fill="${C.snow}"/>
    <path d="${SNOW_RIGHT}" fill="${C.mist}"/>
  </g>
  ${orbitG(`
    <path d="${ORBIT_FRONT}" fill="none" stroke="url(#${id}O)" stroke-width="14" stroke-linecap="round"/>
    <circle cx="${r2(SAT[0])}" cy="${r2(SAT[1])}" r="28" fill="${C.glacier}" opacity=".18"/>
    <circle cx="${r2(SAT[0])}" cy="${r2(SAT[1])}" r="13" fill="${C.snow}" stroke="${C.glacier}" stroke-width="6"/>`)}
  <g fill="none" stroke="url(#${id}S)" stroke-width="12" stroke-linecap="round">
    <path d="${arc(40)}"/><path d="${arc(68)}" opacity=".7"/>
  </g>
  <circle cx="${PEAK[0]}" cy="${PEAK[1]}" r="8" fill="${C.volcano}"/>`;
}

/** Isotipo monocromo (un solo color, con cortes por máscara). */
function markMono(color, id = 'mm') {
  return `
  <defs>
    <mask id="${id}K" maskUnits="userSpaceOnUse" x="0" y="0" width="512" height="512">
      <rect width="512" height="512" fill="#fff"/>
      ${orbitG(`<path d="${ORBIT_FRONT}" fill="none" stroke="#000" stroke-width="34"/>`)}
      <path d="${SNOWLINE}" fill="none" stroke="#000" stroke-width="9" stroke-linejoin="round"/>
      <path d="${RIDGE}" fill="none" stroke="#000" stroke-width="7"/>
      <circle cx="${PEAK[0]}" cy="${PEAK[1]}" r="16" fill="#000"/>
    </mask>
  </defs>
  ${orbitG(`<ellipse cx="${ORBIT.cx}" cy="${ORBIT.cy}" rx="${ORBIT.rx}" ry="${ORBIT.ry}" fill="none" stroke="${color}" stroke-width="12" opacity=".5"/>`)}
  <g mask="url(#${id}K)" fill="${color}"><path d="${MOUNTAIN_LEFT}"/><path d="${MOUNTAIN_RIGHT}"/></g>
  ${orbitG(`<path d="${ORBIT_FRONT}" fill="none" stroke="${color}" stroke-width="14" stroke-linecap="round"/>
    <circle cx="${r2(SAT[0])}" cy="${r2(SAT[1])}" r="16" fill="${color}"/>`)}
  <g fill="none" stroke="${color}" stroke-width="12" stroke-linecap="round"><path d="${arc(40)}"/><path d="${arc(68)}"/></g>
  <circle cx="${PEAK[0]}" cy="${PEAK[1]}" r="8" fill="${color}"/>`;
}

// El isotipo ocupa aprox. x 40–476, y 90–404 → recorte cuadrado centrado.
const MARK_BOX = { x: 18, y: 8, s: 480 };
const placeMark = (inner, x, y, size) =>
  `<g transform="translate(${r2(x)} ${r2(y)}) scale(${r2(size / MARK_BOX.s)}) translate(${-MARK_BOX.x} ${-MARK_BOX.y})">${inner}</g>`;

const svg = (w, h, body, title) =>
  `<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 ${r2(w)} ${r2(h)}" width="${r2(w)}" height="${r2(h)}" role="img" aria-label="${title}"><title>${title}</title>${body}</svg>\n`;

// ---------------------------------------------------------------- piezas
const files = {};

// 1. Isotipo
files['isotipo.svg'] = svg(MARK_BOX.s, MARK_BOX.s, placeMark(markColor('a'), 0, 0, MARK_BOX.s), 'Titan ATLAS');
files['isotipo-blanco.svg'] = svg(MARK_BOX.s, MARK_BOX.s, placeMark(markMono('#FFFFFF', 'b'), 0, 0, MARK_BOX.s), 'Titan ATLAS');
files['isotipo-negro.svg'] = svg(MARK_BOX.s, MARK_BOX.s, placeMark(markMono(C.ink, 'c'), 0, 0, MARK_BOX.s), 'Titan ATLAS');

// 2. Ícono de app / favicon (fondo navy)
const appIcon = (size) =>
  svg(size, size, `
  <defs><radialGradient id="bg" cx=".5" cy=".3" r=".8"><stop offset="0" stop-color="${C.deep}"/><stop offset="1" stop-color="${C.night}"/></radialGradient></defs>
  <rect width="${size}" height="${size}" rx="${size * 0.22}" fill="url(#bg)"/>
  ${placeMark(markColor('d'), size * 0.08, size * 0.08, size * 0.84)}`, 'Titan ATLAS');
files['app-icon.svg'] = appIcon(512);

// 3. Logo horizontal (oscuro y claro)
function horizontal(dark, mono) {
  const H = 240;
  const textX = H + 28;
  const title = textPath(GROTESK, 'TITAN ATLAS', textX, 132, 104, { tracking: 0.06 });
  const sub = textPath(MONO, 'MISIÓN DOMUYO', textX + 4, 186, 34, { tracking: 0.42 });
  const W = textX + Math.max(title.width, sub.width + 4) + 8;
  const fg = mono || (dark ? C.snow : C.ink);
  const accent = mono || C.volcano;
  const mark = mono ? markMono(mono, dark ? 'hm' : 'hn') : markColor(dark ? 'h' : 'i');
  return svg(W, H, `
  ${placeMark(mark, 0, 0, H)}
  <path d="${title.d}" fill="${fg}"/>
  <rect x="${textX + 2}" y="150" width="56" height="5" rx="2.5" fill="${accent}"/>
  <path d="${sub.d}" fill="${accent}"/>`, 'Titan ATLAS — Misión Domuyo');
}
files['logo-horizontal-oscuro.svg'] = horizontal(true);
files['logo-horizontal-claro.svg'] = horizontal(false);
files['logo-horizontal-blanco.svg'] = horizontal(true, '#FFFFFF');
files['logo-horizontal-negro.svg'] = horizontal(false, C.ink);

// 4. Logo vertical
function vertical(dark) {
  const W = 760;
  const title = textPath(GROTESK, 'TITAN ATLAS', W / 2, 520, 112, { tracking: 0.06, anchor: 'middle' });
  const sub = textPath(MONO, 'MISIÓN DOMUYO', W / 2, 590, 36, { tracking: 0.42, anchor: 'middle' });
  return svg(W, 630, `
  ${placeMark(markColor(dark ? 'v' : 'w'), (W - 400) / 2, 10, 400)}
  <path d="${title.d}" fill="${dark ? C.snow : C.ink}"/>
  <path d="${sub.d}" fill="${C.volcano}"/>`, 'Titan ATLAS — Misión Domuyo');
}
files['logo-vertical-oscuro.svg'] = vertical(true);
files['logo-vertical-claro.svg'] = vertical(false);

// 5. Parche de misión (insignia circular)
function patch() {
  const S = 1024;
  const c = S / 2;
  const stars = [];
  let seed = 7;
  const rnd = () => ((seed = (seed * 16807) % 2147483647) / 2147483647);
  for (let i = 0; i < 70; i++) {
    const a = rnd() * Math.PI * 2;
    const rr = Math.sqrt(rnd()) * 330;
    const x = c + rr * Math.cos(a);
    const y = c - 40 + rr * Math.sin(a) * 0.7 - 60;
    if (y < c + 120) stars.push(`<circle cx="${r2(x)}" cy="${r2(y)}" r="${r2(0.8 + rnd() * 2)}" fill="#fff" opacity="${r2(0.3 + rnd() * 0.6)}"/>`);
  }
  const top = arcText(GROTESK, 'MISIÓN DOMUYO', c, c, 410, 74, { tracking: 0.12, position: 'top' });
  const bottom = arcText(GROTESK, 'TITAN ATLAS', c, c, 440, 74, { tracking: 0.12, position: 'bottom' });
  const coords = textPath(MONO, '36°38′S · 70°26′W · 4709 m', c, 740, 26, { tracking: 0.12, anchor: 'middle' });
  const star = (x, y, r) => {
    const p = [];
    for (let i = 0; i < 10; i++) {
      const a = -Math.PI / 2 + (i * Math.PI) / 5;
      const rad = i % 2 ? r * 0.42 : r;
      p.push(`${r2(x + rad * Math.cos(a))},${r2(y + rad * Math.sin(a))}`);
    }
    return `<polygon points="${p.join(' ')}" fill="${C.volcano}"/>`;
  };
  return svg(S, S, `
  <defs>
    <radialGradient id="sky" cx=".5" cy=".25" r=".9"><stop offset="0" stop-color="#1B3A63"/><stop offset=".6" stop-color="${C.navy}"/><stop offset="1" stop-color="${C.night}"/></radialGradient>
    <clipPath id="in"><circle cx="${c}" cy="${c}" r="340"/></clipPath>
  </defs>
  <circle cx="${c}" cy="${c}" r="508" fill="${C.night}"/>
  <circle cx="${c}" cy="${c}" r="496" fill="none" stroke="${C.volcano}" stroke-width="8"/>
  <circle cx="${c}" cy="${c}" r="352" fill="none" stroke="${C.glacier}" stroke-width="6"/>
  <circle cx="${c}" cy="${c}" r="340" fill="url(#sky)"/>
  <g clip-path="url(#in)">
    ${stars.join('')}
    ${placeMark(markColor('p'), c - 260, c - 320, 520)}
    <path d="${coords.d}" fill="${C.ice}"/>
  </g>
  <path d="${top}" fill="${C.snow}"/>
  <path d="${bottom}" fill="${C.snow}"/>
  ${star(c - 448, c, 20)}${star(c + 448, c, 20)}`, 'Parche de misión — Misión Domuyo · Titan ATLAS');
}
files['parche-mision.svg'] = patch();

// 6. Imagen para redes (Open Graph 1200×630)
function ogImage() {
  const W = 1200, H = 630;
  const title = textPath(GROTESK, 'TITAN ATLAS', 470, 290, 92, { tracking: 0.05 });
  const sub = textPath(MONO, 'MISIÓN DOMUYO', 474, 345, 30, { tracking: 0.42 });
  const tag = textPath(GROTESK, 'Telemetría en tiempo real desde el techo de la Patagonia', 474, 420, 28);
  return svg(W, H, `
  <defs>
    <linearGradient id="g" x1="0" y1="0" x2="1" y2="1"><stop offset="0" stop-color="${C.deep}"/><stop offset="1" stop-color="${C.night}"/></linearGradient>
    <radialGradient id="glow" cx=".22" cy=".5" r=".45"><stop offset="0" stop-color="${C.glacier}" stop-opacity=".22"/><stop offset="1" stop-color="${C.glacier}" stop-opacity="0"/></radialGradient>
  </defs>
  <rect width="${W}" height="${H}" fill="url(#g)"/>
  <rect width="${W}" height="${H}" fill="url(#glow)"/>
  <path d="M0 630 L0 540 L140 470 L230 510 L360 420 L470 500 L560 455 L700 540 L820 480 L960 560 L1080 500 L1200 545 L1200 630 Z" fill="${C.night}" opacity=".7"/>
  ${placeMark(markColor('o'), 70, 135, 360)}
  <path d="${title.d}" fill="${C.snow}"/>
  <rect x="476" y="310" width="60" height="5" rx="2.5" fill="${C.volcano}"/>
  <path d="${sub.d}" fill="${C.volcano}"/>
  <path d="${tag.d}" fill="${C.mist}"/>`, 'Titan ATLAS — Misión Domuyo');
}
files['og-image.svg'] = ogImage();

// ---------------------------------------------------------------- escritura + PNG
(async () => {
  for (const [name, content] of Object.entries(files)) {
    fs.writeFileSync(path.join(OUT, name), content);
  }

  const png = (src, out, width) =>
    sharp(Buffer.from(files[src]), { density: 300 }).resize({ width }).png({ compressionLevel: 9 }).toFile(path.join(OUT, out));

  await Promise.all([
    png('isotipo.svg', 'isotipo-1024.png', 1024),
    png('isotipo-blanco.svg', 'isotipo-blanco-1024.png', 1024),
    png('isotipo-negro.svg', 'isotipo-negro-1024.png', 1024),
    png('app-icon.svg', 'app-icon-512.png', 512),
    png('app-icon.svg', 'app-icon-192.png', 192),
    png('app-icon.svg', 'apple-touch-icon.png', 180),
    png('app-icon.svg', 'favicon-32.png', 32),
    png('app-icon.svg', 'favicon-16.png', 16),
    png('logo-horizontal-oscuro.svg', 'logo-horizontal-oscuro.png', 2000),
    png('logo-horizontal-claro.svg', 'logo-horizontal-claro.png', 2000),
    png('logo-horizontal-blanco.svg', 'logo-horizontal-blanco.png', 2000),
    png('logo-horizontal-negro.svg', 'logo-horizontal-negro.png', 2000),
    png('logo-vertical-oscuro.svg', 'logo-vertical-oscuro.png', 1520),
    png('logo-vertical-claro.svg', 'logo-vertical-claro.png', 1520),
    png('parche-mision.svg', 'parche-mision-2048.png', 2048),
    png('og-image.svg', 'og-image.png', 1200),
  ]);

  fs.copyFileSync(path.join(OUT, 'app-icon.svg'), path.join(OUT, '..', '..', 'favicon.svg'));
  console.log(`✔ Identidad generada en ${path.relative(process.cwd(), OUT)} (${fs.readdirSync(OUT).length} archivos)`);
})();
