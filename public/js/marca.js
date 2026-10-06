(() => {
  'use strict';
  const B = '/assets/brand/';

  const ASSETS = [
    { name: 'Logo horizontal · oscuro', desc: 'Para fondos oscuros', file: 'logo-horizontal-oscuro', stage: 'dark' },
    { name: 'Logo horizontal · claro', desc: 'Para fondos claros', file: 'logo-horizontal-claro', stage: 'light' },
    { name: 'Logo vertical · oscuro', desc: 'Portadas, banners verticales', file: 'logo-vertical-oscuro', stage: 'dark' },
    { name: 'Logo vertical · claro', desc: 'Documentos, informes', file: 'logo-vertical-claro', stage: 'light' },
    { name: 'Isotipo', desc: 'Color, fondo transparente', file: 'isotipo', png: 'isotipo-1024', stage: 'check' },
    { name: 'Parche de misión', desc: 'Remeras, stickers, bordado', file: 'parche-mision', png: 'parche-mision-2048', stage: 'dark' },
    { name: 'Ícono de app', desc: 'Favicon, redes, perfil', file: 'app-icon', png: 'app-icon-512', stage: 'light' },
    { name: 'Monocromo blanco', desc: 'Sobre fotos o grabado', file: 'logo-horizontal-blanco', stage: 'dark' },
    { name: 'Monocromo negro', desc: 'Impresión a una tinta, láser', file: 'logo-horizontal-negro', stage: 'light' },
    { name: 'Isotipo blanco', desc: 'Una tinta', file: 'isotipo-blanco', png: 'isotipo-blanco-1024', stage: 'dark' },
    { name: 'Isotipo negro', desc: 'Una tinta', file: 'isotipo-negro', png: 'isotipo-negro-1024', stage: 'light' },
    { name: 'Imagen para redes', desc: 'Open Graph 1200×630', file: 'og-image', png: 'og-image', stage: 'dark' },
  ];

  const COLORS = [
    { name: 'Navy ATLAS', hex: '#0A1120', use: 'Fondo principal' },
    { name: 'Panel', hex: '#101A2E', use: 'Tarjetas y superficies' },
    { name: 'Naranja ATLAS', hex: '#FF6B35', use: 'Acento de marca, señal' },
    { name: 'Ámbar', hex: '#FF9F43', use: 'Acento secundario' },
    { name: 'Celeste', hex: '#38BDF8', use: 'Órbita, enlaces, humedad' },
    { name: 'Verde', hex: '#34D399', use: 'Altitud, estados correctos' },
    { name: 'Violeta', hex: '#A78BFA', use: 'Presión, servidor' },
    { name: 'Amarillo', hex: '#FACC15', use: 'Batería, ESP32' },
    { name: 'Blanco', hex: '#F8FAFC', use: 'Texto sobre oscuro' },
    { name: 'Azul acero', hex: '#4A6FA0', use: 'Isotipo, ladera iluminada' },
  ];

  const esc = (s) => String(s).replace(/[&<>"]/g, (c) => ({ '&': '&amp;', '<': '&lt;', '>': '&gt;', '"': '&quot;' }[c]));

  document.getElementById('assets').innerHTML = ASSETS.map((a) => `
    <article class="panel asset">
      <div class="stage ${a.stage}"><img src="${B}${a.file}.svg" alt="${esc(a.name)}" loading="lazy"></div>
      <div class="meta">
        <div><b>${esc(a.name)}</b><small>${esc(a.desc)}</small></div>
        <div class="dl">
          <a href="${B}${a.file}.svg" download>SVG</a>
          <a href="${B}${a.png || a.file}.png" download>PNG</a>
        </div>
      </div>
    </article>`).join('');

  const rgb = (h) => [1, 3, 5].map((i) => parseInt(h.slice(i, i + 2), 16)).join(', ');
  document.getElementById('colors').innerHTML = COLORS.map((c) => `
    <tr data-hex="${c.hex}" title="Copiar ${c.hex}">
      <td><span class="sw" style="background:${c.hex}"></span></td>
      <td>${esc(c.name)}</td><td class="mono">${c.hex}</td><td class="mono">${rgb(c.hex)}</td><td>${esc(c.use)}</td>
    </tr>`).join('');

  const toast = (msg) => {
    const t = document.getElementById('toast');
    t.textContent = msg;
    t.classList.add('show');
    clearTimeout(toast._t);
    toast._t = setTimeout(() => t.classList.remove('show'), 2000);
  };

  document.getElementById('colors').addEventListener('click', async (e) => {
    const btn = e.target.closest('tr[data-hex]');
    if (!btn) return;
    try {
      await navigator.clipboard.writeText(btn.dataset.hex);
      toast(`${btn.dataset.hex} copiado`);
    } catch {
      toast(btn.dataset.hex);
    }
  });

  document.getElementById('year').textContent = new Date().getFullYear();
})();
