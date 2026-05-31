let current = null;
const $ = (id) => document.getElementById(id);
const esc = (s) =>
  String(s ?? "").replace(
    /[&<>"']/g,
    (m) =>
      ({ "&": "&amp;", "<": "&lt;", ">": "&gt;", '"': "&quot;", "'": "&#39;" })[
        m
      ],
  );
const jsArg = (s) =>
  String(s ?? "")
    .replace(/\\/g, "\\\\")
    .replace(/'/g, "\\'");
const routeHash = (name, symbol = null) => {
  const base = encodeURIComponent(String(name ?? "").trim());
  const sym = symbol == null ? "" : String(symbol).trim();
  return "#" + base + (sym ? "::" + encodeURIComponent(sym) : "");
};
function manualRootUrl() {
  const url = new URL(window.location.href);
  url.hash = "";
  url.search = "";
  if (/\/index\.html?$/i.test(url.pathname))
    url.pathname = url.pathname.replace(/\/index\.html?$/i, "/");
  return url.href;
}
function sameUrl(a, b) {
  try {
    return (
      new URL(a, window.location.href).href ===
      new URL(b, window.location.href).href
    );
  } catch {
    return String(a || "") === String(b || "");
  }
}
function writeRoute(url, options = {}) {
  if (options.preserveHash) return;
  const target = new URL(url, window.location.href).href;
  if (sameUrl(target, window.location.href)) return;
  const mode = options.replaceRoute ? "replaceState" : "pushState";
  try {
    history[mode](null, "", target);
  } catch {
    window.location.href = target;
  }
}
function goHome(event = null) {
  if (event) event.preventDefault();
  selectModule("Overview", null, { preserveHash: true });
  writeRoute(manualRootUrl());
}
const decodeRouteHash = (s) => {
  try {
    return decodeURIComponent(s);
  } catch {
    return s;
  }
};
const slugifyHeading = (s) =>
  String(s ?? "")
    .toLowerCase()
    .replace(/`/g, "")
    .replace(/[^a-z0-9_.-]+/g, "-")
    .replace(/^-+|-+$/g, "") || "section";
const cleanHTML = (html) =>
  DOMPurify.sanitize(String(html ?? ""), {
    USE_PROFILES: { html: true, svg: true },
    ADD_TAGS: ["svg", "path", "circle", "ellipse", "rect", "line", "polyline"],
    ADD_ATTR: [
      "aria-expanded",
      "aria-hidden",
      "aria-pressed",
      "class",
      "cx",
      "cy",
      "d",
      "data-code-action",
      "data-code-label",
      "data-code-toggle",
      "data-copy",
      "data-copy-source",
      "data-doc-anchor",
      "data-doc-route",
      "data-internal-module",
      "data-internal-toggle",
      "data-module-source",
      "data-route-copy",
      "data-select-module",
      "data-select-symbol",
      "data-select-tag",
      "data-source-module",
      "fill",
      "height",
      "href",
      "r",
      "rel",
      "role",
      "rx",
      "stroke",
      "stroke-linecap",
      "stroke-linejoin",
      "stroke-width",
      "target",
      "viewBox",
      "width",
      "x",
      "x1",
      "x2",
      "y",
      "y1",
      "y2",
    ],
  });
const setHTML = (id, html) => {
  const el = $(id);
  if (el) el.innerHTML = cleanHTML(html);
};
const setPageTitle = (title) => {
  const page = String(title || "Manual").trim() || "Manual";
  document.title = `Nytrix - ${page}`;
};
let apiModuleList = [];
let overviewModule = null;
let navTreeCache = {};
let markdownDocsCache = [];
let docTreeCache = null;
let searchTimer = null;
let navHideTimer = null;
let categoryModulesByPath = new Map();
const INTERNAL_SYMBOLS_STORAGE_KEY = "nytrix.docs.showInternals";
let showInternalSymbols = (() => {
  try {
    return localStorage.getItem(INTERNAL_SYMBOLS_STORAGE_KEY) === "1";
  } catch {
    return false;
  }
})();
const apiModules = () => apiModuleList;
const countApiModules = () => apiModules().length;
const countAllSyms = () =>
  apiModules().reduce((n, m) => n + (m.symbols ? m.symbols.length : 0), 0);
const moduleByName = new Map();
const markdownDocByName = new Map();
const categorySet = new Set();
const tagIndex = new Map();
let tagEntries = [];
let searchRows = [];
const moduleExists = (name) => moduleByName.has(name);
const docTitle = (doc) => esc((doc && (doc.title || doc.name)) || "");
const titleCaseSegment = (segment) =>
  String(segment ?? "")
    .replace(/[-_]+/g, " ")
    .replace(/\b\w/g, (ch) => ch.toUpperCase());
const docSegmentNavTitle = (segment) =>
  esc(titleCaseSegment(segment).toUpperCase());
const docLeafTitle = (doc) => {
  const title = String((doc && (doc.title || doc.name)) || "");
  return esc(title.split(" / ").pop().split("/").pop());
};
const MARKDOWN_DOC_PRIORITY = [
  "README",
  "learn/start",
  "learn/programs",
  "learn/repl",
  "learn/examples",
  "learn/ui",
  "learn/library",
  "learn/tooling",
  "learn/networking",
  "learn/packages",
  "learn/performance",
  "learn/metaprogramming",
  "learn/native",
  "learn/diagnostics",
  "learn/testing",
  "learn/troubleshooting",
  "spec/language",
  "spec/source",
  "spec/imports",
  "spec/modules",
  "spec/values",
  "spec/functions",
  "spec/types",
  "spec/operators",
  "spec/patterns",
  "spec/control-flow",
  "spec/errors",
  "spec/comptime",
  "spec/native",
  "spec/runtime",
  "spec/syntax",
  "CHANGELOG",
  "NY",
  "NYTRIX",
];
const DOC_CARD_SUMMARIES = new Map(
  Object.entries({
    README: "Docs map and command index.",
    CHANGELOG: "Release notes and maintenance checks.",
    "learn/start": "Build, run, and check one file.",
    "learn/programs": "Scripts, modules, imports, entrypoints.",
    "learn/repl": "Interactive probes and completions.",
    "learn/examples": "Small runnable Nytrix programs.",
    "learn/ui": "Windows, input, rendering, text, assets.",
    "learn/library": "Stdlib map and API lookup.",
    "learn/tooling": "Format, test, docs, packages, builds.",
    "learn/networking": "HTTP, sockets, servers, TLS, processes.",
    "learn/packages": "Manifests, dependencies, installs, lockfiles.",
    "learn/performance": "Compile-time and runtime measurement.",
    "learn/metaprogramming": "Comptime tables, templates, generated code.",
    "learn/native": "C ABI, layouts, pointers, handles.",
    "learn/diagnostics": "Import, type, runtime, and FFI failures.",
    "learn/testing": "Executable assertions and test runs.",
    "learn/troubleshooting": "Common failures and quick fixes.",
    "spec/language": "Specification index and execution model.",
    "spec/source": "Source units, files, and execution.",
    "spec/imports": "Module, file, package, and alias imports.",
    "spec/modules": "Exports and public module boundaries.",
    "spec/values": "Literals, strings, containers, equality.",
    "spec/functions": "Parameters, returns, lambdas, attributes.",
    "spec/types": "Typed values, generics, ADTs, strict checks.",
    "spec/operators": "Arithmetic, calls, indexing, member access.",
    "spec/patterns": "Case and match dispatch forms.",
    "spec/control-flow": "Branches, loops, cleanup, error flow.",
    "spec/errors": "Assertions, panics, results, diagnostics.",
    "spec/comptime": "Compile-time code and generated declarations.",
    "spec/native": "Layouts, externs, pointers, ABI rules.",
    "spec/runtime": "Execution modes, memory, ownership, async.",
    "spec/syntax": "Source spellings and grammar forms.",
  }),
);
const API_NAMESPACE_SUMMARIES = new Map(
  Object.entries({
    "std.core":
      "Core values, collections, strings, assertions, reflection, terminal helpers, queues, and channels.",
    "std.math":
      "Numbers, vectors, matrices, finite fields, number theory, and analysis helpers.",
    "std.math.crypto":
      "Encodings, hashes, symmetric ciphers, RSA/ECC helpers, number theory, and analysis utilities.",
    "std.math.crypto.factorization":
      "Fermat, Pollard, ECM, primality, and known-prime helpers.",
    "std.math.crypto.lattice":
      "LLL, CVP, BKZ, basis matrices, and reduction reports.",
    "std.os":
      "Files, paths, processes, environment, time, threads, async work, networking, UI, and native boundaries.",
    "std.os.net":
      "HTTP clients, HTTP servers, sockets, TLS transport, tubes, transcripts, and remote process helpers.",
    "std.os.sound":
      "Audio devices, formats, playback helpers, and sound-facing native integration.",
    "std.os.ui":
      "Windows, input, rendering, terminal UI, scenes, and platform backends.",
    "std.parse":
      "Structured data, source syntax, images, fonts, compressed data, and 3D asset parsing.",
    "std.parse.data":
      "JSON, YAML, TOML, CSV, XML, SQL, zlib, and related data-format helpers.",
    "std.parse.img":
      "Image codecs, metadata, loading, saving, and pixel format utilities.",
    "std.parse.syntax":
      "Tokenizers and highlighters for Nytrix and common source formats.",
    "std.parse.3d":
      "3D asset loading, glTF scene inspection, meshes, materials, and asset workflow helpers.",
  }),
);
const SEARCH_LIMIT = 120;
const sortMarkdownDocs = (docs) =>
  docs.slice().sort((docA, docB) => {
    const a = docA.name,
      b = docB.name,
      ia = MARKDOWN_DOC_PRIORITY.indexOf(a),
      ib = MARKDOWN_DOC_PRIORITY.indexOf(b);
    if (ia !== -1 && ib !== -1) return ia - ib;
    if (ia !== -1) return -1;
    if (ib !== -1) return 1;
    return a.localeCompare(b);
  });
const stripDocText = (text) =>
  String(text ?? "")
    .replace(/```[\s\S]*?```/g, " ")
    .replace(/`([^`]+)`/g, "$1")
    .replace(/!\[[^\]]*]\([^)]*\)/g, " ")
    .replace(/\[[^\]]*]\([^)]*\)/g, (match) =>
      match.replace(/^\[|\]\([^)]*\)$/g, ""),
    )
    .replace(/^\s*[-*+]\s+\[[ xX]\]\s+/gm, "")
    .replace(/^\s*(?:[-*+]|\d+[.)])\s+/gm, "")
    .replace(/[#>*_~|]/g, " ")
    .replace(/\s+/g, " ")
    .trim();
const markdownDocSummary = (doc) => {
  const raw = String((doc && doc.html) || "")
    .replace(/\r\n?/g, "\n")
    .replace(/```[\s\S]*?```/g, "\n");
  const paragraphs = [];
  const listFallbacks = [];
  let current = [];
  let skipListContinuation = false;
  const flush = () => {
    if (!current.length) return;
    paragraphs.push(current.join(" "));
    current = [];
  };
  raw.split("\n").forEach((line) => {
    const trimmed = line.trim();
    const isNoise =
      !trimmed ||
      /^#{1,6}\s+/.test(trimmed) ||
      /^[-*_]{3,}$/.test(trimmed) ||
      /^\|/.test(trimmed) ||
      /^[-:| ]+$/.test(trimmed);
    if (isNoise) {
      flush();
      skipListContinuation = false;
      return;
    }
    if (skipListContinuation && /^\s{2,}\S/.test(line)) return;
    skipListContinuation = false;
    if (/^[-*+]\s+\[[ xX]\]\s+/.test(trimmed)) {
      flush();
      skipListContinuation = true;
      return;
    }
    const list = trimmed.match(/^(?:[-*+]|\d+[.)])\s+(.+)$/);
    if (list) {
      flush();
      const cleanList = stripDocText(list[1]);
      if (cleanList && !/^todo\b/i.test(cleanList))
        listFallbacks.push(cleanList);
      skipListContinuation = true;
      return;
    }
    current.push(trimmed);
  });
  flush();
  const text =
    paragraphs
      .map(stripDocText)
      .find((s) => /[A-Za-z]/.test(s) && s.length >= 24) ||
    listFallbacks.find((s) => /[A-Za-z]/.test(s) && s.length >= 24) ||
    stripDocText(paragraphs[0] || listFallbacks[0] || "");
  if (!text) return `${markdownDocLabel(doc)} manual page.`;
  return text.length > 170 ? `${text.slice(0, 167).trim()}...` : text;
};
const markdownDocCardSummary = (doc) => {
  const route = String((doc && doc.name) || "");
  const fixed = DOC_CARD_SUMMARIES.get(route);
  if (fixed) return fixed;
  let text = markdownDocSummary(doc);
  const sentence = text.match(/^(.{24,92}?[.!?])(?:\s|$)/);
  if (sentence) text = sentence[1];
  return text.length > 96 ? `${text.slice(0, 93).trim()}...` : text;
};
const markdownDocLabel = (doc) =>
  String((doc && (doc.title || doc.name)) || "")
    .split(" / ")
    .pop();
const ICONS = {
  alert: `<path d="M12 9v4"/><path d="M12 17h.01"/><path d="M10.3 3.9 1.8 18a2 2 0 0 0 1.7 3h17a2 2 0 0 0 1.7-3L13.7 3.9a2 2 0 0 0-3.4 0Z"/>`,
  atom: `<circle cx="12" cy="12" r="1.4"/><path d="M20.2 12c0 2-3.7 3.7-8.2 3.7S3.8 14 3.8 12 7.5 8.3 12 8.3s8.2 1.7 8.2 3.7Z"/><path d="M16.1 19.1c-1.7 1-4.9-1.5-7.1-5.4S6.3 5.9 8 4.9s4.9 1.5 7.1 5.4 2.7 7.8 1 8.8Z"/><path d="M7.9 19.1c-1.7-1-1.2-4.9 1-8.8s5.4-6.4 7.1-5.4 1.2 4.9-1 8.8-5.4 6.4-7.1 5.4Z"/>`,
  book: `<path d="M12 7v14"/><path d="M3 5a7 7 0 0 1 9 2v14a7 7 0 0 0-9-2Z"/><path d="M21 5a7 7 0 0 0-9 2v14a7 7 0 0 1 9-2Z"/>`,
  box: `<path d="m21 8-9-5-9 5 9 5Z"/><path d="M3 8v8l9 5 9-5V8"/><path d="M12 13v8"/>`,
  check: `<path d="M20 6 9 17l-5-5"/><circle cx="12" cy="12" r="10"/>`,
  chevron: `<path d="m9 18 6-6-6-6"/>`,
  cloud: `<path d="M17.5 19H7a5 5 0 1 1 1.1-9.9 6 6 0 0 1 11.4 2.5A3.8 3.8 0 0 1 17.5 19Z"/>`,
  code: `<path d="m16 18 6-6-6-6"/><path d="m8 6-6 6 6 6"/><path d="m14 4-4 16"/>`,
  copy: `<rect x="9" y="9" width="11" height="11" rx="2"/><path d="M5 15H4a2 2 0 0 1-2-2V4a2 2 0 0 1 2-2h9a2 2 0 0 1 2 2v1"/>`,
  cpu: `<rect x="5" y="5" width="14" height="14" rx="2"/><path d="M9 1v4"/><path d="M15 1v4"/><path d="M9 19v4"/><path d="M15 19v4"/><path d="M1 9h4"/><path d="M1 15h4"/><path d="M19 9h4"/><path d="M19 15h4"/><rect x="9" y="9" width="6" height="6" rx="1"/>`,
  database: `<ellipse cx="12" cy="5" rx="8" ry="3"/><path d="M4 5v14c0 1.7 3.6 3 8 3s8-1.3 8-3V5"/><path d="M4 12c0 1.7 3.6 3 8 3s8-1.3 8-3"/>`,
  dice: `<rect x="4" y="4" width="16" height="16" rx="3"/><circle cx="8.5" cy="8.5" r=".7"/><circle cx="15.5" cy="8.5" r=".7"/><circle cx="12" cy="12" r=".7"/><circle cx="8.5" cy="15.5" r=".7"/><circle cx="15.5" cy="15.5" r=".7"/>`,
  file: `<path d="M14 2H6a2 2 0 0 0-2 2v16a2 2 0 0 0 2 2h12a2 2 0 0 0 2-2V8Z"/><path d="M14 2v6h6"/><path d="M8 13h8"/><path d="M8 17h6"/>`,
  file3d: `<path d="M14 2H6a2 2 0 0 0-2 2v16a2 2 0 0 0 2 2h12a2 2 0 0 0 2-2V8Z"/><path d="M14 2v6h6"/><path d="m9 13 3-1.7 3 1.7v3.4L12 18l-3-1.6Z"/><path d="M12 14.7V18"/>`,
  folder: `<path d="M3 7a2 2 0 0 1 2-2h5l2 2h7a2 2 0 0 1 2 2v8a2 2 0 0 1-2 2H5a2 2 0 0 1-2-2Z"/>`,
  github: `<path d="M15 22v-3.9a4.7 4.7 0 0 0-1.2-3.6c4 0 6.7-1.9 6.7-5.4 0-1.2-.4-2.3-1.1-3.2.1-.4.5-2-.2-3.3 0 0-1-.3-3.3 1.2a11.4 11.4 0 0 0-5.8 0C7.8 2.3 6.8 2.6 6.8 2.6c-.7 1.3-.3 2.9-.2 3.3A5 5 0 0 0 5.5 9.1c0 3.5 2.7 5.4 6.7 5.4a4.7 4.7 0 0 0-1.2 3.6V22"/><path d="M10.8 18.2c-3.2 1-4.5-.8-5.2-2.1-.4-.8-1.2-1.4-2.1-1.4"/>`,
  home: `<path d="m3 11 9-8 9 8"/><path d="M5 10v10h14V10"/><path d="M9 20v-6h6v6"/>`,
  image: `<rect x="3" y="5" width="18" height="14" rx="2"/><circle cx="8" cy="10" r="1.4"/><path d="m21 15-4.5-4.5L7 19"/>`,
  key: `<circle cx="7.5" cy="14.5" r="3.5"/><path d="M10 12 21 1"/><path d="m16 6 2 2"/><path d="m14 8 2 2"/>`,
  layers: `<path d="m12 2 10 6-10 6L2 8Z"/><path d="m2 14 10 6 10-6"/><path d="m2 10 10 6 10-6"/>`,
  library: `<path d="M4 19.5A2.5 2.5 0 0 1 6.5 17H20"/><path d="M4 4.5A2.5 2.5 0 0 1 6.5 2H20v20H6.5A2.5 2.5 0 0 1 4 19.5Z"/><path d="M8 7h8"/><path d="M8 11h6"/>`,
  list: `<path d="M8 6h13"/><path d="M8 12h13"/><path d="M8 18h13"/><path d="M3 6h.01"/><path d="M3 12h.01"/><path d="M3 18h.01"/>`,
  lock: `<rect x="4" y="11" width="16" height="10" rx="2"/><path d="M8 11V8a4 4 0 0 1 8 0v3"/><path d="M12 15v2"/>`,
  hash: `<path d="M4 9h16"/><path d="M4 15h16"/><path d="M10 3 8 21"/><path d="m16 3-2 18"/>`,
  math: `<path d="M3 4h18"/><path d="M6 8l4 4-4 4"/><path d="M14 16h5"/><path d="M14 12h5"/>`,
  network: `<rect x="16" y="16" width="6" height="6" rx="1"/><rect x="2" y="16" width="6" height="6" rx="1"/><rect x="9" y="2" width="6" height="6" rx="1"/><path d="M5 16v-3a2 2 0 0 1 2-2h10a2 2 0 0 1 2 2v3"/><path d="M12 8v8"/>`,
  package: `<path d="m16.5 9.4-9-5.2"/><path d="M21 16V8a2 2 0 0 0-1-1.7l-7-4a2 2 0 0 0-2 0l-7 4A2 2 0 0 0 3 8v8a2 2 0 0 0 1 1.7l7 4a2 2 0 0 0 2 0l7-4a2 2 0 0 0 1-1.7Z"/><path d="m3.3 7 8.7 5 8.7-5"/><path d="M12 22V12"/>`,
  play: `<circle cx="12" cy="12" r="10"/><path d="m10 8 6 4-6 4Z"/>`,
  route: `<circle cx="6" cy="19" r="3"/><circle cx="18" cy="5" r="3"/><path d="M6 16v-4a4 4 0 0 1 4-4h4"/>`,
  search: `<circle cx="11" cy="11" r="7"/><path d="m21 21-4.3-4.3"/>`,
  server: `<rect x="3" y="4" width="18" height="6" rx="2"/><rect x="3" y="14" width="18" height="6" rx="2"/><path d="M7 7h.01"/><path d="M7 17h.01"/>`,
  shield: `<path d="M12 22s8-4 8-10V5l-8-3-8 3v7c0 6 8 10 8 10Z"/><path d="M9 12l2 2 4-4"/>`,
  sound: `<path d="M4 10v4h4l5 5V5L8 10Z"/><path d="M16 9a4 4 0 0 1 0 6"/><path d="M18.5 6.5a8 8 0 0 1 0 11"/>`,
  tag: `<path d="M20.6 13.4 13.4 20.6a2 2 0 0 1-2.8 0L3 13V3h10l7.6 7.6a2 2 0 0 1 0 2.8Z"/><path d="M7.5 7.5h.01"/>`,
  terminal: `<path d="m4 17 6-6-6-6"/><path d="M12 19h8"/>`,
  timer: `<path d="M10 2h4"/><path d="M12 14v-4"/><path d="M12 22a8 8 0 1 0 0-16 8 8 0 0 0 0 16Z"/><path d="m17 7 1.5-1.5"/>`,
  window: `<rect x="3" y="4" width="18" height="16" rx="2"/><path d="M3 9h18"/><path d="M8 4v5"/>`,
  zap: `<path d="M13 2 3 14h8l-1 8 11-14h-8Z"/>`,
  wrench: `<path d="M14.7 6.3a5 5 0 0 0-6.4 6.4L2 19l3 3 6.3-6.3a5 5 0 0 0 6.4-6.4l-3.1 3.1-3-3Z"/>`,
  gear: `<circle cx="12" cy="12" r="3"/><path d="M19.4 15a1.65 1.65 0 0 0 .33 1.82l.06.06a2 2 0 0 1-2.83 2.83l-.06-.06a1.65 1.65 0 0 0-1.82-.33 1.65 1.65 0 0 0-1 1.51V21a2 2 0 0 1-4 0v-.09A1.65 1.65 0 0 0 9 19.4a1.65 1.65 0 0 0-1.82.33l-.06.06a2 2 0 0 1-2.83-2.83l.06-.06A1.65 1.65 0 0 0 4.68 15a1.65 1.65 0 0 0-1.51-1H3a2 2 0 0 1 0-4h.09A1.65 1.65 0 0 0 4.6 9a1.65 1.65 0 0 0-.33-1.82l-.06-.06a2 2 0 0 1 2.83-2.83l.06.06A1.65 1.65 0 0 0 9 4.68a1.65 1.65 0 0 0 1-1.51V3a2 2 0 0 1 4 0v.09a1.65 1.65 0 0 0 1 1.51 1.65 1.65 0 0 0 1.82-.33l.06-.06a2 2 0 0 1 2.83 2.83l-.06.06A1.65 1.65 0 0 0 19.4 9a1.65 1.65 0 0 0 1.51 1H21a2 2 0 0 1 0 4h-.09a1.65 1.65 0 0 0-1.51 1Z"/>`,
  globe: `<circle cx="12" cy="12" r="10"/><path d="M2 12h20"/><path d="M12 2a15.3 15.3 0 0 1 4 10 15.3 15.3 0 0 1-4 10 15.3 15.3 0 0 1-4-10 15.3 15.3 0 0 1 4-10Z"/>`,
  external: `<path d="M18 13v6a2 2 0 0 1-2 2H5a2 2 0 0 1-2-2V8a2 2 0 0 1 2-2h6"/><path d="M15 3h6v6"/><path d="m10 14 11-11"/>`,
  info: `<circle cx="12" cy="12" r="10"/><path d="M12 16v-4"/><path d="M12 8h.01"/>`,
  dots: `<circle cx="12" cy="12" r="1"/><circle cx="19" cy="12" r="1"/><circle cx="5" cy="12" r="1"/>`,
};

function icon(name, cls = "ico") {
  const safe =
    String(name || "box")
      .replace(/[^a-z0-9-]/gi, "")
      .toLowerCase() || "box";
  const body = ICONS[safe] || ICONS.box;
  return `<svg class="${cls} ico-${safe}" viewBox="0 0 24 24" aria-hidden="true" role="img" fill="none" stroke="currentColor" stroke-width="1.7" stroke-linecap="round" stroke-linejoin="round">${body}</svg>`;
}

const collapseIcon = () =>
  `<span class="collapse-icon">${icon("chevron", "ico ico-xs")}</span>`;

function docIconName(route) {
  const r = String(route || "");
  if (r === "README" || r === "Overview") return "book";
  if (r.includes("quick-reference")) return "list";
  if (r.includes("repl")) return "terminal";
  if (r.includes("editor")) return "window";
  if (r.includes("performance")) return "timer";
  if (r.includes("start")) return "play";
  if (r.includes("program")) return "file";
  if (r.includes("example")) return "code";
  if (r.includes("idiom")) return "layers";
  if (r.includes("data") || r.includes("value")) return "database";
  if (r.includes("library")) return "library";
  if (r.includes("network")) return "network";
  if (r.includes("package")) return "package";
  if (r.includes("metaprogram") || r.includes("comptime")) return "cpu";
  if (r.includes("native") || r.includes("runtime")) return "shield";
  if (
    r.includes("diagnostic") ||
    r.includes("troubleshoot") ||
    r.includes("error")
  )
    return "alert";
  if (r.includes("test")) return "check";
  if (r.includes("tooling") || r.includes("tool")) return "gear";
  if (r.includes("import")) return "package";
  if (r.includes("module")) return "layers";
  if (r.includes("pattern")) return "route";
  if (r.includes("syntax") || r.includes("function") || r.includes("control"))
    return "code";
  if (r.includes("release") || r.includes("CHANGELOG")) return "route";
  return "file";
}

function moduleIconName(route) {
  const r = String(route || "");
  if (r === "Home") return "home";
  if (r === "Tags" || r === "tag") return "tag";
  if (r.includes(".net.server") || r.includes(".http")) return "server";
  if (
    r.includes(".net.socket") ||
    r.includes(".net.remote") ||
    r.includes(".net.ssh")
  )
    return "network";
  if (r.includes(".net.curl") || r.includes(".net.requests")) return "globe";
  if (r.includes(".net") || r === "std.os.net") return "cloud";
  if (r.includes(".sound")) return "sound";
  if (r.includes(".ui") || r.includes(".window") || r.includes(".render"))
    return "window";
  if (r.includes(".gpu") || r.includes(".vulkan") || r.includes(".opencl"))
    return "zap";
  if (r.includes(".parse.3d") || r.includes(".gltf") || r.includes(".obj"))
    return "file3d";
  if (r.includes(".parse.img") || r.includes(".image") || r.includes(".font"))
    return "image";
  if (r.includes(".parse.syntax")) return "code";
  if (r.includes(".parse.data.sql")) return "database";
  if (r.includes(".parse.data") || r.includes(".data")) return "database";
  if (r.includes(".crypto.hash")) return "hash";
  if (
    r.includes(".crypto.rsa") ||
    r.includes(".crypto.ecc") ||
    r.includes(".public_key")
  )
    return "key";
  if (r.includes(".crypto.block") || r.includes(".crypto.cipher"))
    return "lock";
  if (r.includes(".crypto.prng") || r.includes(".random")) return "dice";
  if (r.includes(".crypto")) return "shield";
  if (r.includes(".thread") || r.includes(".async") || r.includes(".time"))
    return "timer";
  if (r.includes(".ffi") || r.includes(".disasm")) return "cpu";
  if (r.includes(".process") || r.includes(".io") || r.includes(".term"))
    return "terminal";
  if (r.includes(".os") || r === "std.os") return "terminal";
  if (r.includes(".lattice") || r.includes(".factor") || r.includes(".math"))
    return "math";
  if (r.includes(".parse")) return "file";
  if (r.includes(".pkg") || r.includes("package")) return "package";
  if (r.includes(".reflect") || r.includes(".inspect") || r.includes(".debug"))
    return "search";
  if (r.includes(".syntax")) return "code";
  if (r.includes(".tbuf") || r.includes(".buffer")) return "layers";
  if (r.includes(".test") || r.includes(".assert")) return "check";
  if (r.includes(".alloc") || r.includes(".cache")) return "cpu";
  return "box";
}

function pageIconName(title) {
  const t = String(title || "");
  if (t === "Tags" || t.startsWith("Tag /")) return "tag";
  if (t.includes("Search")) return "search";
  if (t.includes("Manual")) return "book";
  return docIconName(t.includes("/") ? t : t.toLowerCase());
}

function importHref(imp) {
  if (!imp) return routeHash("Overview");
  if (imp.symbol_target === "*" || moduleExists(imp.full_path))
    return routeHash(imp.full_path || imp.module_target);
  if (moduleExists(imp.module_target))
    return routeHash(imp.module_target, imp.symbol_target);
  return routeHash(imp.full_path || imp.module_target || "Overview");
}

function renderImports(imports) {
  if (!imports || !imports.length) return "";
  const groups = {};
  imports.forEach((imp) => {
    const mod = imp.module_target || "global";
    (groups[mod] || (groups[mod] = [])).push(imp);
  });
  let html = `<div class="imports"><div class="codehd"><span>IMPORTS</span></div><ul>`;
  Object.keys(groups)
    .sort()
    .forEach((mod) => {
      const symbolLinks = groups[mod]
        .sort((a, b) => a.symbol_target.localeCompare(b.symbol_target))
        .map((imp) => {
          const display = imp.alias
            ? `${imp.symbol_target} as ${imp.alias}`
            : imp.symbol_target;
          return `<a class="syn-call" href="${importHref(imp)}">${esc(display)}</a>`;
        })
        .join(", ");
      html +=
        mod === "global"
          ? `<li>${symbolLinks}</li>`
          : `<li><a class="syn-module" href="${routeHash(mod)}">${esc(mod)}</a>: ${symbolLinks}</li>`;
    });
  return html + `</ul></div>`;
}

const NY_KEYWORDS = new Set(
  "fn def mut if else elif while for return use import export module case break continue asm struct layout enum match type defer in as and or not try catch throw finally lambda comptime nil true false self".split(
    " ",
  ),
);
const NY_TYPES = new Set(
  "bool int i8 i16 i32 i64 u8 u16 u32 u64 f16 f32 f64 f128 str ptr void any list dict set tuple char byte size_t usize isize".split(
    " ",
  ),
);
const NY_LITERALS = new Set("nil true false".split(" "));
const NY_BUILTINS = new Set(
  "len print println assert assert_eq panic warn error get put append pop push slice contains keys values items type typeof repr to_str str_contains is_ptr is_dict is_list is_set is_tuple is_bool is_int is_float is_str int float bool str malloc realloc free zalloc load8 load16 load32 load64 store8 store16 store32 store64 load64_h call0 call1 call2 call3 call4 call5 env pid ticks msleep usleep".split(
    " ",
  ),
);
const NY_OPERATORS = new Set([
  "==",
  "!=",
  "<=",
  ">=",
  "&&",
  "||",
  "??",
  "?.",
  "|>",
  "->",
  "=>",
  "<<",
  ">>",
  "+=",
  "-=",
  "*=",
  "/=",
  "%=",
  "&=",
  "|=",
  "^=",
  "++",
  "--",
  "::",
  "...",
]);

function span(cls, text) {
  return `<span class="${cls}">${esc(text)}</span>`;
}

function classifyIdent(word, nextNonWs, prevNonWs) {
  if (NY_LITERALS.has(word)) return "syn-const";
  if (NY_KEYWORDS.has(word)) return "syn-kw";
  if (nextNonWs === ":") return "syn-type";
  if (/^[A-Z][A-Z0-9_]*$/.test(word)) return "syn-const";
  if (NY_TYPES.has(word) || /^[A-Z][A-Za-z0-9_]*$/.test(word))
    return "syn-type";
  if (word.startsWith("_") && word.toUpperCase() === word) return "syn-const";
  if (nextNonWs === "(")
    return NY_BUILTINS.has(word) ? "syn-builtin" : "syn-call";
  if (prevNonWs === "." || word.includes(".")) return "syn-module";
  if (NY_BUILTINS.has(word)) return "syn-builtin";
  return "syn-ident";
}

function highlight(code) {
  if (!code) return "";
  let out = "";
  let i = 0;
  const n = code.length;
  const nextNonWs = (p) => {
    while (p < n && /\s/.test(code[p])) p++;
    return code[p] || "";
  };
  const prevNonWs = (p) => {
    p--;
    while (p >= 0 && /\s/.test(code[p])) p--;
    return p >= 0 ? code[p] : "";
  };

  while (i < n) {
    const c = code[i];
    if (c === ";" || (c === "/" && code[i + 1] === "/")) {
      const start = i;
      while (i < n && code[i] !== "\n") i++;
      out += span("syn-com", code.slice(start, i));
      continue;
    }
    if (
      (c === "f" || c === "F") &&
      (code[i + 1] === '"' || code[i + 1] === "'")
    ) {
      const start = i++;
      const q = code[i++];
      while (i < n) {
        if (code[i] === "\\") {
          i += 2;
          continue;
        }
        if (code[i++] === q) break;
      }
      out += span("syn-str", code.slice(start, i));
      continue;
    }
    if (c === '"' || c === "'") {
      const q = c,
        start = i++;
      const triple = code[i] === q && code[i + 1] === q;
      if (triple) i += 2;
      while (i < n) {
        if (code[i] === "\\") {
          i += 2;
          continue;
        }
        if (triple && code[i] === q && code[i + 1] === q && code[i + 2] === q) {
          i += 3;
          break;
        }
        if (!triple && code[i++] === q) break;
        if (triple) i++;
      }
      out += span("syn-str", code.slice(start, i));
      continue;
    }
    if (c === "@" && /[A-Za-z_]/.test(code[i + 1] || "")) {
      const start = i++;
      while (i < n && /[A-Za-z0-9_]/.test(code[i])) i++;
      out += span("syn-attr", code.slice(start, i));
      continue;
    }
    if (/[0-9]/.test(c) || (c === "." && /[0-9]/.test(code[i + 1] || ""))) {
      const start = i;
      if (code[i] === "0" && /[xX]/.test(code[i + 1] || ""))
        ((i += 2),
          (() => {
            while (i < n && /[0-9A-Fa-f_]/.test(code[i])) i++;
          })());
      else if (code[i] === "0" && /[bB]/.test(code[i + 1] || ""))
        ((i += 2),
          (() => {
            while (i < n && /[01_]/.test(code[i])) i++;
          })());
      else if (code[i] === "0" && /[oO]/.test(code[i + 1] || ""))
        ((i += 2),
          (() => {
            while (i < n && /[0-7_]/.test(code[i])) i++;
          })());
      else {
        while (i < n && /[0-9_]/.test(code[i])) i++;
        if (code[i] === ".") {
          i++;
          while (i < n && /[0-9_]/.test(code[i])) i++;
        }
        if (/[eE]/.test(code[i] || "")) {
          i++;
          if (/[+-]/.test(code[i] || "")) i++;
          while (i < n && /[0-9_]/.test(code[i])) i++;
        }
      }
      while (i < n && /[A-Za-z0-9_]/.test(code[i])) i++;
      out += span("syn-num", code.slice(start, i));
      continue;
    }
    if (/[A-Za-z_]/.test(c)) {
      const start = i++;
      while (i < n && /[A-Za-z0-9_.]/.test(code[i])) i++;
      const word = code.slice(start, i);
      out += span(classifyIdent(word, nextNonWs(i), prevNonWs(start)), word);
      continue;
    }
    const three = code.slice(i, i + 3),
      two = code.slice(i, i + 2);
    if (NY_OPERATORS.has(three)) {
      out += span("syn-op", three);
      i += 3;
      continue;
    }
    if (NY_OPERATORS.has(two)) {
      out += span("syn-op", two);
      i += 2;
      continue;
    }
    if (/[+\-*\/%=&|^!~<>?:]/.test(c)) {
      out += span("syn-op", c);
      i++;
      continue;
    }
    if (/[()[\]{},.]/.test(c)) {
      out += span("syn-punc", c);
      i++;
      continue;
    }
    out += esc(c);
    i++;
  }
  return out;
}

function highlightModuleLabel(name) {
  return String(name ?? "")
    .split(".")
    .map((part) =>
      span(/^[A-Z][A-Za-z0-9_]*$/.test(part) ? "syn-type" : "syn-module", part),
    )
    .join(span("syn-punc", "."));
}

function highlightNavLabel(text, kind = "") {
  const s = String(text ?? "");
  if (kind === "module" || s.includes(".")) return highlightModuleLabel(s);
  if (
    kind === "function" ||
    kind === "extern" ||
    /^fn\s|^extern\s+fn\s/.test(s)
  )
    return highlight(s);
  if (
    kind === "struct" ||
    kind === "layout" ||
    kind === "enum" ||
    kind === "alias"
  )
    return highlight(s);
  if (kind === "constant") return span("syn-const", s);
  if (kind === "variable") return span("syn-var", s);
  return esc(s);
}

function highlightBash(code) {
  if (!code) return "";
  const rules = [
    { name: "com", regex: /(?:#[^\n]*)/ },
    { name: "str", regex: /(?:'(?:\\.|[^'])*'|"(?:\\.|[^"])*")/ },
    { name: "var", regex: /(?:\$\{[^}]+\}|\$[A-Za-z_][A-Za-z0-9_]*)/ },
    {
      name: "kw",
      regex:
        /\b(?:if|then|fi|elif|else|for|while|do|done|case|esac|in|function|export|return|exit)\b/,
    },
    { name: "num", regex: /\b\d+\b/ },
    {
      name: "call",
      regex:
        /\b(?:sudo|apt|apt-get|pacman|dnf|yum|brew|docker|podman|make|cmake|ninja|git|curl|wget|python3|python|pip|pip3|bash|sh|zsh|chmod|chown|tar|grep|rg|sed|awk)\b/,
    },
  ];
  const full = new RegExp(
    rules.map((r) => "(" + r.regex.source + ")").join("|"),
    "g",
  );
  let lastIdx = 0,
    out = "",
    m;
  while ((m = full.exec(code)) !== null) {
    out += esc(code.substring(lastIdx, m.index));
    for (let i = 0; i < rules.length; i++)
      if (m[i + 1] !== undefined) {
        out += `<span class="syn-${rules[i].name}">${esc(m[i + 1])}</span>`;
        break;
      }
    lastIdx = full.lastIndex;
  }
  return out + esc(code.substring(lastIdx));
}

function renderRichDocstring(text) {
  if (!text) return "";
  let html = esc(text);
  html = html
    .replace(/\*\*([^*]+)\*\*/g, "<b>$1</b>")
    .replace(/\*([^*]+)\*/g, "<i>$1</i>")
    .replace(/`([^`]+)`/g, "<code>$1</code>")
    .replace(/^\s*[-*]\s+(.*)$/gm, "<ul><li>$1</li></ul>")
    .replace(/<\/ul>\n<ul>/g, "\n")
    .replace(/\[\[([a-zA-Z0-9_.:(\), ]+)\]\]/g, (match, p1) => {
      const parts = p1.split("::");
      if (parts.length > 1) {
        const mod = parts[0].trim();
        const sym = parts.slice(1).join("::").trim();
        return `<a href="${routeHash(mod, sym)}" onclick="selectModule('${jsArg(mod)}', '${jsArg(sym)}');event.preventDefault();">${esc(`${mod}::${sym}`)}</a>`;
      }
      const mod = p1.trim();
      return `<a href="${routeHash(mod)}" onclick="selectModule('${jsArg(mod)}');event.preventDefault();">${esc(mod)}</a>`;
    });
  return html;
}

function parseModuleDoc(text) {
  const info = { summary: "", keywords: [], sections: [], refs: [] };
  const raw = String(text ?? "").replace(/\r\n?/g, "\n");
  const lines = raw.split("\n").map((line) => line.trim());
  let intro = [];
  let section = null;
  const topSections = new Set(
    "abstract algorithm api bigint bigrational contract encoding example examples format invariant invariants layout note notes reference references security storage".split(
      " ",
    ),
  );
  const pushIntro = (line) => {
    if (line) intro.push(line);
  };
  const pushSection = (title, body = "") => {
    section = { title, body: [] };
    if (body) section.body.push(body);
    info.sections.push(section);
  };
  const addRef = (line) => {
    const ref = String(line ?? "")
      .replace(/^[-*]\s*/, "")
      .trim();
    if (ref) info.refs.push(ref);
  };

  lines.forEach((line) => {
    if (!line) {
      if (
        section &&
        section.body.length &&
        section.body[section.body.length - 1] !== ""
      )
        section.body.push("");
      else if (intro.length && intro[intro.length - 1] !== "") intro.push("");
      return;
    }
    const kv = line.match(/^([A-Za-z][A-Za-z0-9 _/-]{1,40}):\s*(.*)$/);
    if (kv) {
      const key = kv[1].trim();
      const body = kv[2].trim();
      const normalized = key.toLowerCase().replace(/\s+/g, " ");
      if (normalized === "keywords") {
        info.keywords = info.keywords.concat(
          body
            .split(/[,\s]+/)
            .map((s) => s.trim())
            .filter(Boolean),
        );
        return;
      }
      if (
        normalized === "reference" ||
        normalized === "references" ||
        normalized === "see also"
      ) {
        section = null;
        if (body) addRef(body);
        return;
      }
      if (topSections.has(normalized) || /^[A-Z][A-Za-z0-9 _/-]*$/.test(key)) {
        pushSection(key, body);
        return;
      }
    }
    if (/^[-*]\s*(?:https?:\/\/|www\.)/i.test(line)) {
      section = null;
      addRef(line);
      return;
    }
    if (section) section.body.push(line);
    else pushIntro(line);
  });

  info.keywords = [...new Set(info.keywords)];
  info.summary = intro
    .join("\n")
    .replace(/\n{3,}/g, "\n\n")
    .trim();
  info.sections = info.sections
    .map((s) => ({
      title: s.title,
      body: s.body
        .join("\n")
        .replace(/\n{3,}/g, "\n\n")
        .trim(),
    }))
    .filter((s) => s.title || s.body);
  return info;
}

const normalizeTag = (tag) =>
  String(tag ?? "")
    .trim()
    .toLowerCase()
    .replace(/[^a-z0-9_.+-]+/g, "-")
    .replace(/^-+|-+$/g, "");

function parsedModuleDoc(modOrText) {
  if (modOrText && typeof modOrText === "object") {
    if (!modOrText._parsed_doc)
      modOrText._parsed_doc = parseModuleDoc(modOrText.module_doc);
    return modOrText._parsed_doc;
  }
  return parseModuleDoc(modOrText);
}

function moduleDocSummary(modOrText) {
  const parsed = parsedModuleDoc(modOrText);
  if (parsed.summary)
    return parsed.summary.split(/\n\n+/)[0].replace(/\n+/g, " ");
  const firstSection = parsed.sections.find((s) => s.body);
  if (firstSection)
    return firstSection.body.split(/\n\n+/)[0].replace(/\n+/g, " ");
  return moduleFallbackSummary(modOrText);
}

function moduleFallbackSummary(modOrText) {
  return "";
}

function renderModuleDoc(modOrText) {
  const doc = parsedModuleDoc(modOrText);
  if (
    !doc.summary &&
    !doc.keywords.length &&
    !doc.sections.length &&
    !doc.refs.length
  )
    return "";
  const linkRef = (ref) => {
    const m = String(ref ?? "").match(/https?:\/\/[^\s)]+|www\.[^\s)]+/i);
    if (!m) return esc(ref);
    const href = m[0].startsWith("www.") ? `https://${m[0]}` : m[0];
    return `${esc(ref.slice(0, m.index))}<a href="${esc(href)}" rel="noopener noreferrer">${esc(m[0])}${icon("external", "ico ico-xs")}</a>${esc(ref.slice(m.index + m[0].length))}`;
  };
  let html = `<div class="module-doc-panel">`;
  if (doc.keywords.length) {
    html += `<div class="module-keywords"><span>KEYWORDS</span>${doc.keywords
      .map((k) => {
        const tag = normalizeTag(k);
        return `<a class="module-keyword-chip" href="${routeHash("tag", tag)}" data-select-tag="${esc(tag)}"><code>${esc(k)}</code></a>`;
      })
      .join("")}</div>`;
  }
  if (doc.summary)
    html += `<div class="module-summary">${renderRichDocstring(doc.summary)}</div>`;
  doc.sections.forEach((section) => {
    html += `<div class="module-doc-section"><div class="module-doc-title">${esc(section.title)}</div><div class="module-doc-body">${renderRichDocstring(section.body)}</div></div>`;
  });
  if (doc.refs.length) {
    html += `<div class="module-doc-section"><div class="module-doc-title">References</div><ul class="module-ref-list">`;
    doc.refs.forEach((ref) => {
      html += `<li>${linkRef(ref)}</li>`;
    });
    html += `</ul></div>`;
  }
  return html + `</div>`;
}

