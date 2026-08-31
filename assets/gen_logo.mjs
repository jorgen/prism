// Generates the prism logo. Deterministic: there is no random number generator
// here, so regeneration is byte-identical.
// Usage: node gen_logo.mjs <outdir>
//
// THE MARK. One stream of requests arrives, and leaves as a spectrum of routes.
// That is not a metaphor invented for the logo -- it is the sentence in prism's
// own README, and the reason the project is called prism at all.
//
// It is drawn in the same packet cells vio's mark is made of, because prism is
// built on vio and the thing being dispersed is vio's stream. The incoming beam
// is vio's violet, and the middle route -- the one with zero deviation, which in
// a real prism is the ray that passes closest to undeflected -- stays exactly
// #7c3aed, vio's own colour, and carries straight on. The four either side bend
// away from it into rose and cyan.
//
// WHAT IS DELIBERATELY NOT DRAWN: the prism. A glass triangle with a rainbow
// coming out of it is Dark Side of the Moon, one of the most recognisable images
// there is, and no amount of restyling would make it this project's. So the mark
// is the beam and the dispersion, with a gap where the prism would be -- the
// thing that did the work is the absence in the middle. It also fixes a
// legibility problem, which is how you can tell it was the right call: without
// the gap the beam and the first routes run together and the mark reads as one
// blob rather than as before and after.

const outdir = process.argv[2] ?? '.';
import { writeFileSync } from 'node:fs';
import { join } from 'node:path';

const lerp = (a, b, t) => a + (b - a) * t;
function hex(r, g, b) {
  return '#' + [r, g, b].map((v) => Math.round(Math.max(0, Math.min(255, v))).toString(16).padStart(2, '0')).join('');
}
const mix = (a, b, t) => hex(lerp(a[0], b[0], t), lerp(a[1], b[1], t), lerp(a[2], b[2], t));

const BEAM_DEEP = [67, 26, 133];
const VIOLET = [124, 58, 237]; // vio's violet, and the undeviated route

// A spectrum centred on violet rather than a full rainbow: what arrives is
// violet, so what it disperses into is a spread either side of it. A red-to-
// green rainbow would say "colourful" and stop saying "this came from vio".
const ROUTES = [
  [244, 114, 182], // rose
  [168, 85, 247], // purple
  VIOLET, // straight on
  [79, 70, 229], // indigo
  [34, 211, 238], // cyan
];

const PITCH = 0.135; // step along a route
const CELL = 0.118;
const BEAM_N = 3; // cells before the dispersion
const ROUTE_N = 4; // cells along each route
const SPLIT = 0.70; // extra pitch across the gap, in place of the prism
const EASE = 0.55; // how the routes walk apart: < 1 spreads early
const CLEAR = 0.90; // of a cell, between neighbouring routes at their first step
const TILT = 27; // degrees, so the mark runs on the square's diagonal

// The routes block up into a coloured grid unless neighbours are already a clear
// cell apart at their FIRST step; past that the divergence keeps them apart on
// its own. So the spread is derived from that one constraint rather than chosen,
// and changing the route count or the easing cannot silently reintroduce it.
const SPREAD = (CELL / CLEAR) / Math.pow(1 / ROUTE_N, EASE);

function makeCells() {
  const cells = [];
  const x0 = 0.05;
  for (let i = 0; i < BEAM_N; i++) {
    cells.push({ x: x0 + i * PITCH, y: 0.5, c: mix(BEAM_DEEP, VIOLET, BEAM_N === 1 ? 1 : i / (BEAM_N - 1)) });
  }
  const xs = x0 + (BEAM_N - 1) * PITCH + PITCH * (1 + SPLIT);
  for (let r = 0; r < ROUTES.length; r++) {
    const lane = r - (ROUTES.length - 1) / 2;
    for (let k = 0; k < ROUTE_N; k++) {
      const t = (k + 1) / ROUTE_N;
      cells.push({
        x: xs + k * PITCH,
        y: 0.5 + lane * SPREAD * Math.pow(t, EASE),
        c: mix(VIOLET, ROUTES[r], Math.min(1, t * 1.15)),
      });
    }
  }
  const a = (TILT * Math.PI) / 180, ca = Math.cos(a), sa = Math.sin(a);
  for (const c of cells) {
    const dx = c.x - x0, dy = c.y - 0.5;
    c.x = x0 + dx * ca - dy * sa;
    c.y = 0.5 + dx * sa + dy * ca;
  }
  return cells;
}

// Fitted to the padded square from the drawn bounding box, so the geometry above
// can be tuned without anyone having to re-centre it by hand.
function markSvg(cells, size, pad) {
  const s = size - 2 * pad;
  const xs = cells.map((c) => c.x), ys = cells.map((c) => c.y);
  const minx = Math.min(...xs) - CELL / 2, maxx = Math.max(...xs) + CELL / 2;
  const miny = Math.min(...ys) - CELL / 2, maxy = Math.max(...ys) + CELL / 2;
  const scale = Math.min(1 / (maxx - minx), 1 / (maxy - miny));
  const ox = (1 - (maxx - minx) * scale) / 2 - minx * scale;
  const oy = (1 - (maxy - miny) * scale) / 2 - miny * scale;
  return cells
    .map((d) => {
      const w = CELL * scale * s;
      const x = pad + (d.x * scale + ox) * s - w / 2;
      const y = pad + (d.y * scale + oy) * s - w / 2;
      return `<rect x="${x.toFixed(2)}" y="${y.toFixed(2)}" width="${w.toFixed(2)}" height="${w.toFixed(2)}" rx="${(w * 0.30).toFixed(2)}" fill="${d.c}"/>`;
    })
    .join('\n    ');
}

const cells = makeCells();

const ICON = 512, PAD = 34;
const icon = `<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 ${ICON} ${ICON}" width="${ICON}" height="${ICON}">
  <title>prism</title>
  <g>
    ${markSvg(cells, ICON, PAD)}
  </g>
</svg>
`;
writeFileSync(join(outdir, 'logo.svg'), icon);

// The wordmark disperses too: one gradient across the five letters, from the
// violet that arrived to the cyan it leaves as. dewfall and vio both split their
// wordmark into two flat tones; this is the one project where a gradient is the
// subject rather than a decoration.
const BH = 240, BW = 720, DS = 212, DPAD = 4;
const banner = `<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 ${BW} ${BH}" width="${BW}" height="${BH}">
  <title>prism — requests, resolved</title>
  <defs>
    <linearGradient id="wordmark" x1="0" y1="0" x2="1" y2="0">
      <stop offset="0" stop-color="#4c1d95"/>
      <stop offset="0.42" stop-color="#7c3aed"/>
      <stop offset="1" stop-color="#22d3ee"/>
    </linearGradient>
  </defs>
  <g transform="translate(24, 14)">
    ${markSvg(cells, DS, DPAD)}
  </g>
  <g font-family="ui-sans-serif, system-ui, -apple-system, 'Segoe UI', sans-serif">
    <text x="266" y="135" font-size="100" font-weight="650" letter-spacing="-2" fill="url(#wordmark)">prism</text>
    <text x="270" y="184" font-size="28" fill="#64748b">requests, resolved.</text>
  </g>
</svg>
`;
writeFileSync(join(outdir, 'banner.svg'), banner);
console.log(`wrote logo.svg (${cells.length} cells) and banner.svg to ${outdir}`);
