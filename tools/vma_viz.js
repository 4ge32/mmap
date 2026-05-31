/*
 * vma_viz.js - Interactive SVG visualizer for the mmap/ld.so simulator.
 *
 * Zero-dependency vanilla JS + inline SVG (no build step, no CDN). Renders an
 * address space as a horizontal track of VMA boxes and steps through a
 * scenario (mmap / mprotect / munmap operations) with prev/next/play controls,
 * so the design docs can be explored visually.
 *
 * This is a *library*: each scenario has a stable `id` slug and is mounted as
 * an independent inline widget via window.VmaViz.mount / mountAll. The Docs
 * pages embed it next to the prose (gen_dashboard.py turns a ```vma-viz <id>```
 * fence into a <div data-scenario="<id>"> mount point, then calls mountAll).
 *
 * Scenarios are plain data (see SCENARIOS below): each has an `id`, a `span`,
 * and steps with a `note` plus VMAs {start, end, prot, backing, label}.
 * Addresses are in pages. The design-visualizer agent keeps these in sync with
 * docs/ + tests/.
 */
(function () {
  "use strict";

  var PAGE = 1; // scenario coordinates are in pages

  // Protection -> label/colour. PROT_NONE=0, R=1, W=2, X=4.
  function protStr(p) {
    return (p & 1 ? "r" : "-") + (p & 2 ? "w" : "-") + (p & 4 ? "x" : "-");
  }
  function protFill(p, backing) {
    if (p === 0) return "#e5e7eb";            // none: grey (reservation)
    if (backing === "file") {
      if (p & 4) return "#bfdbfe";            // text r-x: blue
      if (p & 2) return "#fde68a";            // data rw-: amber
      return "#c7f9cc";                       // file r--: green (relro)
    }
    return p & 2 ? "#fbcfe8" : "#ddd6fe";     // anon rw / other: pink / violet
  }

  // ---- scenarios (pages) ------------------------------------------------
  // ld.so synthetic example mirrors docs/ldso_sequence.md (7-page image).
  var SCENARIOS = {
    "ld.so shared-object mapping": {
      id: "ldso-mapping",
      span: [0, 7],
      steps: [
        { note: "1. Whole-object PROT_NONE reservation of the 7-page image span.",
          vmas: [{ start: 0, end: 7, prot: 0, backing: "anon", label: "reservation" }] },
        { note: "2. MAP_FIXED text segment [0,2) as r-x, file-backed (overlays the reservation).",
          vmas: [
            { start: 0, end: 2, prot: 5, backing: "file", label: "text r-x" },
            { start: 2, end: 7, prot: 0, backing: "anon", label: "reservation" }] },
        { note: "3. MAP_FIXED data file part [3,5) as rw-, file-backed. The [2,3) alignment gap stays PROT_NONE.",
          vmas: [
            { start: 0, end: 2, prot: 5, backing: "file", label: "text r-x" },
            { start: 2, end: 3, prot: 0, backing: "anon", label: "gap" },
            { start: 3, end: 5, prot: 3, backing: "file", label: "data rw-" },
            { start: 5, end: 7, prot: 0, backing: "anon", label: "reservation" }] },
        { note: "4. bss: MAP_FIXED anonymous rw- pages [5,7) beyond the file content.",
          vmas: [
            { start: 0, end: 2, prot: 5, backing: "file", label: "text r-x" },
            { start: 2, end: 3, prot: 0, backing: "anon", label: "gap" },
            { start: 3, end: 5, prot: 3, backing: "file", label: "data rw-" },
            { start: 5, end: 7, prot: 3, backing: "anon", label: "bss rw-" }] },
        { note: "5. GNU_RELRO: mprotect [3,4) to read-only — splits the data segment. Final canonical layout (5 VMAs).",
          vmas: [
            { start: 0, end: 2, prot: 5, backing: "file", label: "text r-x" },
            { start: 2, end: 3, prot: 0, backing: "anon", label: "gap" },
            { start: 3, end: 4, prot: 1, backing: "file", label: "relro r--" },
            { start: 4, end: 5, prot: 3, backing: "file", label: "data rw-" },
            { start: 5, end: 7, prot: 3, backing: "anon", label: "bss rw-" }] }
      ]
    },
    "MAP_FIXED overlay (split + fill)": {
      id: "map-fixed-overlay",
      span: [0, 8],
      steps: [
        { note: "Start: one 8-page PROT_NONE reservation.",
          vmas: [{ start: 0, end: 8, prot: 0, backing: "anon", label: "reservation" }] },
        { note: "mmap MAP_FIXED [2,4) r-x: splits the reservation and fills the hole.",
          vmas: [
            { start: 0, end: 2, prot: 0, backing: "anon", label: "none" },
            { start: 2, end: 4, prot: 5, backing: "file", label: "r-x" },
            { start: 4, end: 8, prot: 0, backing: "anon", label: "none" }] }
      ]
    },
    "mprotect sub-range (split + merge)": {
      id: "mprotect-split-merge",
      span: [0, 4],
      steps: [
        { note: "Start: one 4-page rw- anonymous mapping.",
          vmas: [{ start: 0, end: 4, prot: 3, backing: "anon", label: "rw-" }] },
        { note: "mprotect [1,2) to r--: splits into three VMAs (differing prot prevents merge).",
          vmas: [
            { start: 0, end: 1, prot: 3, backing: "anon", label: "rw-" },
            { start: 1, end: 2, prot: 1, backing: "anon", label: "r--" },
            { start: 2, end: 4, prot: 3, backing: "anon", label: "rw-" }] },
        { note: "mprotect [1,2) back to rw-: now all three are compatible and canonicalize merges them.",
          vmas: [{ start: 0, end: 4, prot: 3, backing: "anon", label: "rw-" }] }
      ]
    },
    "munmap punches a hole": {
      id: "munmap-hole",
      span: [0, 6],
      steps: [
        { note: "Start: one 6-page r-- mapping.",
          vmas: [{ start: 0, end: 6, prot: 1, backing: "anon", label: "r--" }] },
        { note: "munmap [2,4): splits at the boundaries and removes the covered VMAs, leaving a gap.",
          vmas: [
            { start: 0, end: 2, prot: 1, backing: "anon", label: "r--" },
            { start: 4, end: 6, prot: 1, backing: "anon", label: "r--" }] }
      ]
    }
  };

  // ---- rendering --------------------------------------------------------
  var SVGNS = "http://www.w3.org/2000/svg";
  function el(tag, attrs, text) {
    var n = document.createElementNS(SVGNS, tag);
    for (var k in attrs) n.setAttribute(k, attrs[k]);
    if (text != null) n.textContent = text;
    return n;
  }

  function renderTrack(svg, scen, vmas) {
    while (svg.firstChild) svg.removeChild(svg.firstChild);
    var span = scen.span, lo = span[0], hi = span[1];
    var W = 880, H = 140, padX = 30, padTop = 30, trackH = 60;
    var usable = W - 2 * padX;
    var pxPerPage = usable / (hi - lo);
    svg.setAttribute("viewBox", "0 0 " + W + " " + H);

    // baseline track
    svg.appendChild(el("rect", { x: padX, y: padTop, width: usable, height: trackH,
      fill: "#f3f4f6", stroke: "#d1d5db", rx: 4 }));

    // page gridlines + address ticks
    for (var pg = lo; pg <= hi; pg++) {
      var x = padX + (pg - lo) * pxPerPage;
      svg.appendChild(el("line", { x1: x, y1: padTop, x2: x, y2: padTop + trackH,
        stroke: "#e5e7eb" }));
      svg.appendChild(el("text", { x: x, y: padTop + trackH + 16, "text-anchor": "middle",
        "font-size": 11, fill: "#6b7280" }, pg + "P"));
    }

    // VMA boxes
    vmas.forEach(function (v) {
      var x = padX + (v.start - lo) * pxPerPage;
      var w = (v.end - v.start) * pxPerPage;
      var g = el("g", {});
      g.appendChild(el("rect", { x: x + 1, y: padTop + 1, width: Math.max(w - 2, 1),
        height: trackH - 2, fill: protFill(v.prot, v.backing), stroke: "#374151", rx: 3 }));
      var cx = x + w / 2;
      g.appendChild(el("text", { x: cx, y: padTop + 24, "text-anchor": "middle",
        "font-size": 12, "font-weight": 600, fill: "#111827" }, v.label));
      g.appendChild(el("text", { x: cx, y: padTop + 42, "text-anchor": "middle",
        "font-size": 11, fill: "#374151", "font-family": "ui-monospace, monospace" },
        protStr(v.prot) + " " + v.backing));
      svg.appendChild(g);
    });
  }

  function legend(container) {
    var items = [
      ["PROT_NONE (reservation/gap)", "#e5e7eb"],
      ["text r-x (file)", "#bfdbfe"],
      ["data rw- (file)", "#fde68a"],
      ["relro r-- (file)", "#c7f9cc"],
      ["anon rw-", "#fbcfe8"]
    ];
    var box = document.createElement("div");
    box.className = "vmaviz-legend";
    items.forEach(function (it) {
      var s = document.createElement("span");
      s.className = "vmaviz-key";
      s.innerHTML = '<i style="background:' + it[1] + '"></i>' + it[0];
      box.appendChild(s);
    });
    container.appendChild(box);
  }

  // Resolve a scenario by its stable `id` slug.
  function findScenario(id) {
    var names = Object.keys(SCENARIOS);
    for (var i = 0; i < names.length; i++) {
      if (SCENARIOS[names[i]].id === id) return SCENARIOS[names[i]];
    }
    return null;
  }

  function button(label) {
    var b = document.createElement("button");
    b.type = "button"; b.textContent = label; b.className = "vmaviz-btn";
    return b;
  }

  // Mount ONE fixed scenario's stepper into `container`. Idempotent: any prior
  // widget (DOM + running Play timer) in the container is torn down first, so
  // repeated mounts (the Docs page re-mounts on every client-side navigation)
  // never duplicate controls or leak intervals.
  function mount(container, scenarioId) {
    if (!container) return;

    // Tear down any prior widget in this container.
    if (container._vmaViz && container._vmaViz.timer) {
      clearInterval(container._vmaViz.timer);
    }
    while (container.firstChild) container.removeChild(container.firstChild);
    container._vmaViz = { timer: null };

    var scen = findScenario(scenarioId);
    if (!scen) {
      var err = document.createElement("p");
      err.className = "vmaviz-note";
      err.textContent = "Unknown scenario: " + scenarioId;
      container.appendChild(err);
      return;
    }

    var state = { step: 0 };

    var controls = document.createElement("div");
    controls.className = "vmaviz-controls";
    var prev = button("◀ Prev"), play = button("▶ Play"), next = button("Next ▶");
    controls.appendChild(prev);
    controls.appendChild(play);
    controls.appendChild(next);

    var svg = document.createElementNS(SVGNS, "svg");
    svg.setAttribute("class", "vmaviz-svg");
    svg.setAttribute("role", "img");

    var note = document.createElement("p");
    note.className = "vmaviz-note";
    var counter = document.createElement("span");
    counter.className = "vmaviz-counter";

    container.appendChild(controls);
    container.appendChild(svg);
    legend(container);
    var foot = document.createElement("div");
    foot.className = "vmaviz-foot";
    foot.appendChild(counter);
    foot.appendChild(note);
    container.appendChild(foot);

    function draw() {
      var step = scen.steps[state.step];
      renderTrack(svg, scen, step.vmas);
      note.textContent = step.note;
      counter.textContent = "Step " + (state.step + 1) + " / " + scen.steps.length;
      prev.disabled = state.step === 0;
      next.disabled = state.step === scen.steps.length - 1;
    }
    function stop() {
      if (container._vmaViz.timer) {
        clearInterval(container._vmaViz.timer);
        container._vmaViz.timer = null;
        play.textContent = "▶ Play";
      }
    }

    prev.addEventListener("click", function () { stop(); if (state.step > 0) { state.step--; draw(); } });
    next.addEventListener("click", function () { stop(); if (state.step < scen.steps.length - 1) { state.step++; draw(); } });
    play.addEventListener("click", function () {
      if (container._vmaViz.timer) { stop(); return; }
      play.textContent = "⏸ Pause";
      container._vmaViz.timer = setInterval(function () {
        if (state.step < scen.steps.length - 1) { state.step++; draw(); }
        else stop();
      }, 1400);
    });

    draw();
  }

  // Find every [data-scenario] container under `root` and mount it. Idempotent.
  function mountAll(root) {
    var scope = root || document;
    var nodes = scope.querySelectorAll("[data-scenario]");
    for (var i = 0; i < nodes.length; i++) {
      mount(nodes[i], nodes[i].getAttribute("data-scenario"));
    }
  }

  // Expose the library so the Docs page can mount widgets after rendering.
  window.VmaViz = { mount: mount, mountAll: mountAll, scenarios: SCENARIOS };

  // Mount any server-rendered containers present at load.
  document.addEventListener("DOMContentLoaded", function () { mountAll(document); });
})();