function addModuleToNavTree(tree, mod) {
  let currentNode = tree;
  (mod.path || []).forEach((component, index) => {
    if (!currentNode[component]) currentNode[component] = { _modules: [] };
    if (index === mod.path.length - 1)
      currentNode[component]._modules.push(mod);
    currentNode = currentNode[component];
  });
}

function addModuleToCategoryCache(mod) {
  const parts = String(mod.name || "").split(".");
  for (let i = 1; i <= parts.length; i++) {
    const path = parts.slice(0, i).join(".");
    if (!categoryModulesByPath.has(path)) categoryModulesByPath.set(path, []);
    categoryModulesByPath.get(path).push(mod);
  }
}

function sortNavTree(node) {
  if (!node) return;
  if (node._modules) node._modules.sort((a, b) => a.name.localeCompare(b.name));
  Object.keys(node).forEach((key) => {
    if (key !== "_modules") sortNavTree(node[key]);
  });
}

function buildDocTree(docs) {
  const makeNode = (path) => ({ path, entries: [], children: new Map() });
  const root = makeNode("");
  docs.forEach((doc) => {
    const parts = String(doc.name || "")
      .split("/")
      .filter(Boolean);
    if (!parts.length) return;
    let node = root;
    for (let i = 0; i < parts.length - 1; i++) {
      const key = parts[i];
      let child = node.children.get(key);
      if (!child) {
        child = makeNode(parts.slice(0, i + 1).join("/"));
        node.children.set(key, child);
        node.entries.push({ type: "group", key, node: child });
      }
      node = child;
    }
    node.entries.push({ type: "doc", doc });
  });
  return root;
}

