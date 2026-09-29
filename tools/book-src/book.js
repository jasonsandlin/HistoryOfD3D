/* The History of Direct3D - book interactions (no dependencies, works from file://). */
(function () {
  "use strict";
  var BOOK = window.BOOK || { samples: {}, pages: [], search: [] };
  var $ = function (s, r) { return (r || document).querySelector(s); };
  var $$ = function (s, r) { return Array.prototype.slice.call((r || document).querySelectorAll(s)); };
  var pageId = document.body.getAttribute("data-page");

  /* ---------- Reading progress (visited parts) ---------- */
  var KEY = "d3dbook.visited";
  var visited = {};
  try { visited = JSON.parse(localStorage.getItem(KEY) || "{}"); } catch (e) { visited = {}; }
  if (BOOK.pages.indexOf(pageId + ".html") >= 0) {
    visited[pageId] = 1;
    try { localStorage.setItem(KEY, JSON.stringify(visited)); } catch (e) { /* storage may be disabled */ }
  }
  $$("[data-page]").forEach(function (el) {
    if (el !== document.body && visited[el.getAttribute("data-page")]) el.classList.add("visited");
  });

  /* ---------- Scroll progress + TOC scroll-spy ---------- */
  var bar = $(".progress-bar");
  var tocLinks = $$(".toc a");
  var heads = tocLinks.map(function (a) { return document.getElementById(decodeURIComponent(a.getAttribute("href").slice(1))); });
  function onScroll() {
    var h = document.documentElement;
    var max = h.scrollHeight - h.clientHeight;
    if (bar) bar.style.width = (max > 0 ? (h.scrollTop / max) * 100 : 0) + "%";
    if (!tocLinks.length) return;
    var idx = 0;
    for (var i = 0; i < heads.length; i++) { if (heads[i] && heads[i].getBoundingClientRect().top < 120) idx = i; }
    tocLinks.forEach(function (a, i) { a.classList.toggle("active", i === idx); });
  }
  window.addEventListener("scroll", onScroll, { passive: true });
  onScroll();

  /* ---------- Mobile nav ---------- */
  var menu = $(".menu-btn");
  if (menu) menu.addEventListener("click", function () { document.body.classList.toggle("nav-open"); });

  /* ---------- Copy buttons ---------- */
  function copyText(text, btn) {
    var done = function () { btn.classList.add("done"); var t = btn.textContent; btn.textContent = "Copied"; setTimeout(function () { btn.classList.remove("done"); btn.textContent = t; }, 1300); };
    if (navigator.clipboard && window.isSecureContext) { navigator.clipboard.writeText(text).then(done, fallback); } else { fallback(); }
    function fallback() {
      var ta = document.createElement("textarea"); ta.value = text; ta.style.position = "fixed"; ta.style.opacity = "0";
      document.body.appendChild(ta); ta.select(); try { document.execCommand("copy"); } catch (e) { /* ignore */ } ta.remove(); done();
    }
  }
  document.addEventListener("click", function (e) {
    var b = e.target.closest("button.copy, button.copy-src");
    if (!b) return;
    var box = b.closest(".codeblock, .runbox, .pane");
    var code = box && (b.classList.contains("copy-src") ? box.querySelector("td.code pre, .code pre") : box.querySelector("pre"));
    if (code) copyText(code.innerText, b);
  });

  /* ---------- Animated screenshots (sprite strips) ---------- */
  function play(shot, on) {
    if (!shot || !shot.hasAttribute("data-anim")) return;
    var a = shot.querySelector(".anim");
    if (on && a && !a.style.backgroundImage) {
      a.style.backgroundImage = "url('" + shot.getAttribute("data-anim") + "')";
      shot.style.setProperty("--frames", shot.getAttribute("data-frames") || 12);
      if (shot.hasAttribute("data-dur")) shot.style.setProperty("--dur", shot.getAttribute("data-dur") + "ms");
    }
    shot.classList.toggle("playing", on);
  }
  // Gallery: every card on screen plays by itself, each at a random phase so
  // the wall of cubes doesn't turn in lockstep. Off-screen cards stop.
  var autoplay = pageId === "samples" && "IntersectionObserver" in window;
  var reduceMotion = window.matchMedia && window.matchMedia("(prefers-reduced-motion: reduce)").matches;
  var motionPref = null;
  try { motionPref = localStorage.getItem("d3dbook.autoplay"); } catch (e) { /* ignore */ }
  // An explicit "Play animations" overrides the system's reduce-motion setting
  // (the CSS keeps capture strips still unless body.motion-ok is set).
  document.body.classList.toggle("motion-ok", motionPref === "1");
  var autoOn = false, visibleShots = [];
  if (autoplay) {
    var pref = motionPref;
    autoOn = pref ? pref === "1" : !reduceMotion;
    var shotsIo = new IntersectionObserver(function (entries) {
      entries.forEach(function (en) {
        var s = en.target;
        if (en.isIntersecting) {
          if (visibleShots.indexOf(s) < 0) visibleShots.push(s);
          if (!s.hasAttribute("data-phase")) {
            var dur = +(s.getAttribute("data-dur") || 4000);
            s.setAttribute("data-phase", "1");
            s.querySelector(".anim").style.animationDelay = (-Math.random() * dur).toFixed(0) + "ms";
          }
          if (autoOn) play(s, true);
        } else {
          visibleShots = visibleShots.filter(function (x) { return x !== s; });
          play(s, false);
        }
      });
    }, { rootMargin: "150px 0px" });
    $$(".gal-group .shot[data-anim]").forEach(function (s) { shotsIo.observe(s); });
    var toggle = $(".anim-toggle");
    var syncToggle = function () {
      document.body.classList.toggle("autoplay", autoOn);
      if (autoOn) document.body.classList.add("motion-ok");
      if (!toggle) return;
      toggle.classList.toggle("paused", !autoOn);
      toggle.setAttribute("aria-pressed", autoOn ? "false" : "true");
      toggle.querySelector(".lbl").textContent = autoOn ? "Pause animations" : "Play animations";
      toggle.title = !autoOn && reduceMotion && !pref
        ? "Paused because your system asks for reduced motion (Windows: Settings > Accessibility > Animation effects). Click to play anyway."
        : "";
    };
    syncToggle();
    if (toggle) toggle.addEventListener("click", function () {
      autoOn = !autoOn;
      pref = autoOn ? "1" : "0";
      try { localStorage.setItem("d3dbook.autoplay", pref); } catch (e) { /* ignore */ }
      visibleShots.forEach(function (s) { play(s, autoOn); });
      syncToggle();
    });
  }
  function hoverTarget(e) {
    var host = e.target.closest(".card, .mini, .mosaic-tile, .tl-media, .shot");
    if (!host || host.contains(e.relatedTarget)) return null;
    return host.matches(".shot") ? host : host.querySelector(".shot");
  }
  document.addEventListener("mouseover", function (e) { var s = hoverTarget(e); if (s) play(s, true); });
  document.addEventListener("mouseout", function (e) {
    var s = hoverTarget(e);
    if (s && !(autoOn && visibleShots.indexOf(s) >= 0)) play(s, false);
  });

  /* ---------- Lightbox for the big screenshot ---------- */
  var lb = $(".lightbox");
  document.addEventListener("click", function (e) {
    var s = e.target.closest(".big-shot");
    if (!s || !lb) return;
    var img = s.querySelector("img");
    if (!img) return;
    lb.querySelector("img").src = img.getAttribute("data-full") || img.src;
    lb.querySelector(".lightbox-cap").textContent = img.alt.replace(" screenshot", "");
    lb.hidden = false;
  });
  if (lb) lb.addEventListener("click", function () { lb.hidden = true; });

  /* ---------- Sample hover-cards on inline references ---------- */
  function shotMarkup(name, cls) {
    var s = BOOK.samples[name];
    if (!s || !s.ok) return '<div class="shot noshot ' + (cls || "") + '"><div class="noshot-inner"><div class="noshot-cube"></div><b>No live capture</b></div></div>';
    var anim = s.anim ? ' data-anim="assets/img/' + name + '.anim.jpg" data-frames="' + s.frames + '" data-dur="' + s.ms + '"' : "";
    return '<div class="shot ' + (cls || "") + '"' + anim + '><img src="assets/img/' + name + '.thumb.jpg" alt=""><div class="anim"></div></div>';
  }
  var hc = $(".hovercard");
  document.addEventListener("mouseover", function (e) {
    var a = e.target.closest("a.sample-ref");
    if (!a || !hc || a.contains(e.relatedTarget)) return;
    var name = a.getAttribute("data-sample"), s = BOOK.samples[name];
    if (!s) return;
    hc.innerHTML = shotMarkup(name) + '<div class="hc-body"><b>' + name + '</b><p>' + (s.desc.length > 180 ? s.desc.slice(0, 177) + "…" : s.desc) + "</p></div>";
    var r = a.getBoundingClientRect();
    var left = Math.min(window.innerWidth - 316, Math.max(12, r.left));
    var top = r.bottom + 10;
    hc.hidden = false;
    if (top + hc.offsetHeight > window.innerHeight - 10) top = r.top - hc.offsetHeight - 10;
    hc.style.left = left + "px"; hc.style.top = top + "px";
    play(hc.querySelector(".shot"), true);
  });
  document.addEventListener("mouseout", function (e) {
    var a = e.target.closest("a.sample-ref");
    if (a && hc && !a.contains(e.relatedTarget)) hc.hidden = true;
  });
  window.addEventListener("scroll", function () { if (hc) hc.hidden = true; }, { passive: true });

  /* ---------- Search ---------- */
  var ov = $(".search-overlay"), input = ov && ov.querySelector("input"), results = ov && ov.querySelector(".search-results");
  var sel = 0, hits = [];
  function esc(s) { return s.replace(/[&<>"]/g, function (c) { return { "&": "&amp;", "<": "&lt;", ">": "&gt;", '"': "&quot;" }[c]; }); }
  function openSearch() { if (!ov) return; ov.hidden = false; input.value = ""; render(""); setTimeout(function () { input.focus(); }, 0); }
  function closeSearch() { if (ov) ov.hidden = true; }
  function snippet(text, terms) {
    var low = text.toLowerCase(), pos = -1;
    terms.forEach(function (t) { var p = low.indexOf(t); if (p >= 0 && (pos < 0 || p < pos)) pos = p; });
    var start = Math.max(0, pos - 60), s = (start > 0 ? "…" : "") + text.slice(start, start + 190) + (text.length > start + 190 ? "…" : "");
    s = esc(s);
    terms.forEach(function (t) { if (t.length > 1) s = s.replace(new RegExp("(" + t.replace(/[.*+?^${}()|[\]\\]/g, "\\$&") + ")", "ig"), "<mark>$1</mark>"); });
    return s;
  }
  function render(q) {
    var terms = q.toLowerCase().split(/\s+/).filter(Boolean);
    if (!terms.length) {
      hits = BOOK.pages.map(function (p) { return BOOK.search.filter(function (x) { return x.u === p; })[0]; }).filter(Boolean);
      hits = hits.map(function (h) { return { h: h, s: 0 }; });
    } else {
      hits = [];
      BOOK.search.forEach(function (h) {
        var t = h.t.toLowerCase(), x = h.x.toLowerCase(), score = 0, all = true;
        terms.forEach(function (term) {
          var inT = t.indexOf(term) >= 0, inX = x.indexOf(term) >= 0;
          if (!inT && !inX) all = false;
          score += (inT ? 10 : 0) + (inX ? 2 + Math.min(4, x.split(term).length - 1) : 0);
        });
        if (all) hits.push({ h: h, s: score + (h.k === "sample" ? 3 : 0) });
      });
      hits.sort(function (a, b) { return b.s - a.s; });
    }
    hits = hits.slice(0, 40);
    sel = 0;
    results.innerHTML = hits.length ? hits.map(function (r, i) {
      return '<a class="sr' + (i === 0 ? " sel" : "") + '" href="' + r.h.u + '"><small>' + esc(r.h.p) + "</small><b>" + esc(r.h.t) + "</b><span>" + snippet(r.h.x, terms) + "</span></a>";
    }).join("") : '<div class="search-empty">No matches. Try “fence”, “barrier”, “HLSL” or a sample name.</div>';
  }
  function move(d) {
    var items = $$(".sr", results); if (!items.length) return;
    items[sel].classList.remove("sel"); sel = (sel + d + items.length) % items.length;
    items[sel].classList.add("sel"); items[sel].scrollIntoView({ block: "nearest" });
  }
  if (ov) {
    $(".search-btn").addEventListener("click", openSearch);
    ov.addEventListener("click", function (e) { if (e.target === ov) closeSearch(); });
    input.addEventListener("input", function () { render(input.value); });
    input.addEventListener("keydown", function (e) {
      if (e.key === "ArrowDown") { e.preventDefault(); move(1); }
      else if (e.key === "ArrowUp") { e.preventDefault(); move(-1); }
      else if (e.key === "Enter") { var a = $$(".sr", results)[sel]; if (a) location.href = a.href; }
      else if (e.key === "Escape") closeSearch();
    });
  }

  /* ---------- Keyboard: search, paging, presentation mode ---------- */
  document.addEventListener("keydown", function (e) {
    var typing = /INPUT|TEXTAREA|SELECT/.test((e.target.tagName || "")) || e.target.isContentEditable;
    if ((e.ctrlKey || e.metaKey) && e.key.toLowerCase() === "k") { e.preventDefault(); openSearch(); return; }
    if (typing || e.ctrlKey || e.metaKey || e.altKey) return;
    if (e.key === "Escape") { closeSearch(); if (lb) lb.hidden = true; return; }
    if (e.key === "/") { e.preventDefault(); openSearch(); }
    else if (e.key === "ArrowRight") { var n = $(".pager .next"); if (n) location.href = n.href; }
    else if (e.key === "ArrowLeft") { var p = $(".pager .prev"); if (p) location.href = p.href; }
    else if (e.key === "f" || e.key === "F") {
      document.body.classList.toggle("present");
      try { localStorage.setItem("d3dbook.present", document.body.classList.contains("present") ? "1" : ""); } catch (x) { /* ignore */ }
      if (document.body.classList.contains("present")) { if (document.documentElement.requestFullscreen) document.documentElement.requestFullscreen().catch(function () {}); }
      else if (document.fullscreenElement) document.exitFullscreen();
    }
  });
  try { if (localStorage.getItem("d3dbook.present")) document.body.classList.add("present"); } catch (x) { /* ignore */ }

  /* ---------- Home: timeline, counters, reveal animations ---------- */
  $$(".tl-node").forEach(function (n) {
    n.addEventListener("click", function () {
      var i = n.getAttribute("data-idx");
      $$(".tl-node").forEach(function (x) { x.classList.toggle("active", x === n); });
      $$(".tl-panel").forEach(function (p) { p.classList.toggle("active", p.getAttribute("data-idx") === i); });
    });
  });
  if ($(".timeline")) {
    document.addEventListener("keydown", function (e) {
      if (!/^[1-6]$/.test(e.key) || /INPUT|TEXTAREA|SELECT/.test(e.target.tagName)) return;
      var n = $$(".tl-node")[+e.key - 1]; if (n) n.click();
    });
  }
  function countUp(el) {
    var target = +el.getAttribute("data-count"), t0 = null;
    function step(ts) { if (!t0) t0 = ts; var k = Math.min(1, (ts - t0) / 1100); el.textContent = Math.round(target * (1 - Math.pow(1 - k, 3))).toLocaleString(); if (k < 1) requestAnimationFrame(step); }
    requestAnimationFrame(step);
  }
  if ("IntersectionObserver" in window) {
    var io = new IntersectionObserver(function (entries) {
      entries.forEach(function (en) {
        if (!en.isIntersecting) return;
        en.target.classList.add("in");
        $$("[data-count]", en.target).forEach(countUp);
        io.unobserve(en.target);
      });
    }, { threshold: 0.25 });
    $$(".bars, .trend-fig, .stats").forEach(function (el) { io.observe(el); });
  } else {
    $$(".bars, .trend-fig").forEach(function (el) { el.classList.add("in"); });
  }

  /* ---------- Gallery filters ---------- */
  var gs = $(".gal-search");
  if (gs) {
    var current = "all";
    var apply = function () {
      var q = gs.value.trim().toLowerCase();
      $$(".gal-group").forEach(function (g) {
        var groupOk = current === "all" || g.getAttribute("data-group") === current, any = false;
        $$(".sample-card", g).forEach(function (c) {
          var ok = groupOk && (!q || c.getAttribute("data-name").indexOf(q) >= 0 || c.textContent.toLowerCase().indexOf(q) >= 0);
          c.style.display = ok ? "" : "none"; if (ok) any = true;
        });
        g.style.display = any ? "" : "none";
      });
    };
    $$(".filter").forEach(function (b) {
      b.addEventListener("click", function () {
        current = b.getAttribute("data-filter");
        $$(".filter").forEach(function (x) { x.classList.toggle("active", x === b); });
        apply();
      });
    });
    gs.addEventListener("input", apply);
    var h = decodeURIComponent(location.hash.slice(1));
    var fb = h && $('.filter[data-filter="' + h + '"]');
    if (fb) fb.click();
  }

  /* ---------- Sample page: Run button (d3dbook: protocol) ---------- */
  var toastEl = null, toastTimer = 0;
  function toast(html) {
    if (!toastEl) { toastEl = document.createElement("div"); toastEl.className = "toast"; document.body.appendChild(toastEl); }
    toastEl.innerHTML = html; toastEl.classList.add("show");
    clearTimeout(toastTimer); toastTimer = setTimeout(function () { toastEl.classList.remove("show"); }, 5200);
  }
  document.addEventListener("click", function (e) {
    var r = e.target.closest(".run-btn");
    if (!r || !/^d3dbook:/.test(r.getAttribute("href") || "")) return;
    toast("Launching <b>" + r.getAttribute("data-sample") + "</b>… If nothing opens, run <code>setup.ps1</code> once (in the HistoryOfD3D folder).");
  });
  document.addEventListener("keydown", function (e) {
    if ((e.key === "r" || e.key === "R") && !e.ctrlKey && !e.metaKey && !e.altKey && !/INPUT|TEXTAREA|SELECT/.test(e.target.tagName)) {
      var r = $(".run-btn"); if (r) r.click();
    }
  });

  /* ---------- Sample page source tabs ---------- */
  $$(".tab").forEach(function (t) {
    t.addEventListener("click", function () {
      var i = t.getAttribute("data-tab");
      $$(".tab").forEach(function (x) { x.classList.toggle("active", x === t); });
      $$(".pane").forEach(function (p) { p.classList.toggle("active", p.getAttribute("data-tab") === i); });
    });
  });

  /* ---------- Compare page ---------- */
  var cmp = $(".cmp");
  if (cmp) {
    window.__code = window.__code || {};
    var cols = { a: $('.cmp-col[data-side="a"]'), b: $('.cmp-col[data-side="b"]') };
    var params = new URLSearchParams(location.search);
    var names = { a: params.get("a") || "D3D7.Basic", b: params.get("b") || "D3D12.Basic" };
    if (names.a === names.b) names.b = names.a === "D3D12.Basic" ? "D3D7.Basic" : "D3D12.Basic";
    var load = function (name, cb) {
      if (window.__code[name]) return cb(window.__code[name]);
      var s = document.createElement("script");
      s.src = "assets/code/" + name + ".js";
      s.onload = function () { cb(window.__code[name]); };
      document.head.appendChild(s);
    };
    var draw = function () {
      var la = BOOK.samples[names.a], lb2 = BOOK.samples[names.b];
      var mx = Math.max(la ? la.lines : 1, lb2 ? lb2.lines : 1);
      ["a", "b"].forEach(function (side) {
        var name = names[side], meta = BOOK.samples[name], col = cols[side];
        col.querySelector("select").value = name;
        col.querySelector(".cmp-meta").innerHTML = '<a href="' + meta.url + '">' + shotMarkup(name) + "</a><div><b>" + meta.lines.toLocaleString() +
          ' lines</b><small>' + meta.era + (meta.tier ? " · " + meta.tier.replace("Tier", "Tier ") : "") + " · " + meta.lang +
          '</small><div class="cmp-bar"><i style="width:' + (meta.lines / mx * 100).toFixed(1) + '%"></i></div></div>';
        load(name, function (data) {
          col.querySelector(".cmp-code").innerHTML = '<pre class="hl">' + data.lines.map(function (l) { return '<span class="ln">' + (l || " ") + "</span>"; }).join("") + "</pre>";
        });
      });
      history.replaceState(null, "", "?a=" + encodeURIComponent(names.a) + "&b=" + encodeURIComponent(names.b));
    };
    ["a", "b"].forEach(function (side) {
      cols[side].querySelector("select").addEventListener("change", function (e) { names[side] = e.target.value; draw(); });
    });
    $$(".cmp-presets button").forEach(function (b) {
      b.addEventListener("click", function () { names.a = b.getAttribute("data-a"); names.b = b.getAttribute("data-b"); draw(); });
    });
    var syncing = false, syncBox = $(".sync");
    ["a", "b"].forEach(function (side) {
      var me = cols[side].querySelector(".cmp-code"), other = cols[side === "a" ? "b" : "a"].querySelector(".cmp-code");
      me.addEventListener("scroll", function () {
        if (syncing || !syncBox.checked) return;
        syncing = true;
        var r = me.scrollTop / Math.max(1, me.scrollHeight - me.clientHeight);
        other.scrollTop = r * (other.scrollHeight - other.clientHeight);
        requestAnimationFrame(function () { syncing = false; });
      });
    });
    draw();
  }
})();

/* ---------- Audiobook read-along (pages with window.__narr) ---------- */
(function () {
  "use strict";
  var N = window.__narr, bar = document.querySelector(".player");
  if (!N || !bar) return;
  var audio = bar.querySelector("audio");
  var q = function (s) { return bar.querySelector(s); };
  var seek = q(".pl-seek"), cur = q(".pl-cur"), durEl = q(".pl-dur"), speedBtn = q(".pl-speed"), followBtn = q(".pl-follow"), next = q(".pl-next");
  var dur = Math.max(1, N.end - N.start);
  var blocks = N.blocks, blockEls = {}, wordCache = {};
  Array.prototype.forEach.call(document.querySelectorAll(".prose [data-b]"), function (el) { blockEls[el.getAttribute("data-b")] = el; });
  var byId = {}; blocks.forEach(function (b, i) { byId[b[0]] = i; });
  var store = { get: function (k) { try { return localStorage.getItem(k); } catch (e) { return null; } },
                set: function (k, v) { try { localStorage.setItem(k, v); } catch (e) { /* ignore */ } } };
  var SPEEDS = [1, 1.25, 1.5, 1.75, 2, 0.8];
  var speed = +store.get("d3dbook.speed") || 1;
  var follow = store.get("d3dbook.follow") !== "0";
  var posKey = "d3dbook.pos." + N.page;
  var userScrollAt = 0, raf = 0, curBlock = -1, curWord = -1, curWordEl = null;

  function fmt(s) { s = Math.max(0, Math.floor(s)); return Math.floor(s / 60) + ":" + ("0" + (s % 60)).slice(-2); }
  function wordsOf(id) {
    if (!wordCache[id]) wordCache[id] = blockEls[id] ? Array.prototype.slice.call(blockEls[id].querySelectorAll(".w")) : [];
    return wordCache[id];
  }
  function bsearch(n, get, t) { var lo = 0, hi = n - 1, ans = -1; while (lo <= hi) { var m = (lo + hi) >> 1; if (get(m) <= t) { ans = m; lo = m + 1; } else hi = m - 1; } return ans; }
  function syncUi() {
    speedBtn.textContent = speed + "\u00d7";
    followBtn.classList.toggle("on", follow);
  }
  function clearBlock() {
    if (curBlock >= 0) {
      var el = blockEls[blocks[curBlock][0]];
      if (el) { el.classList.remove("reading"); wordsOf(blocks[curBlock][0]).forEach(function (w) { w.classList.remove("said", "on"); }); }
    }
    curBlock = -1; curWord = -1; curWordEl = null;
  }
  function highlight(t) {
    var bi = bsearch(blocks.length, function (i) { return blocks[i][1]; }, t + 0.02);
    if (bi !== curBlock) {
      clearBlock();
      curBlock = bi;
      if (bi < 0) return;
      var el = blockEls[blocks[bi][0]];
      if (el) {
        el.classList.add("reading");
        if (follow && Date.now() - userScrollAt > 4000) {
          var r = el.getBoundingClientRect();
          if (r.top < 90 || r.bottom > window.innerHeight - 150) el.scrollIntoView({ block: "center", behavior: "smooth" });
        }
      }
    }
    if (bi < 0) return;
    var flat = blocks[bi][3], words = wordsOf(blocks[bi][0]);
    var wi = bsearch(flat.length / 2, function (i) { return flat[i * 2]; }, t + 0.02);
    if (wi === curWord) return;
    if (curWordEl) curWordEl.classList.remove("on");
    if (wi < curWord) words.forEach(function (w) { w.classList.remove("said"); });
    for (var k = Math.max(0, curWord); k < wi && k < words.length; k++) words[k].classList.add("said");
    curWord = wi;
    curWordEl = words[wi] || null;
    if (curWordEl && t <= flat[wi * 2 + 1] + 0.35) curWordEl.classList.add("on");
  }
  function update() {
    var t = audio.currentTime;
    if (t >= N.end - 0.05 && !audio.paused) { audio.pause(); ended(); }
    var r = Math.min(1, Math.max(0, (t - N.start) / dur));
    if (document.activeElement !== seek) seek.value = Math.round(r * 1000);
    cur.textContent = fmt(t - N.start);
    highlight(t);
  }
  function loop() { update(); if (!audio.paused) raf = requestAnimationFrame(loop); }
  function open() {
    bar.hidden = false;
    document.body.classList.add("narrating");
    durEl.textContent = fmt(dur);
    syncUi();
  }
  function playFrom(t) {
    open();
    next.hidden = true;
    t = Math.min(Math.max(t, N.start), N.end - 0.1);
    var go = function () {
      audio.currentTime = t;
      audio.playbackRate = speed;
      var p = audio.play();
      if (p && p.catch) p.catch(function () { bar.classList.add("waiting"); });
      update();
    };
    if (audio.readyState >= 1) go();
    else { audio.preload = "auto"; audio.addEventListener("loadedmetadata", go, { once: true }); audio.load(); }
  }
  function resumeTime() {
    var saved = +store.get(posKey);
    return saved > N.start && saved < N.end - 3 ? saved : N.start;
  }
  function toggle() {
    if (audio.paused) {
      var t = audio.currentTime;
      if (audio.readyState < 1 || t < N.start - 0.5 || t >= N.end - 0.1) playFrom(audio.readyState < 1 ? resumeTime() : N.start);
      else { audio.playbackRate = speed; audio.play(); }
    } else audio.pause();
  }
  function ended() {
    store.set(posKey, "");
    var n = document.querySelector(".pager .next");
    next.innerHTML = "<span>Finished this part.</span>" + (n ? '<a href="' + n.getAttribute("href") + '?listen=1">Continue: ' + n.querySelector("span").textContent + " \u2192</a>" : "");
    next.hidden = false;
  }
  function jump(d) { var t = Math.min(Math.max(audio.currentTime + d, N.start), N.end - 0.1); if (audio.readyState >= 1) { audio.currentTime = t; update(); } }

  audio.addEventListener("play", function () { bar.classList.add("playing"); bar.classList.remove("waiting"); cancelAnimationFrame(raf); raf = requestAnimationFrame(loop); });
  audio.addEventListener("pause", function () { bar.classList.remove("playing"); store.set(posKey, String(audio.currentTime)); update(); });
  audio.addEventListener("seeked", update);
  audio.addEventListener("error", function () { q(".pl-title").textContent = "Audio unavailable - run tools\\narrate_book.py"; });
  setInterval(function () { if (!audio.paused) store.set(posKey, String(audio.currentTime)); }, 2000);

  document.querySelector(".listen-btn") && document.querySelector(".listen-btn").addEventListener("click", function () {
    if (!bar.hidden && !audio.paused) return;
    playFrom(resumeTime());
  });
  q(".pl-play").addEventListener("click", toggle);
  q(".pl-back").addEventListener("click", function () { jump(-15); });
  q(".pl-fwd").addEventListener("click", function () { jump(15); });
  speedBtn.addEventListener("click", function () {
    speed = SPEEDS[(SPEEDS.indexOf(speed) + 1) % SPEEDS.length];
    audio.playbackRate = speed; store.set("d3dbook.speed", String(speed)); syncUi();
  });
  followBtn.addEventListener("click", function () { follow = !follow; store.set("d3dbook.follow", follow ? "1" : "0"); userScrollAt = 0; syncUi(); });
  q(".pl-close").addEventListener("click", function () { audio.pause(); bar.hidden = true; document.body.classList.remove("narrating"); clearBlock(); });
  seek.addEventListener("input", function () {
    var t = N.start + (+seek.value / 1000) * dur;
    cur.textContent = fmt(t - N.start);
    if (audio.readyState >= 1) { audio.currentTime = t; highlight(t); }
  });
  ["wheel", "touchmove"].forEach(function (ev) { window.addEventListener(ev, function () { userScrollAt = Date.now(); }, { passive: true }); });
  document.addEventListener("keydown", function (e) {
    if (/^(PageUp|PageDown|Home|End|ArrowUp|ArrowDown)$/.test(e.key)) userScrollAt = Date.now();
    if (e.ctrlKey || e.metaKey || e.altKey || /INPUT|TEXTAREA|SELECT/.test(e.target.tagName)) return;
    var k = e.key.toLowerCase();
    if (k === "k") { e.preventDefault(); if (bar.hidden) playFrom(resumeTime()); else toggle(); }
    else if (k === "j" && !bar.hidden) jump(-15);
    else if (k === "l" && !bar.hidden) jump(15);
  });
  // Click any word to hear it from there (while the player is open).
  document.addEventListener("click", function (e) {
    if (!document.body.classList.contains("narrating")) return;
    var w = e.target.closest && e.target.closest(".prose [data-b] .w");
    if (!w || w.closest("a")) return;
    var id = w.closest("[data-b]").getAttribute("data-b"), bi = byId[+id];
    if (bi === undefined) return;
    var t = blocks[bi][3][(+w.getAttribute("data-i")) * 2];
    if (t !== undefined) playFrom(t);
  });
  if ("mediaSession" in navigator) {
    navigator.mediaSession.metadata = new MediaMetadata({ title: N.title, artist: "The History of Direct3D", album: "The History of Direct3D",
      artwork: [{ src: "assets/audio/cover.jpg", sizes: "1400x1400", type: "image/jpeg" }] });
    navigator.mediaSession.setActionHandler("play", toggle);
    navigator.mediaSession.setActionHandler("pause", toggle);
    navigator.mediaSession.setActionHandler("seekbackward", function () { jump(-15); });
    navigator.mediaSession.setActionHandler("seekforward", function () { jump(15); });
  }
  // ?listen=1 opens and plays; &t=<seconds into this part> deep-links a moment.
  if (/[?&]listen=1/.test(location.search)) {
    var tm = /[?&]t=([\d.]+)/.exec(location.search);
    playFrom(N.start + (tm ? +tm[1] : 0));
  }
})();