function initDataIndexes() {
  apiModuleList = [];
  navTreeCache = {};
  categoryModulesByPath = new Map();
  overviewModule = null;
  data.forEach((m) => {
    if (m.name === "Overview") overviewModule = m;
    else apiModuleList.push(m);
    m._sortedSymbols = (m.symbols || [])
      .slice()
      .sort((a, b) => a.name.localeCompare(b.name));
    moduleByName.set(m.name, m);
    (m.markdown_docs || []).forEach((doc) =>
      markdownDocByName.set(doc.name, doc),
    );
    addModuleToNavTree(navTreeCache, m);
    if (m.name !== "Overview") addModuleToCategoryCache(m);
    const parts = String(m.name || "").split(".");
    let prefix = "";
    parts.forEach((part, index) => {
      prefix = index === 0 ? part : `${prefix}.${part}`;
      if (index < parts.length - 1) categorySet.add(prefix);
    });

    const doc = parsedModuleDoc(m);
    const tags = [
      ...new Set((doc.keywords || []).map(normalizeTag).filter(Boolean)),
    ].sort();
    m.tags = tags;
    tags.forEach((tag) => {
      if (!tagIndex.has(tag)) tagIndex.set(tag, []);
      tagIndex.get(tag).push(m);
    });
  });
  sortNavTree(navTreeCache);
  markdownDocsCache = sortMarkdownDocs(
    overviewModule ? overviewModule.markdown_docs : [],
  );
  docTreeCache = buildDocTree(
    markdownDocsCache.filter((d) => d.name !== "NY" && d.name !== "NYTRIX"),
  );
  tagEntries = Array.from(tagIndex.entries())
    .map(([tag, modules]) => ({
      tag,
      modules: modules.slice().sort((a, b) => a.name.localeCompare(b.name)),
    }))
    .sort((a, b) => a.tag.localeCompare(b.tag));

  searchRows = [];
  apiModuleList.forEach((m) => {
    searchRows.push({
      type: "module",
      mod: m.name,
      id: null,
      name: m.name,
      kind: "module",
      haystack:
        `${m.name} ${(m.tags || []).join(" ")} ${moduleDocSummary(m)} ${m.module_doc || ""}`.toLowerCase(),
    });
    (m.symbols || []).forEach((s) =>
      searchRows.push({
        type: "symbol",
        mod: m.name,
        id: s.id,
        name: s.name,
        kind: s.kind,
        symbol: s,
        haystack:
          `${m.name} ${(m.tags || []).join(" ")} ${s.id || ""} ${s.name || ""} ${s.kind || ""} ${s.sig || ""} ${s.doc || ""} ${(s.imports || []).join(" ")}`.toLowerCase(),
      }),
    );
  });
  markdownDocByName.forEach((doc) => {
    searchRows.push({
      type: "doc",
      mod: doc.name,
      id: null,
      name: markdownDocLabel(doc),
      kind: "doc",
      doc,
      haystack:
        `${doc.name} ${doc.title || ""} ${stripDocText(doc.html)}`.toLowerCase(),
    });
  });
}

function searchTerms(query) {
  return String(query || "")
    .trim()
    .toLowerCase()
    .split(/[\s,;]+/)
    .map((term) => term.trim())
    .filter(Boolean);
}

function searchScore(row, query, terms) {
  if (!terms.length) return -1;
  const haystack = row.haystack || "";
  for (const term of terms) {
    if (!haystack.includes(term)) return -1;
  }

  const raw = String(query || "").trim().toLowerCase();
  const name = String(row.name || "").toLowerCase();
  const mod = String(row.mod || "").toLowerCase();
  const id = String(row.id || "").toLowerCase();
  const kindBoost = row.type === "module" ? 80 : row.type === "symbol" ? 45 : 20;
  let score = kindBoost;

  if (name === raw || mod === raw || id === raw) score += 1200;
  else if (name.startsWith(raw) || mod.startsWith(raw) || id.startsWith(raw))
    score += 800;
  else if (name.includes(raw) || mod.includes(raw) || id.includes(raw))
    score += 500;
  else if (haystack.includes(raw)) score += 180;

  for (const term of terms) {
    if (name === term || mod === term || id === term) score += 260;
    else if (
      name.startsWith(term) ||
      mod.startsWith(term) ||
      id.startsWith(term)
    )
      score += 150;
    else if (name.includes(term) || mod.includes(term) || id.includes(term))
      score += 80;
  }

  const first = haystack.indexOf(raw);
  if (first >= 0) score += Math.max(0, 80 - Math.min(first, 80));
  return score;
}

const getArgs = (sig) => (sig || "").match(/\((.*)\)/)?.[1]?.trim() || "";
const ROUTE_NAME_RE = /^[A-Za-z0-9_.]+$/;
const ROUTE_SYMBOL_RE = /^[A-Za-z0-9_.:-]+$/;
const SOURCE_SYMBOL_ID = "__source";

function symbolBaseName(symbol) {
  const id = String((symbol && symbol.id) || "").trim();
  if (id) return id;
  return String((symbol && symbol.name) || "")
    .replace(/^(?:fn|def|mut|extern)\s+/, "")
    .split(/[(:\s]/)[0]
    .trim();
}

function isInternalSymbol(symbol) {
  return symbolBaseName(symbol).startsWith("_");
}

function moduleInternalSymbolCount(mod) {
  return ((mod && mod._sortedSymbols) || []).filter(isInternalSymbol).length;
}

function moduleVisibleSymbols(mod) {
  const syms = (mod && mod._sortedSymbols) || [];
  return showInternalSymbols ? syms : syms.filter((s) => !isInternalSymbol(s));
}

function setInternalSymbolsVisible(open) {
  showInternalSymbols = Boolean(open);
  try {
    localStorage.setItem(
      INTERNAL_SYMBOLS_STORAGE_KEY,
      showInternalSymbols ? "1" : "0",
    );
  } catch {}
}

function selectedSymbol(mod, symbolNameOrId) {
  if (!mod || !symbolNameOrId) return null;
  return (
    (mod.symbols || []).find(
      (s) => s.id === symbolNameOrId || s.name === symbolNameOrId,
    ) || null
  );
}

function resolveRouteName(name) {
  const raw = String(name ?? "").trim();
  if (raw === "Overview") return raw;
  if (raw === "Tags" || raw === "tags") return "Tags";
  const doc = markdownDocByName.get(raw);
  if (doc) return doc.name;
  const mod = moduleByName.get(raw);
  if (mod) return mod.name;
  if (ROUTE_NAME_RE.test(raw) && categorySet.has(raw)) return raw;
  return null;
}

function resolveRouteSymbol(moduleName, symbolNameOrId) {
  if (!symbolNameOrId) return null;
  const raw = String(symbolNameOrId).trim();
  if (!ROUTE_SYMBOL_RE.test(raw)) return null;
  const mod = moduleByName.get(moduleName);
  if (!mod || !mod.symbols) return raw;
  const sym = mod.symbols.find((s) => s.id === raw || s.name === raw);
  return sym ? sym.id : raw;
}

function splitDocHref(rawHref) {
  const raw = String(rawHref ?? "").trim();
  const hash = raw.indexOf("#");
  return {
    path: hash >= 0 ? raw.slice(0, hash) : raw,
    anchor: hash >= 0 ? decodeRouteHash(raw.slice(hash + 1)) : "",
  };
}

function isExternalHref(rawHref) {
  const href = String(rawHref ?? "").trim();
  return /^(?:[a-z][a-z0-9+.-]*:|\/\/)/i.test(href);
}

function docRouteFromHref(rawHref, baseRoute) {
  const href = String(rawHref ?? "").trim();
  if (!href || isExternalHref(href)) return null;

  const { path, anchor } = splitDocHref(href);
  if (!path) return { route: baseRoute || current || "Overview", anchor };

  let cleanPath = path.split("?")[0].replace(/\\/g, "/").trim();
  try {
    cleanPath = decodeURIComponent(cleanPath);
  } catch {
    /* keep original spelling */
  }

  cleanPath = cleanPath.replace(/^\/+/, "").replace(/^docs\//, "");
  cleanPath = cleanPath.replace(/\/index(?:\.html?)?$/i, "");
  cleanPath = cleanPath.replace(/\.(?:md|html?)$/i, "");
  if (!cleanPath) return { route: baseRoute || "Overview", anchor };

  const rootDocPath = /^(?:learn|spec)\//.test(cleanPath);
  const absolute =
    path.startsWith("/") || path.startsWith("docs/") || rootDocPath;
  const baseParts = absolute
    ? []
    : String(baseRoute || "")
        .split("/")
        .filter(Boolean)
        .slice(0, -1);
  const parts = baseParts;
  cleanPath.split("/").forEach((part) => {
    if (!part || part === ".") return;
    if (part === "..") parts.pop();
    else parts.push(part);
  });

  let route = parts.join("/");
  route = route.replace(/^docs\//, "");
  if (markdownDocByName.has(route)) return { route, anchor };
  if (moduleByName.has(route) || categorySet.has(route))
    return { route, anchor };
  return null;
}

function rewriteMarkdownDocLinks() {
  document.querySelectorAll(".md-section").forEach((section) => {
    const baseRoute = section.dataset.docRoute || current || "";
    section
      .querySelectorAll(".markdown-content a[href], .html-content a[href]")
      .forEach((link) => {
        const rawHref = link.getAttribute("href") || "";
        const resolved = docRouteFromHref(rawHref, baseRoute);
        if (resolved && resolved.route) {
          link.dataset.docRoute = resolved.route;
          if (resolved.anchor) link.dataset.docAnchor = resolved.anchor;
          link.setAttribute("href", routeHash(resolved.route));
          return;
        }
        if (rawHref.startsWith("#")) {
          link.dataset.docAnchor = decodeRouteHash(rawHref.slice(1));
          return;
        }
        if (isExternalHref(rawHref)) {
          link.setAttribute("target", "_blank");
          link.setAttribute("rel", "noopener noreferrer");
        }
      });
  });
}

function copyText(t, btn) {
  const text = String(t ?? "");
  const done = () => {
    const old = btn.innerHTML;
    btn.innerHTML = `${icon("check", "ico ico-xs")}<span>OK</span>`;
    setTimeout(() => {
      btn.innerHTML = old;
    }, 1000);
  };
  if (navigator.clipboard && navigator.clipboard.writeText) {
    navigator.clipboard
      .writeText(text)
      .then(done)
      .catch(() => copyTextFallback(text, done));
  } else copyTextFallback(text, done);
}

function copyTextFallback(text, done) {
  const ta = document.createElement("textarea");
  ta.value = text;
  ta.setAttribute("readonly", "");
  ta.style.position = "fixed";
  ta.style.opacity = "0";
  document.body.appendChild(ta);
  ta.select();
  try {
    document.execCommand("copy");
  } catch {
    /* best effort for local file previews */
  }
  ta.remove();
  if (done) done();
}

function isDesktopNav() {
  return window.matchMedia && window.matchMedia("(min-width: 1040px)").matches;
}

function clearDesktopNavTimer() {
  if (!navHideTimer) return;
  clearTimeout(navHideTimer);
  navHideTimer = null;
}

function clearMobileNavState() {
  const aside = $("aside"),
    main = $("content-area"),
    backdrop = $("backdrop"),
    hamburger = $("hamburger");
  if (aside) aside.classList.remove("active");
  if (main) main.classList.remove("aside-active");
  if (backdrop) backdrop.classList.remove("active");
  if (hamburger) {
    hamburger.classList.remove("active");
    hamburger.setAttribute("aria-expanded", "false");
  }
}

function setDesktopNav(open) {
  if (!isDesktopNav()) {
    document.body.classList.remove("nav-open");
    return;
  }
  clearDesktopNavTimer();
  clearMobileNavState();
  document.body.classList.toggle("nav-open", Boolean(open));
}

function isDesktopNavHeld() {
  const aside = $("aside"),
    trigger = $("dock-trigger");
  const active = document.activeElement;
  return Boolean(
    (aside &&
      (aside.matches(":hover") ||
        aside.matches(":focus-within") ||
        (active && aside.contains(active)))) ||
    (trigger && trigger.matches(":hover")),
  );
}

function scheduleDesktopNavClose(delay = 360) {
  clearDesktopNavTimer();
  if (!isDesktopNav()) return;
  navHideTimer = setTimeout(() => {
    if (!isDesktopNavHeld()) document.body.classList.remove("nav-open");
    navHideTimer = null;
  }, delay);
}

function setupDesktopNav() {
  const aside = $("aside"),
    trigger = $("dock-trigger"),
    main = $("content-area");
  const open = () => setDesktopNav(true);
  const close = () => scheduleDesktopNavClose();

  if (trigger) {
    trigger.addEventListener("mouseenter", open);
    trigger.addEventListener("mouseleave", close);
    trigger.addEventListener("focus", open);
    trigger.addEventListener("blur", close);
    trigger.addEventListener("click", open);
  }
  if (aside) {
    aside.addEventListener("mouseenter", open);
    aside.addEventListener("mouseleave", close);
    aside.addEventListener("focusin", open);
    aside.addEventListener("focusout", close);
  }
  if (main)
    main.addEventListener("mouseenter", () => scheduleDesktopNavClose(180));
  window.addEventListener("resize", () => {
    if (isDesktopNav())
      setDesktopNav(document.body.classList.contains("nav-open"));
    else {
      clearDesktopNavTimer();
      document.body.classList.remove("nav-open");
    }
  });
}

function toggleNavCategory(categoryId) {
  const categoryElement = $(categoryId);
  const gtitleElement = categoryElement.previousElementSibling;
  if (categoryElement.classList.contains("active")) {
    categoryElement.classList.remove("active");
    gtitleElement.classList.remove("active", "open");
  } else {
    categoryElement.classList.add("active");
    gtitleElement.classList.add("active", "open");
  }
}

function toggleNavModule(moduleId, moduleName = null) {
  const moduleElement = $(moduleId);
  const gtitleElement = moduleElement.previousElementSibling;
  if (!moduleElement || !gtitleElement) return;
  if (moduleElement.classList.contains("active")) {
    moduleElement.classList.remove("active");
    gtitleElement.classList.remove("active", "open");
  } else {
    if (moduleName && !moduleElement.dataset.loaded) {
      const mod = moduleByName.get(moduleName);
      if (mod) {
        const depth = Number(moduleElement.dataset.symbolDepth || "0");
        moduleElement.innerHTML = renderSymbolNavItems(mod, depth);
        moduleElement.dataset.loaded = "1";
      }
    }
    moduleElement.classList.add("active");
    gtitleElement.classList.add("active", "open");
  }
}

function toggleOverviewModule(moduleId) {
  const moduleElement = $(moduleId);
  const cardRowElement = moduleElement.previousElementSibling;
  if (moduleElement.classList.contains("active")) {
    moduleElement.classList.remove("active");
    cardRowElement.classList.remove("active");
  } else {
    moduleElement.classList.add("active");
    cardRowElement.classList.add("active");
  }
}

function toggleSidebar() {
  const asideElement = $("aside"),
    mainElement = $("content-area"),
    backdrop = $("backdrop"),
    hamburger = $("hamburger");
  if (isDesktopNav()) {
    const nextOpen = !document.body.classList.contains("nav-open");
    setDesktopNav(nextOpen);
    if (!nextOpen) {
      const searchInput = $("search");
      if (searchInput) searchInput.value = "";
    }
    return;
  }
  if (asideElement && mainElement) {
    const isActive = asideElement.classList.toggle("active");
    mainElement.classList.toggle("aside-active");
    if (backdrop) backdrop.classList.toggle("active", isActive);
    if (hamburger) {
      hamburger.classList.toggle("active", isActive);
      hamburger.setAttribute("aria-expanded", isActive ? "true" : "false");
    }
    if (!isActive) {
      const searchInput = $("search");
      if (searchInput) searchInput.value = "";
    }
  }
}

function tagEntriesByWeight(limit = null) {
  const entries = tagEntries.slice().sort((a, b) => {
    const n = b.modules.length - a.modules.length;
    return n || a.tag.localeCompare(b.tag);
  });
  return limit == null ? entries : entries.slice(0, limit);
}

function renderTagSection(depth = 0) {
  if (!tagEntries.length) return "";
  const tagId = "nav-tags";
  const active = current === "Tags" || (current && current.startsWith("tag::"));
  const open = true;
  const currentTag =
    current && current.startsWith("tag::") ? current.slice(5) : "";
  const visible = tagEntriesByWeight(28);
  if (currentTag && !visible.some((entry) => entry.tag === currentTag)) {
    const selected = tagEntries.find((entry) => entry.tag === currentTag);
    if (selected) visible.unshift(selected);
  }
  let html = `<div class="gtitle collapsible ${active ? "active" : open ? "open" : ""}" style="padding-left: ${12 + depth * 10}px;" onclick="toggleNavCategory('${tagId}')"><span class="nav-label">${icon("tag", "ico nav-ico")}<span>TAGS</span></span>${collapseIcon()}</div>`;
  html += `<div id="${tagId}" class="collapsible-content ${open ? "active" : ""}">`;
  html += `<a class="item tags-index-item ${current === "Tags" ? "active" : ""}" href="${routeHash("tags")}" onclick="selectTagIndex();event.preventDefault();" style="padding-left: ${12 + (depth + 1) * 10}px;"><span class="nav-label">${icon("list", "ico nav-ico")}<span>All tags</span></span><span class="tag-count">${tagEntries.length}</span></a>`;
  html += `<div class="tag-nav-cloud" style="padding-left: ${12 + (depth + 1) * 10}px;">`;
  visible.forEach((entry) => {
    const itemActive = current === `tag::${entry.tag}`;
    html += `<a class="tag-nav-chip ${itemActive ? "active" : ""}" href="${routeHash("tag", entry.tag)}" onclick="selectTag('${jsArg(entry.tag)}');event.preventDefault();">${icon("tag", "ico ico-xs")}<span class="tag-nav-name">${esc(entry.tag)}</span><span class="tag-nav-count">${entry.modules.length}</span></a>`;
  });
  return html + `</div></div>`;
}

function renderSourceNavItem(mod, depth) {
  if (!mod || !mod.source) return "";
  const sourceCurrent = current === `${mod.name}::${SOURCE_SYMBOL_ID}`;
  return `<a class="item is-source ${sourceCurrent ? "active" : ""}" href="${routeHash(mod.name, SOURCE_SYMBOL_ID)}" onclick="selectModule('${jsArg(mod.name)}', '${SOURCE_SYMBOL_ID}');event.preventDefault();" style="padding-left: ${12 + depth * 10}px;"><span class="nav-label">${icon("file", "ico nav-ico")}<span>Source</span></span></a>`;
}

function renderSymbolNavItems(mod, depth) {
  const sourceItem = renderSourceNavItem(mod, depth);
  const symbolItems = moduleVisibleSymbols(mod)
    .map((s) => {
      const symbolPart =
        current && current.includes("::") ? current.split("::")[1] : "";
      const isSymbolCurrent =
        current &&
        current.split("::")[0] === mod.name &&
        (symbolPart === s.name || symbolPart === s.id);
      const isFunction = s.kind === "function";
      return `<a class="item ${isSymbolCurrent ? "active" : ""} ${isFunction ? "is-function" : ""}" href="${routeHash(mod.name, s.id)}" onclick="selectModule('${jsArg(mod.name)}', '${jsArg(s.id)}');event.preventDefault();" style="padding-left: ${12 + depth * 10}px;"><span class="nav-label">${icon(isFunction ? "code" : "box", "ico nav-ico")}<span>${highlightNavLabel(s.name, s.kind)}</span></span></a>`;
    })
    .join("");
  return sourceItem + symbolItems;
}

function renderNav() {
  const overviewMod = overviewModule;
  const mdDocs = markdownDocsCache;
  const infoDocs = mdDocs.filter((d) => d.name === "NY" || d.name === "NYTRIX");
  const miscDocs = mdDocs.filter((d) => d.name !== "NY" && d.name !== "NYTRIX");

  function sortedDocEntries(entries) {
    const groupRank = (key) => {
      const order = ["learn", "spec"];
      const idx = order.indexOf(String(key || "").toLowerCase());
      return idx === -1 ? 20 : idx;
    };
    const docRank = (doc) => {
      const order = ["CHANGELOG", "README"];
      const name = String((doc && doc.name) || "");
      const idx = order.indexOf(name);
      if (idx !== -1) return idx;
      const priority = MARKDOWN_DOC_PRIORITY.indexOf(name);
      return priority === -1 ? 100 : 20 + priority;
    };
    return entries.slice().sort((a, b) => {
      if (a.type !== b.type) return a.type === "group" ? -1 : 1;
      if (a.type === "group") {
        const ra = groupRank(a.key),
          rb = groupRank(b.key);
        if (ra !== rb) return ra - rb;
        return String(a.key || "").localeCompare(String(b.key || ""));
      }
      const ra = docRank(a.doc),
        rb = docRank(b.doc);
      if (ra !== rb) return ra - rb;
      return String((a.doc && a.doc.name) || "").localeCompare(
        String((b.doc && b.doc.name) || ""),
      );
    });
  }

  function renderMarkdownDocTree(node, depth) {
    let html = "";
    sortedDocEntries(node.entries).forEach((entry) => {
      if (entry.type === "doc") {
        const doc = entry.doc;
        const itemActive = current === doc.name;
        const label = String(doc.name || "").includes("/")
          ? docLeafTitle(doc)
          : docTitle(doc);
        html += `<a class="item ${itemActive ? "active" : ""}" href="${routeHash(doc.name)}" onclick="selectModule('${jsArg(doc.name)}');event.preventDefault();" style="padding-left: ${12 + depth * 10}px;"><span class="nav-label">${icon(docIconName(doc.name), "ico nav-ico")}<span>${label}</span></span></a>`;
        return;
      }
      const groupPath = entry.node.path;
      const groupId = `nav-doc-${groupPath.replace(/[^a-zA-Z0-9]/g, "_")}-${depth}`;
      const active =
        current &&
        (current === groupPath || current.startsWith(groupPath + "/"));
      html += `<div class="gtitle collapsible ${active ? "active" : ""}" style="padding-left: ${12 + depth * 10}px;" onclick="toggleNavCategory('${groupId}')"><span class="nav-label">${icon("folder", "ico nav-ico")}<span>${docSegmentNavTitle(entry.key)}</span></span>${collapseIcon()}</div>`;
      html += `<div id="${groupId}" class="collapsible-content ${active ? "active" : ""}">`;
      html += renderMarkdownDocTree(entry.node, depth + 1);
      html += `</div>`;
    });
    return html;
  }

  function renderInfoSection(depth) {
    if (!infoDocs.length) return "";
    const infoId = `nav-info-${depth}`;
    const infoActive = infoDocs.some((doc) => doc.name === current);
    let html = "";
    html += `<div class="gtitle collapsible ${infoActive ? "active" : ""}" style="padding-left: ${12 + depth * 10}px;" onclick="toggleNavCategory('${infoId}')"><span class="nav-label">${icon("file", "ico nav-ico")}<span>INFO</span></span>${collapseIcon()}</div>`;
    html += `<div id="${infoId}" class="collapsible-content ${infoActive ? "active" : ""}">`;
    infoDocs.forEach((doc) => {
      const itemActive = current === doc.name;
      html += `<a class="item ${itemActive ? "active" : ""}" href="${routeHash(doc.name)}" onclick="selectModule('${jsArg(doc.name)}');event.preventDefault();" style="padding-left: ${12 + (depth + 1) * 10}px;"><span class="nav-label">${icon("file", "ico nav-ico")}<span>${docTitle(doc)}</span></span></a>`;
    });
    html += `</div>`;
    return html;
  }

  function renderTree(node, depth = 0, currentPath = "") {
    let nodeHtml = "";
    const sortedKeys = Object.keys(node).sort((a, b) => {
      const priority = [
        "Home",
        "README",
        "LANGUAGE",
        "NY",
        "NYTRIX",
        "PROPOSALS",
        "DOCKER",
      ];
      const ia = priority.indexOf(a),
        ib = priority.indexOf(b);
      if (ia !== -1 && ib !== -1) return ia - ib;
      if (ia !== -1) return -1;
      if (ib !== -1) return 1;
      return a.localeCompare(b);
    });

    sortedKeys.forEach((key) => {
      if (key === "_modules") return;

      const newPath = currentPath ? `${currentPath}.${key}` : key;
      const currentCategory = node[key];
      const categoryId = `nav-cat-${newPath.replace(/[^a-zA-Z0-9]/g, "_")}-${depth}`;
      const isPathActive =
        current &&
        (current === newPath ||
          current.startsWith(newPath + ".") ||
          current.startsWith(newPath + "::"));

      if (key === "Home") {
        const isActive =
          depth === 0 ||
          current === "Overview" ||
          (overviewMod &&
            overviewMod.markdown_docs.some((d) => d.name === current));
        const hasHomeDocs = miscDocs.length > 0 || infoDocs.length > 0;
        nodeHtml += `<div class="gtitle ${hasHomeDocs ? "collapsible" : ""} ${isActive ? "active" : ""}" style="padding-left: ${12 + depth * 10}px;" ${hasHomeDocs ? `onclick="toggleNavCategory('${categoryId}')"` : ""}><span class="nav-label">${icon("home", "ico nav-ico")}<span>${esc(key)}</span></span>${hasHomeDocs ? collapseIcon() : ""}</div>`;
        if (hasHomeDocs) {
          nodeHtml += `<div id="${categoryId}" class="collapsible-content ${isActive ? "active" : ""}">`;
          const homeDocTree = docTreeCache || buildDocTree(miscDocs);
          const rootGroups = {
            entries: homeDocTree.entries.filter(
              (entry) => entry.type === "group",
            ),
          };
          const rootDocs = {
            entries: homeDocTree.entries.filter(
              (entry) => entry.type === "doc",
            ),
          };
          nodeHtml += renderMarkdownDocTree(rootGroups, depth + 1);
          const isOverviewActive = current === "Overview";
          nodeHtml += `<a class="item ${isOverviewActive ? "active" : ""}" href="#Overview" onclick="selectModule('Overview');event.preventDefault();" style="padding-left: ${12 + (depth + 1) * 10}px;"><span class="nav-label">${icon("book", "ico nav-ico")}<span>OVERVIEW</span></span></a>`;
          nodeHtml += renderMarkdownDocTree(rootDocs, depth + 1);
          nodeHtml += `</div>`;
        }
        if (depth === 0) {
          nodeHtml += renderInfoSection(depth);
        }
        return;
      }

      const isDirectModule =
        currentCategory._modules.length === 1 &&
        currentCategory._modules[0].name === newPath &&
        Object.keys(currentCategory).filter((k) => k !== "_modules").length ===
          0;

      if (isDirectModule) {
        const m = currentCategory._modules[0];
        const moduleId = `nav-mod-syms-${m.name.replace(/[^a-zA-Z0-9]/g, "_")}`;
        const hasSymbols = m._sortedSymbols && m._sortedSymbols.length > 0;
        const hasNavItems = hasSymbols || Boolean(m.source);
        const isModuleCurrent =
          current && (current === m.name || current.startsWith(`${m.name}::`));
        const moduleActive = isModuleCurrent;

        nodeHtml += `<div class="gtitle ${hasNavItems ? "collapsible" : ""} ${moduleActive ? "active" : ""}" style="padding-left: ${12 + depth * 10}px;" ${hasNavItems ? `onclick="toggleNavModule('${moduleId}', '${jsArg(m.name)}')"` : ""}><span class="nav-label" onclick="selectModule('${jsArg(m.name)}');event.stopPropagation();" style="cursor: pointer; flex: 1;">${icon(moduleIconName(m.name), "ico nav-ico")}<span>${highlightNavLabel(key, "module")}</span></span>${hasNavItems ? collapseIcon() : ""}</div>`;
        if (hasNavItems) {
          nodeHtml += `<div id="${moduleId}" class="collapsible-content ${moduleActive ? "active" : ""}" data-symbol-depth="${depth + 1}" ${moduleActive ? `data-loaded="1"` : ""}>`;
          if (moduleActive) nodeHtml += renderSymbolNavItems(m, depth + 1);
          nodeHtml += `</div>`;
        }
      } else {
        const hasChildren =
          currentCategory._modules.length > 0 ||
          Object.keys(currentCategory).some((subKey) => subKey !== "_modules");
        const isActive = isPathActive;
        const defaultOpen = depth === 0 && key === "std";
        const isOpen = isActive || defaultOpen;
        const categoryModule = currentCategory._modules.find(
          (m) => m.name === newPath,
        );
        const navTarget = categoryModule ? categoryModule.name : newPath;

        nodeHtml += `<div class="gtitle ${hasChildren ? "collapsible" : ""} ${isActive ? "active" : isOpen ? "open" : ""}" style="padding-left: ${12 + depth * 10}px;" ${hasChildren ? `onclick="toggleNavCategory('${categoryId}')"` : ""}><span class="nav-label" onclick="selectModule('${jsArg(navTarget)}');event.stopPropagation();" style="cursor: pointer; flex: 1;">${icon(moduleIconName(navTarget), "ico nav-ico")}<span>${highlightNavLabel(key, "module")}</span></span>${hasChildren ? collapseIcon() : ""}</div>`;
        if (hasChildren)
          nodeHtml += `<div id="${categoryId}" class="collapsible-content ${isOpen ? "active" : ""}">`;

        currentCategory._modules.forEach((m) => {
          if (hasChildren && m.name === newPath) return;
          const moduleId = `nav-mod-syms-${m.name.replace(/[^a-zA-Z0-9]/g, "_")}`;
          const hasSymbols = m._sortedSymbols && m._sortedSymbols.length > 0;
          const hasNavItems = hasSymbols || Boolean(m.source);
          const isModuleCurrent =
            current &&
            (current === m.name || current.startsWith(`${m.name}::`));
          const moduleActive = isModuleCurrent;
          const moduleShortName = m.name.split(".").pop();

          if (hasNavItems) {
            nodeHtml += `<div class="gtitle collapsible ${moduleActive ? "active" : ""}" style="padding-left: ${12 + (depth + 1) * 10}px;" onclick="toggleNavModule('${moduleId}', '${jsArg(m.name)}')"><span class="nav-label" onclick="selectModule('${jsArg(m.name)}');event.stopPropagation();" style="cursor: pointer; flex: 1;">${icon(moduleIconName(m.name), "ico nav-ico")}<span>${highlightNavLabel(moduleShortName, "module")}</span></span>${collapseIcon()}</div>`;
            nodeHtml += `<div id="${moduleId}" class="collapsible-content ${moduleActive ? "active" : ""}" data-symbol-depth="${depth + 2}" ${moduleActive ? `data-loaded="1"` : ""}>`;
            if (moduleActive) nodeHtml += renderSymbolNavItems(m, depth + 2);
            nodeHtml += `</div>`;
          } else {
            nodeHtml += `<a class="item${current === m.name ? " active" : ""}" href="${routeHash(m.name)}" onclick="selectModule('${jsArg(m.name)}');event.preventDefault();" style="padding-left: ${12 + (depth + 1) * 10}px;"><span class="nav-label">${icon(moduleIconName(m.name), "ico nav-ico")}<span>${highlightNavLabel(moduleShortName, "module")}</span></span></a>`;
          }
        });

        nodeHtml += renderTree(currentCategory, depth + 1, newPath);
        if (hasChildren) nodeHtml += `</div>`;
      }
    });
    return nodeHtml;
  }

  $("nav").innerHTML = renderTagSection(0) + renderTree(navTreeCache, 0, "");
}

function header(title, desc, stats) {
  const glyph =
    title === "Nytrix Manual"
      ? `<img src="logo.svg" alt="" onerror="this.remove()" />`
      : icon(pageIconName(title), "ico ico-lg");
  return `<div class="header"><div class="header-top"><div class="title-icon">${glyph}</div><div class="header-copy"><div class="mod-label">${icon("book", "ico ico-xs")}<span>MANUAL</span></div><div class="title">${esc(title)}</div><div class="desc">${esc(desc || "Language manual and API reference.")}</div><div class="stats">${esc(stats)}</div></div></div></div>`;
}

function renderDocCard(doc, tone = "") {
  if (!doc) return "";
  const label = markdownDocLabel(doc);
  const route = String(doc.name || "");
  const group = route.includes("/")
    ? titleCaseSegment(route.split("/")[0])
    : "Manual";
  return `<a class="doc-card ${tone}" href="${routeHash(route)}" data-select-module="${esc(route)}"><div class="doc-card-top"><span class="card-icon">${icon(docIconName(route))}</span><div class="doc-card-kicker">${esc(group)}</div></div><div class="doc-card-title">${esc(label)}</div><p>${esc(markdownDocCardSummary(doc))}</p><div class="doc-card-route">${esc(route)}</div></a>`;
}

function renderDocGroup(title, docs, desc = "") {
  const cards = docs
    .filter(Boolean)
    .map((doc) => renderDocCard(doc))
    .join("");
  if (!cards) return "";
  return `<section class="docs-section"><div class="docs-section-head"><div><h2>${esc(title)}</h2>${desc ? `<p>${esc(desc)}</p>` : ""}</div><span>${docs.filter(Boolean).length}</span></div><div class="doc-card-grid">${cards}</div></section>`;
}

function renderApiCard(route, title, summary, meta = "") {
  return `<a class="api-card" href="${routeHash(route)}" data-select-module="${esc(route)}"><div class="api-card-title">${icon(moduleIconName(route), "ico api-card-ico")}<span class="syn-module">${esc(title)}</span></div><p>${esc(summary || "Source-linked reference entry.")}</p>${meta ? `<div class="api-card-meta">${esc(meta)}</div>` : ""}</a>`;
}

function renderNamespaceCard(path, modules) {
  const mod = moduleByName.get(path);
  const label = path.split(".").pop();
  const summary =
    API_NAMESPACE_SUMMARIES.get(path) ||
    (mod
      ? moduleDocSummary(mod)
      : `Open the ${path} namespace.`);
  return renderApiCard(path, label, summary, "namespace");
}

function renderModuleCard(mod) {
  return renderApiCard(
    mod.name,
    mod.name.split(".").pop(),
    moduleDocSummary(mod),
    "module",
  );
}

function renderDocsHome(overviewMod) {
  setPageTitle("Home");
  const docs = sortMarkdownDocs(
    (overviewMod && overviewMod.markdown_docs) || [],
  ).filter((doc) => doc.name !== "NY" && doc.name !== "NYTRIX");
  const learnDocs = docs.filter((doc) => doc.name.startsWith("learn/"));
  const specDocs = docs.filter((doc) => doc.name.startsWith("spec/"));
  const projectDocs = docs.filter(
    (doc) => !doc.name.includes("/") && doc.name !== "README",
  );
  const quickStart = [
    ["wrench", "Build", "./make"],
    ["play", "Run", "ny hello.ny"],
    ["terminal", "REPL", "ny"],
    ["check", "Fmt", "ny fmt file.ny"],
    ["search", "Symbols", "ny doc search --symbols print"],
  ];
  const sampleProgram = `use std
def scale = comptime { 2^5 }
fn tag(n) case n {
   0..31 -> "small"
   32..63 -> "mid"
   _ -> "big"
}
def lens = [[3,4],[6,8],[5,12],[8,15]].map(fn(p){ int(sqrt(p[0]*p[0] + p[1]*p[1])) })
def sum = lens.reduce(0, fn(a,n){ a + n })
print(f"{scale=} {lens=} {sum=} tag={tag(sum)}")`;
  let html = header(
    "Nytrix",
    "Manual, specification, and source-linked API reference.",
    "Manual · API · source",
  );
  html += `<div class="docs-home">`;
  html += `<section class="docs-hero"><div class="docs-hero-copy"><div class="docs-eyebrow">${icon("zap", "ico ico-xs")}<span>Overview</span></div><h1>A language for native systems.</h1><p>Explicit imports, compile-time execution, LLVM output, and direct C ABI interop.</p><div class="hero-actions"><a class="hero-action primary" href="https://github.com/nytrix-lang/nytrix" target="_blank" rel="noopener noreferrer">${icon("github", "ico ico-xs")}<span>GitHub</span></a><a class="hero-action" href="${routeHash("learn/start")}" data-select-module="learn/start">${icon("play", "ico ico-xs")}<span>Start</span></a></div><div class="quick-start-list">${quickStart.map(([name, label, command]) => `<div><b>${icon(name, "ico ico-xs")}<span>${esc(label)}</span></b><code>${esc(command)}</code></div>`).join("")}</div></div><div class="docs-hero-aside"><pre class="docs-example"><code>${highlight(sampleProgram)}</code></pre></div></section>`;
  html += renderDocGroup(
    "Learn",
    learnDocs,
    "Everyday workflow.",
  );
  html += renderDocGroup(
    "Spec",
    specDocs,
    "Exact behavior.",
  );
  html += renderDocGroup(
    "Project",
    projectDocs,
    "Repository notes.",
  );
  html += `</div>`;
  setHTML("content", html);
}

function normalizeCodeBlock(code) {
  const raw = String(code ?? "")
    .replace(/\r\n?/g, "\n")
    .replace(/\t/g, "   ");
  let lines = raw.split("\n");
  while (lines.length && !lines[0].trim()) lines.shift();
  while (lines.length && !lines[lines.length - 1].trim()) lines.pop();
  const lineIndent = (line) => (line.match(/^ */) || [""])[0].length;
  const indents = lines.filter((line) => line.trim()).map(lineIndent);
  const common = indents.length ? Math.min(...indents) : 0;
  if (common > 0)
    lines = lines.map((line) => (line.trim() ? line.slice(common) : ""));
  else if (
    lines.length > 1 &&
    lines[0].trim() &&
    lineIndent(lines[0]) === 0 &&
    !/[{[(]\s*$/.test(lines[0])
  ) {
    const tailIndents = lines
      .slice(1)
      .filter((line) => line.trim())
      .map(lineIndent);
    const tailCommon = tailIndents.length ? Math.min(...tailIndents) : 0;
    if (tailCommon > 0)
      lines = [
        lines[0],
        ...lines
          .slice(1)
          .map((line) => (line.trim() ? line.slice(tailCommon) : "")),
      ];
  }
  return lines.join("\n");
}

function codeBox(code) {
  const s = normalizeCodeBlock(code);
  return `<div class="codebox collapsed"><div class="codehd"><button class="code-toggle" data-code-toggle aria-expanded="false">${icon("code", "ico ico-xs")}<span>SHOW CODE</span></button><button class="copy" data-copy="${esc(s)}">${icon("copy", "ico ico-xs")}<span>COPY</span></button></div><pre><code>${highlight(s)}</code></pre></div>`;
}

function sourceBox(mod) {
  const source = String((mod && mod.source) || "");
  if (!source.trim()) return "";
  return `<div class="codebox sourcebox expanded" id="${SOURCE_SYMBOL_ID}" data-module-source><div class="codehd"><span class="source-label">${icon("file", "ico ico-xs")}<span>SOURCE</span></span><button class="copy" data-copy-source>${icon("copy", "ico ico-xs")}<span>COPY SOURCE</span></button></div><pre><code>${highlight(source)}</code></pre></div>`;
}

function setCodeBoxOpen(box, open) {
  const btn = box && box.querySelector("button[data-code-toggle]");
  if (!box || !btn) return;
  box.classList.toggle("expanded", open);
  box.classList.toggle("collapsed", !open);
  const label = btn.dataset.codeLabel || "CODE";
  const ico = label === "SOURCE" ? "file" : "code";
  btn.innerHTML = `${icon(ico, "ico ico-xs")}<span>${open ? `HIDE ${label}` : `SHOW ${label}`}</span>`;
  btn.setAttribute("aria-expanded", open ? "true" : "false");
}

function toggleCode(btn) {
  const box = btn.closest(".codebox");
  if (!box) return;
  setCodeBoxOpen(box, !box.classList.contains("expanded"));
}

function setAllCode(open) {
  document
    .querySelectorAll("#content .codebox")
    .forEach((box) => setCodeBoxOpen(box, open));
}

function setSourceButtonState(btn, open) {
  if (!btn) return;
  btn.innerHTML = `${icon("file", "ico ico-xs")}<span>${open ? "HIDE SOURCE" : "SHOW SOURCE"}</span>`;
  btn.setAttribute("aria-expanded", open ? "true" : "false");
}

function toggleModuleSource(btn) {
  const moduleName =
    btn.dataset.sourceModule || String(current || "").split("::")[0];
  const mod = moduleByName.get(moduleName);
  if (!mod || !mod.source) return;
  const existing = document.querySelector(
    "#content .sourcebox[data-module-source]",
  );
  if (existing) {
    existing.remove();
    setSourceButtonState(btn, false);
    if (current === `${mod.name}::${SOURCE_SYMBOL_ID}`) {
      current = mod.name;
      renderNav();
      setPageTitle(mod.name);
      writeRoute(routeHash(mod.name));
    }
    return;
  }
  const toolbar = btn.closest(".module-toolbar");
  if (!toolbar) return;
  toolbar.insertAdjacentHTML("afterend", cleanHTML(sourceBox(mod)));
  setSourceButtonState(btn, true);
  const box = $(SOURCE_SYMBOL_ID);
  if (box) box.scrollIntoView({ behavior: "smooth", block: "start" });
}

function toggleInternalSymbols(btn) {
  const moduleName =
    btn.dataset.internalModule || String(current || "").split("::")[0];
  const mod = moduleByName.get(moduleName);
  if (!mod) return;
  const next = !showInternalSymbols;
  setInternalSymbolsVisible(next);
  let selected = null;
  if (current && current.startsWith(`${mod.name}::`)) {
    const currentId = current.split("::").slice(1).join("::");
    const sym = selectedSymbol(mod, currentId);
    if (next || (sym && !isInternalSymbol(sym))) selected = currentId;
  }
  selectModule(mod.name, selected);
}

function moduleToolbar(mod, codeCount, sourceOpen = false, internalCount = 0) {
  const sourceAction = mod.source
    ? `<button data-source-module="${esc(mod.name)}" aria-expanded="${sourceOpen ? "true" : "false"}">${icon("file", "ico ico-xs")}<span>${sourceOpen ? "HIDE SOURCE" : "SHOW SOURCE"}</span></button>`
    : "";
  const internalAction =
    internalCount > 0
      ? `<button data-internal-toggle data-internal-module="${esc(mod.name)}" aria-pressed="${showInternalSymbols ? "true" : "false"}">${icon(showInternalSymbols ? "lock" : "dots", "ico ico-xs")}<span>${showInternalSymbols ? "HIDE INTERNALS" : `SHOW INTERNALS (${internalCount})`}</span></button>`
      : "";
  return `<div class="module-toolbar"><div><span>${esc(mod.orig_file || "")}</span></div><div class="module-actions"><button data-route-copy="${esc(routeHash(mod.name))}">${icon("route", "ico ico-xs")}<span>COPY ROUTE</span></button>${internalAction}${sourceAction}${codeCount > 0 ? `<button data-code-action="expand">${icon("code", "ico ico-xs")}<span>EXPAND CODE</span></button><button data-code-action="collapse">${icon("chevron", "ico ico-xs")}<span>COLLAPSE CODE</span></button>` : ""}</div></div>`;
}

function highlightNytrixCode(code) {
  if (!code) return "";
  return highlight(code);
}

function highlightAllCodeBlocks(selector) {
  document.querySelectorAll(`${selector} pre`).forEach((pre) => {
    const codeEl = pre.querySelector("code");
    const raw = normalizeCodeBlock(
      codeEl ? codeEl.textContent : pre.textContent,
    );
    if (!raw.trim()) return;
    const className =
      ((codeEl ? codeEl.className : "") || "") + " " + (pre.className || "");
    const isNy =
      className.includes("language-ny") ||
      className.includes("language-nytrix");
    const hasLanguage = className.includes("language-");
    if (hasLanguage && !isNy && typeof window.hljs !== "undefined" && codeEl) {
      codeEl.textContent = raw;
      window.hljs.highlightElement(codeEl);
      return;
    }
    const html = highlightNytrixCode(raw);
    if (codeEl) codeEl.innerHTML = html;
    else pre.innerHTML = `<code>${html}</code>`;
  });
}

function highlightExampleBlocks(selector) {
  document
    .querySelectorAll(`${selector} .example, ${selector} .smallexample`)
    .forEach((block) => {
      if (block.querySelector("code")) return;
      const raw = normalizeCodeBlock(block.textContent);
      if (!raw.trim()) return;
      const html = highlightNytrixCode(raw);
      block.innerHTML = `<code>${html}</code>`;
    });
}

function highlightInlineCode(selector) {
  document.querySelectorAll(`${selector} code`).forEach((codeEl) => {
    if (codeEl.closest("pre")) return;
    if (
      codeEl.querySelector(".syn-kw, .syn-str, .syn-num, .syn-call, .syn-com")
    )
      return;
    const raw = codeEl.textContent;
    if (!raw.trim()) return;
    if (!codeEl.classList.contains("code")) codeEl.classList.add("code");
    codeEl.innerHTML = highlightNytrixCode(raw);
  });
}

function findAnchor(anchor) {
  const id = String(anchor ?? "").trim();
  if (!id) return null;
  const byId = document.getElementById(id);
  if (byId) return byId;
  return (
    Array.from(document.querySelectorAll("[name]")).find(
      (el) => el.getAttribute("name") === id,
    ) || null
  );
}

function scrollToAnchor(anchor) {
  setTimeout(() => {
    const target = findAnchor(anchor);
    if (target) target.scrollIntoView({ behavior: "smooth", block: "start" });
  }, 60);
}

function hasActiveContentSelection() {
  const selection = window.getSelection ? window.getSelection() : null;
  if (!selection || selection.isCollapsed || selection.rangeCount === 0)
    return false;
  const content = $("content");
  if (!content) return true;
  const range = selection.getRangeAt(0);
  const start = range.startContainer;
  const end = range.endContainer;
  return content.contains(start) || content.contains(end);
}

function visibleMarkdownDocs() {
  return markdownDocsCache.filter(
    (doc) => doc && doc.name !== "NY" && doc.name !== "NYTRIX",
  );
}

function adjacentMarkdownDoc(name, offset) {
  const docs = visibleMarkdownDocs();
  const index = docs.findIndex((doc) => doc.name === name);
  if (index === -1) return null;
  return docs[index + offset] || null;
}

function renderDocPagerLink(doc, direction) {
  if (!doc) return "";
  const isNext = direction === "next";
  const label = isNext ? "Next" : "Previous";
  const chevron = icon("chevron", "ico ico-xs doc-pager-chevron");
  return `<a class="doc-pager-link ${direction}" href="${routeHash(doc.name)}" data-select-module="${esc(doc.name)}"><span class="doc-pager-kicker">${isNext ? "" : chevron}<span>${label}</span>${isNext ? chevron : ""}</span><span class="doc-pager-title">${esc(markdownDocLabel(doc))}</span><span class="doc-pager-route">${esc(doc.name)}</span></a>`;
}

function renderDocPager(docName) {
  const prev = adjacentMarkdownDoc(docName, -1);
  const next = adjacentMarkdownDoc(docName, 1);
  if (!prev && !next) return "";
  return `<nav class="doc-pager" aria-label="Manual page navigation">${renderDocPagerLink(prev, "prev")}${renderDocPagerLink(next, "next")}</nav>`;
}

function buildDocOutlines() {
  const used = new Set();
  document.querySelectorAll(".md-section").forEach((section) => {
    section
      .querySelectorAll(".doc-outline")
      .forEach((outline) => outline.remove());
    const body = section.querySelector(".markdown-content, .html-content");
    if (!body) return;
    const headings = Array.from(body.querySelectorAll("h1, h2, h3, h4")).filter(
      (h) => h.textContent.trim(),
    );
    if (headings.length < 2) return;
    const levels = headings.map((h) => Number(h.tagName.slice(1)));
    const minLevel = Math.min(...levels);
    const links = headings
      .map((h) => {
        const base = h.id ? h.id.trim() : slugifyHeading(h.textContent);
        let id = base || "section";
        let n = 2;
        while (used.has(id)) id = `${base}-${n++}`;
        h.id = id;
        used.add(id);
        const level = Math.min(
          Math.max(Number(h.tagName.slice(1)) - minLevel, 0),
          4,
        );
        return `<a class="doc-outline-link lvl-${level}" href="#${encodeURIComponent(id)}">${esc(h.textContent.trim())}</a>`;
      })
      .join("");
    const outline = document.createElement("div");
    outline.className = "doc-outline";
    outline.innerHTML = `<div class="doc-outline-title">CONTENTS</div>${links}`;
    const title = section.querySelector(".md-title");
    if (title) title.insertAdjacentElement("afterend", outline);
    else section.prepend(outline);
  });
}

function renderOverviewPage(overviewMod, specificDocName = null) {
  if (!specificDocName) {
    renderDocsHome(overviewMod);
    return;
  }
  const selectedDoc = specificDocName
    ? overviewMod.markdown_docs.find((d) => d.name === specificDocName)
    : null;
  const title = selectedDoc
    ? selectedDoc.title || selectedDoc.name
    : specificDocName || overviewMod.name;
  setPageTitle(title);
  let html = header(
    title,
    selectedDoc ? markdownDocSummary(selectedDoc) : overviewMod.module_doc,
    `Manual page`,
    "",
  );
  const docsToRender = specificDocName
    ? overviewMod.markdown_docs.filter((d) => d.name === specificDocName)
    : overviewMod.markdown_docs.filter(
        (d) => d.name !== "NY" && d.name !== "NYTRIX",
      );
  docsToRender.forEach((doc) => {
    const body = DOMPurify.sanitize(
      doc.format === "html" ? doc.html : marked.parse(doc.html),
    );
    const cls = doc.format === "html" ? "html-content" : "markdown-content";
    html += `<div class="md-section" id="md-${esc(doc.name)}" data-doc-route="${esc(doc.name)}"><div class="md-title">${docTitle(doc)}</div><div class="${cls}">${body}</div></div>`;
  });
  if (specificDocName) html += renderDocPager(specificDocName);
  setHTML("content", html);
  rewriteMarkdownDocLinks();
  highlightAllCodeBlocks(".markdown-content");
  highlightAllCodeBlocks(".html-content");
  highlightExampleBlocks(".html-content");
  highlightInlineCode(".markdown-content");
  highlightInlineCode(".html-content");
  buildDocOutlines();
  if (window.MathJax && window.MathJax.typesetPromise)
    window.MathJax.typesetPromise();
}

function renderModuleContent(mod, selectedId = null) {
  const allSyms = mod._sortedSymbols || [];
  const internalCount = moduleInternalSymbolCount(mod);
  const syms = moduleVisibleSymbols(mod);
  const sourceSelected = selectedId === SOURCE_SYMBOL_ID;
  const stats = allSyms.length
    ? internalCount
      ? showInternalSymbols
        ? `${allSyms.length} symbols · ${internalCount} internal shown`
        : `${syms.length} public symbols · ${internalCount} internal hidden`
      : `${allSyms.length} symbols`
    : "Namespace";
  let html = header(mod.name, moduleDocSummary(mod), stats, mod.orig_file);
  html += renderModuleDoc(mod);
  if (allSyms.length || mod.source)
    html += moduleToolbar(
      mod,
      syms.filter((s) => s.code).length,
      sourceSelected,
      internalCount,
    );
  if (mod.source && sourceSelected) html += sourceBox(mod);
  if (!syms.length)
    return (
      html +
      (allSyms.length
        ? `<div class="empty">Internal symbols are hidden.</div>`
        : "")
    );
  html += `<div class="grid">`;
  syms.forEach((s) => {
    const arg = getArgs(s.sig);
    const internal = isInternalSymbol(s);
    html += `<div class="card symbol-card ${internal ? "internal-symbol" : ""}" id="${esc(s.id)}" data-select-module="${esc(mod.name)}" data-select-symbol="${esc(s.id)}"><div class="row"><div class="name">${icon(s.kind === "function" ? "code" : "box", "ico api-card-ico")}<span class="syn-module">${esc(mod.name)}</span> <span class="syn-call">${esc(s.name)}</span>${arg ? `(${esc(arg)})` : ""}</div><div class="kind">${esc(internal ? `internal ${s.kind}` : s.kind)}</div></div><div class="card-body"><div class="doc">${renderRichDocstring(s.doc)}</div>${renderImports(s.imports)}${s.code ? codeBox(s.code) : ""}</div></div>`;
  });
  html += `</div>`;
  return html;
}

function renderModule(name, selectedId = null) {
  const mod = moduleByName.get(name);
  if (!mod) return;
  setPageTitle(
    selectedId === SOURCE_SYMBOL_ID ? `${mod.name} Source` : mod.name,
  );
  setHTML("content", renderModuleContent(mod, selectedId));
  if (window.MathJax && window.MathJax.typesetPromise)
    window.MathJax.typesetPromise();
}

function renderCategoryOverview(categoryPath) {
  setPageTitle(categoryPath);
  const categoryModules = (
    categoryModulesByPath.get(categoryPath) || []
  ).slice();
  if (categoryModules.length === 0) {
    setHTML(
      "content",
      header(categoryPath, "Namespace", "No entries") +
        `<div class="empty">No modules found.</div>`,
    );
    return;
  }
  categoryModules.sort((a, b) => a.name.localeCompare(b.name));
  const prefix = categoryPath + ".";
  const childGroups = new Map();
  const directModules = [];
  categoryModules.forEach((mod) => {
    if (mod.name === categoryPath) return;
    const rest = mod.name.slice(prefix.length);
    const parts = rest.split(".");
    if (parts.length > 1) {
      const groupPath = prefix + parts[0];
      if (!childGroups.has(groupPath)) childGroups.set(groupPath, []);
      childGroups.get(groupPath).push(mod);
    } else directModules.push(mod);
  });
  const groupPaths = new Set(childGroups.keys());
  const visibleDirect = directModules.filter(
    (mod) => !groupPaths.has(mod.name),
  );
  const categoryModule = moduleByName.get(categoryPath);
  const categorySummary =
    API_NAMESPACE_SUMMARIES.get(categoryPath) ||
    (categoryModule
      ? moduleDocSummary(categoryModule)
      : `Open entries under ${categoryPath}.`);
  let html = header(
    categoryPath,
    categorySummary,
    "Namespace",
  );
  if (categoryModule) html += renderModuleDoc(categoryModule);
  if (childGroups.size) {
    html += `<section class="api-section"><div class="docs-section-head"><div><h2>Namespaces</h2><p>Open a child namespace.</p></div><span>Namespace</span></div><div class="api-card-grid">`;
    Array.from(childGroups.entries())
      .sort((a, b) => a[0].localeCompare(b[0]))
      .forEach(([path, modules]) => {
        html += renderNamespaceCard(path, modules);
      });
    html += `</div></section>`;
  }
  if (visibleDirect.length) {
    html += `<section class="api-section"><div class="docs-section-head"><div><h2>Modules</h2><p>Direct modules in this namespace.</p></div><span>Module</span></div><div class="api-card-grid">`;
    visibleDirect.forEach((mod) => {
      html += renderModuleCard(mod);
    });
    html += `</div></section>`;
  }
  setHTML("content", html);
  if (window.MathJax && window.MathJax.typesetPromise)
    window.MathJax.typesetPromise();
}

function renderTagPage(tag) {
  setPageTitle(`Tag / ${tag}`);
  const entry = tagEntries.find((e) => e.tag === tag);
  const modules = entry ? entry.modules : [];
  let html = header(
    `Tag / ${tag}`,
    `Entries marked with ${tag}.`,
    "Tag",
  );
  if (!modules.length) {
    setHTML("content", html + `<div class="empty">No modules found.</div>`);
    return;
  }
  html += `<div class="tag-page-bar"><a href="${routeHash("tags")}" onclick="selectTagIndex();event.preventDefault();">${icon("list", "ico ico-xs")}<span>All tags</span></a><span>Matches</span></div>`;
  html += `<div class="tag-module-grid">`;
  modules.forEach((m) => {
    const summary = moduleDocSummary(m);
    const tagList = m.tags || [];
    const tags = tagList
      .slice(0, 8)
      .map((t) => `<code>${esc(t)}</code>`)
      .join("");
    const more =
      tagList.length > 8 ? `<code>+${tagList.length - 8}</code>` : "";
    html += `<a class="tag-module-card" href="${routeHash(m.name)}" data-select-module="${esc(m.name)}"><span class="tag-module-name">${icon(moduleIconName(m.name), "ico api-card-ico")}<span>${highlightModuleLabel(m.name)}</span></span><p>${esc(summary)}</p><div class="tag-card-tags">${tags}${more}</div></a>`;
  });
  setHTML("content", html + `</div>`);
}

function renderTagIndexPage() {
  setPageTitle("Tags");
  const entries = tagEntriesByWeight();
  let html = header(
    "Tags",
    "Keyword index for API namespaces.",
    "Index",
  );
  if (!entries.length) {
    setHTML("content", html + `<div class="empty">No tags found.</div>`);
    return;
  }
  html += `<div class="tag-index-grid">`;
  entries.forEach((entry) => {
    const preview = entry.modules
      .slice(0, 3)
      .map((m) => m.name)
      .join(" · ");
    html += `<a class="tag-index-card" href="${routeHash("tag", entry.tag)}" data-select-tag="${esc(entry.tag)}"><div class="tag-index-name">${icon("tag", "ico ico-xs")}<span>${esc(entry.tag)}</span></div><div class="tag-index-meta">Tag</div>${preview ? `<p>${esc(preview)}</p>` : ""}</a>`;
  });
  setHTML("content", html + `</div>`);
}

function selectTagIndex(options = {}) {
  current = "Tags";
  renderTagIndexPage();
  renderNav();
  writeRoute(routeHash("tags"), options);
  $("content-area").scrollTo(0, 0);
  const asideElement = $("aside");
  if (
    !isDesktopNav() &&
    asideElement &&
    asideElement.classList.contains("active")
  )
    toggleSidebar();
}

function selectTag(tag, options = {}) {
  tag = normalizeTag(tag);
  current = `tag::${tag}`;
  renderTagPage(tag);
  renderNav();
  writeRoute(routeHash("tag", tag), options);
  $("content-area").scrollTo(0, 0);
  const asideElement = $("aside");
  if (
    !isDesktopNav() &&
    asideElement &&
    asideElement.classList.contains("active")
  )
    toggleSidebar();
}

function selectModule(name, symbolNameOrId = null, options = {}) {
  options = options || {};
  if (!symbolNameOrId && name.includes("::")) {
    const parts = name.split("::");
    name = parts[0].trim();
    symbolNameOrId = parts.slice(1).join("::").trim();
  }
  const safeName = resolveRouteName(name);
  if (!safeName) {
    name = "Overview";
    symbolNameOrId = null;
  } else {
    name = safeName;
    symbolNameOrId = resolveRouteSymbol(name, symbolNameOrId);
  }
  current = name;
  let actualId = symbolNameOrId;

  const overviewMod = moduleByName.get("Overview");
  const isMarkdown = markdownDocByName.has(name);
  const isModule = moduleByName.has(name);
  const hasChildren = categorySet.has(name);

  if (name === "Tags") {
    renderTagIndexPage();
  } else if (name === "Overview" || isMarkdown) {
    renderOverviewPage(overviewMod, isMarkdown ? name : null);
  } else if (hasChildren) {
    renderCategoryOverview(name);
  } else if (isModule) {
    const routeMod = moduleByName.get(name);
    const routeSym = selectedSymbol(routeMod, symbolNameOrId);
    if (routeSym && isInternalSymbol(routeSym)) setInternalSymbolsVisible(true);
    renderModule(name, symbolNameOrId);
    if (symbolNameOrId) {
      const mod = moduleByName.get(name);
      const symbol = selectedSymbol(mod, symbolNameOrId);
      if (symbol) {
        actualId = symbol.id;
        current = `${name}::${symbol.id}`;
      } else if (symbolNameOrId === SOURCE_SYMBOL_ID && mod && mod.source) {
        actualId = SOURCE_SYMBOL_ID;
        current = `${name}::${SOURCE_SYMBOL_ID}`;
      }
    }
  }

  renderNav();

  if (options.scrollAnchor) {
    scrollToAnchor(options.scrollAnchor);
  } else if (symbolNameOrId) {
    setTimeout(() => {
      const symbolElement = $(actualId);
      if (symbolElement)
        symbolElement.scrollIntoView({ behavior: "smooth", block: "start" });
    }, 50);
  } else $("content-area").scrollTo(0, 0);

  writeRoute(routeHash(name, actualId), options);

  const asideElement = $("aside");
  if (
    !isDesktopNav() &&
    asideElement &&
    asideElement.classList.contains("active")
  )
    toggleSidebar();
}

function doSearch() {
  const q = $("search").value.trim().toLowerCase();
  if (!q) {
    if (current && current.startsWith("tag::")) selectTag(current.slice(5));
    else if (current === "Tags") selectTagIndex();
    else if (current) selectModule(current);
    else if (data.length) selectModule(data[0].name);
    return;
  }
  setPageTitle("Search");
  const terms = searchTerms(q);
  const scored = [];
  for (const row of searchRows) {
    if (
      row.type === "symbol" &&
      !showInternalSymbols &&
      isInternalSymbol(row.symbol)
    )
      continue;
    const score = searchScore(row, q, terms);
    if (score >= 0) scored.push({ row, score });
  }
  scored.sort((a, b) => {
    if (b.score !== a.score) return b.score - a.score;
    return String(a.row.name || a.row.mod || "").localeCompare(
      String(b.row.name || b.row.mod || ""),
    );
  });
  const hits = scored.slice(0, SEARCH_LIMIT).map((hit) => hit.row);
  let html = header("Search", `Query: ${q}`, `${hits.length} matches`);
  if (!hits.length) {
    setHTML("content", html + `<div class="empty">No matches found.</div>`);
    return;
  }
  html += `<div class="grid">`;
  hits.forEach((row) => {
    if (row.type === "doc") {
      const route = row.doc.name;
      html += `<a class="card search-doc-card" href="${routeHash(route)}" data-select-module="${esc(route)}"><div class="row"><div class="name">${icon(docIconName(route), "ico api-card-ico")}<span class="syn-module">docs</span> <span class="syn-call">${esc(markdownDocLabel(row.doc))}</span></div><div class="kind">doc</div></div><div class="card-body"><div class="doc">${esc(markdownDocSummary(row.doc))}</div></div></a>`;
      return;
    }
    if (row.type === "module") {
      const mod = moduleByName.get(row.mod);
      html += `<div class="card" data-select-module="${esc(row.mod)}"><div class="row"><div class="name">${icon(moduleIconName(row.mod), "ico api-card-ico")}<span class="syn-module">${esc(row.mod)}</span></div><div class="kind">module</div></div><div class="card-body"><div class="doc">${esc(moduleDocSummary(mod))}</div></div></div>`;
      return;
    }
    const s = row.symbol;
    if (!showInternalSymbols && isInternalSymbol(s)) return;
    const arg = getArgs(s.sig);
    const internal = isInternalSymbol(s);
    html += `<div class="card symbol-card ${internal ? "internal-symbol" : ""}" data-select-module="${esc(row.mod)}" data-select-symbol="${esc(s.id)}"><div class="row"><div class="name">${icon(s.kind === "function" ? "code" : "box", "ico api-card-ico")}<span class="syn-module">${esc(row.mod)}</span> <span class="syn-call">${esc(s.name)}</span>${arg ? `(${esc(arg)})` : ""}</div><div class="kind">${esc(internal ? `internal ${s.kind}` : s.kind)}</div></div><div class="card-body"><div class="doc">${renderRichDocstring(s.doc)}</div>${renderImports(s.imports)}${s.code ? codeBox(s.code) : ""}</div></div>`;
  });
  setHTML("content", html + `</div>`);
}

function scheduleSearch() {
  clearTimeout(searchTimer);
  searchTimer = setTimeout(doSearch, 90);
}

initDataIndexes();
setupDesktopNav();
$("modcount").textContent = "API index";
$("symcount").textContent = "Source linked";

const brandEl = document.querySelector(".brand");
if (brandEl) {
  brandEl.addEventListener("click", (e) => {
    if (e.target.closest(".search")) return;
    goHome(e);
  });
  brandEl.addEventListener("keydown", (e) => {
    if (e.key !== "Enter" && e.key !== " ") return;
    goHome(e);
  });
}

const contentEl = $("content");
if (contentEl) {
  contentEl.addEventListener("click", (e) => {
    const codeBtn = e.target.closest("button[data-code-toggle]");
    if (codeBtn) {
      toggleCode(codeBtn);
      e.preventDefault();
      e.stopPropagation();
      return;
    }
    const sourceBtn = e.target.closest("button[data-source-module]");
    if (sourceBtn) {
      toggleModuleSource(sourceBtn);
      e.preventDefault();
      e.stopPropagation();
      return;
    }
    const internalBtn = e.target.closest("button[data-internal-toggle]");
    if (internalBtn) {
      toggleInternalSymbols(internalBtn);
      e.preventDefault();
      e.stopPropagation();
      return;
    }
    const codeAction = e.target.closest("button[data-code-action]");
    if (codeAction) {
      setAllCode(codeAction.dataset.codeAction === "expand");
      e.preventDefault();
      e.stopPropagation();
      return;
    }
    const routeBtn = e.target.closest("button[data-route-copy]");
    if (routeBtn) {
      copyText(routeBtn.dataset.routeCopy || "", routeBtn);
      e.preventDefault();
      e.stopPropagation();
      return;
    }
    const copyBtn = e.target.closest("button.copy[data-copy]");
    if (copyBtn) {
      copyText(copyBtn.dataset.copy || "", copyBtn);
      e.stopPropagation();
      return;
    }
    const copySourceBtn = e.target.closest("button.copy[data-copy-source]");
    if (copySourceBtn) {
      const box = copySourceBtn.closest(".sourcebox");
      const code = box && box.querySelector("pre code");
      copyText(code ? code.textContent : "", copySourceBtn);
      e.preventDefault();
      e.stopPropagation();
      return;
    }
    if (hasActiveContentSelection()) return;
    const docRouteLink = e.target.closest(
      "a[data-doc-route], button[data-doc-route], [role='link'][data-doc-route]",
    );
    if (docRouteLink) {
      selectModule(docRouteLink.dataset.docRoute || "Overview", null, {
        scrollAnchor: docRouteLink.dataset.docAnchor || null,
      });
      e.preventDefault();
      e.stopPropagation();
      return;
    }
    const localDocAnchor = e.target.closest(
      ".doc-outline-link, .markdown-content a[href^='#'], .html-content a[href^='#']",
    );
    if (localDocAnchor) {
      const anchor =
        localDocAnchor.dataset.docAnchor ||
        decodeRouteHash(
          (localDocAnchor.getAttribute("href") || "").replace(/^#/, ""),
        );
      scrollToAnchor(anchor);
      e.preventDefault();
      e.stopPropagation();
      return;
    }
    const tagLink = e.target.closest("[data-select-tag]");
    if (tagLink) {
      selectTag(tagLink.dataset.selectTag || "");
      e.preventDefault();
      e.stopPropagation();
      return;
    }
    if (e.target.closest(".codebox")) {
      e.stopPropagation();
      return;
    }
    const card = e.target.closest("[data-select-module]");
    if (card) {
      selectModule(
        card.dataset.selectModule || "Overview",
        card.dataset.selectSymbol || null,
      );
      e.preventDefault();
    }
  });
}

function routeFromHash() {
  const h = decodeRouteHash(window.location.hash.slice(1));
  if (!h) {
    selectModule("Overview", null, { preserveHash: true });
    return;
  }
  if (h.trim() === "tags" || h.trim() === "Tags") {
    selectTagIndex({ preserveHash: true });
    return;
  }
  const parts = h.split("::");
  if (parts.length > 1 && parts[0] === "tag") {
    selectTag(parts.slice(1).join("::").trim(), { preserveHash: true });
    return;
  }
  if (parts.length > 1 && resolveRouteName(parts[0].trim())) {
    selectModule(parts[0].trim(), parts.slice(1).join("::").trim(), {
      preserveHash: true,
    });
  } else if (resolveRouteName(h.trim())) {
    selectModule(h.trim(), null, { preserveHash: true });
  } else {
    selectModule("Overview", null, {
      preserveHash: true,
      scrollAnchor: h.trim(),
    });
  }
}

document.addEventListener("keydown", (e) => {
  if (e.key === "Escape") {
    const aside = $("aside");
    if (isDesktopNav()) {
      if (
        aside &&
        document.activeElement &&
        aside.contains(document.activeElement)
      )
        document.activeElement.blur();
      setDesktopNav(false);
    } else if (aside && aside.classList.contains("active")) toggleSidebar();
    e.preventDefault();
    return;
  }
  const targetTag = e.target && e.target.tagName;
  if (
    targetTag === "INPUT" ||
    targetTag === "TEXTAREA" ||
    (e.target && e.target.isContentEditable)
  )
    return;
  if (e.ctrlKey || e.altKey || e.metaKey) return;
  if (e.key.length === 1) {
    const aside = $("aside");
    if (isDesktopNav()) setDesktopNav(true);
    else if (aside && !aside.classList.contains("active")) toggleSidebar();
    const searchInput = $("search");
    if (searchInput) searchInput.focus();
  }
});

window.addEventListener("popstate", routeFromHash);
window.addEventListener("hashchange", routeFromHash);
routeFromHash();
