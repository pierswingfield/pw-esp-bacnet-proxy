/* Controller point mapping: scan -> name match -> confirm.
 *
 * Shared by the setup wizard and the Objects page. The device serves the
 * point definitions (/api/points: labels, name patterns, compatible types,
 * current binding) and the scan catalogue (/api/objects); all matching runs
 * here in the browser so the bridge never needs a regex engine or any RAM
 * beyond the catalogue it already allocates for a scan. Only the confirmed
 * result is POSTed back and stored.
 *
 * Confidence rules (deliberately conservative - a wrong system power point
 * switches the wrong thing):
 *   100  exact name match (case/spacing-insensitive), expected object type
 *    95  exact name match, other compatible type
 *    90  name pattern match, expected type
 *    85  name pattern match, other compatible type
 *  <=75  word similarity only
 * A point is auto-proposed only with a single candidate scoring >= 90.
 * Everything else is shown as "Missing" (with any suggestion offered as a
 * one-click choice), never silently applied.
 */
(function (global) {
  'use strict';

  var CONFIDENT = 90;
  var SUGGEST = 40;

  var TYPE_SHORT = {
    'analog-input': 'AI', 'analog-output': 'AO', 'analog-value': 'AV',
    'binary-input': 'BI', 'binary-output': 'BO', 'binary-value': 'BV',
    'multi-state-input': 'MSI', 'multi-state-output': 'MSO', 'multi-state-value': 'MSV'
  };

  function normalize(s) {
    return String(s || '').toLowerCase().replace(/[_\-]+/g, ' ').replace(/\s+/g, ' ').trim();
  }

  function words(s) {
    return normalize(s).split(/[^a-z0-9%]+/).filter(function (w) { return w.length > 0; });
  }

  /* Dice coefficient over word sets: 1.0 = same words in any order. */
  function similarity(a, b) {
    var wa = words(a), wb = words(b);
    if (!wa.length || !wb.length) return 0;
    var setB = {};
    wb.forEach(function (w) { setB[w] = true; });
    var common = 0, seen = {};
    wa.forEach(function (w) { if (setB[w] && !seen[w]) { common++; seen[w] = true; } });
    var ua = {}, ub = {};
    wa.forEach(function (w) { ua[w] = true; });
    wb.forEach(function (w) { ub[w] = true; });
    return (2 * common) / (Object.keys(ua).length + Object.keys(ub).length);
  }

  function compileRegex(pattern) {
    try { return new RegExp(pattern, 'i'); } catch (e) { return null; }
  }

  function objKey(o) { return o.type + ':' + o.instance; }

  function objLabel(o) {
    return o.name + ' — ' + (TYPE_SHORT[o.type] || o.type) + ' ' + o.instance;
  }

  function scoreCandidate(point, re, obj) {
    if (point.types.indexOf(obj.type) < 0) return null;
    var expectedType = obj.type === point.default.type;
    if (normalize(obj.name) === normalize(point.reference_name)) {
      return { obj: obj, score: expectedType ? 100 : 95, how: 'Exact name' };
    }
    if (re && re.test(normalize(obj.name))) {
      return { obj: obj, score: expectedType ? 90 : 85, how: 'Name pattern' };
    }
    var sim = similarity(point.reference_name, obj.name);
    var score = Math.round(sim * 75) - (expectedType ? 0 : 5);
    if (score <= 0) return null;
    return { obj: obj, score: score, how: 'Similar name' };
  }

  /* Returns {status:'matched'|'missing', best, candidates[], ambiguous}. */
  function matchPoint(point, objects) {
    var re = compileRegex(point.pattern);
    var cands = [];
    for (var i = 0; i < objects.length; i++) {
      var c = scoreCandidate(point, re, objects[i]);
      if (c) cands.push(c);
    }
    cands.sort(function (a, b) { return b.score - a.score; });
    var strong = cands.filter(function (c) { return c.score >= CONFIDENT; });
    var result = { candidates: cands.slice(0, 5), best: cands[0] || null, ambiguous: false };
    if (strong.length === 1 || (strong.length > 1 && strong[0].score > strong[1].score && strong[0].score >= 95)) {
      result.status = 'matched';
    } else {
      result.status = 'missing';
      result.ambiguous = strong.length > 1;
    }
    return result;
  }

  function el(tag, attrs, html) {
    var e = document.createElement(tag);
    if (attrs) Object.keys(attrs).forEach(function (k) {
      if (k === 'class') e.className = attrs[k]; else e.setAttribute(k, attrs[k]);
    });
    if (html !== undefined) e.innerHTML = html;
    return e;
  }

  function esc(s) {
    return String(s === undefined || s === null ? '' : s)
      .replace(/&/g, '&amp;').replace(/</g, '&lt;').replace(/>/g, '&gt;').replace(/"/g, '&quot;');
  }

  var STYLE =
    '.pm-box{border:1px solid #e2e8f0;border-radius:6px;padding:0.9em 1em;background:#fff;margin-top:0.8em;font-size:0.9em;}' +
    '.pm-head{display:flex;justify-content:space-between;align-items:center;gap:0.6em;flex-wrap:wrap;}' +
    '.pm-title{font-weight:bold;color:#0f172a;}' +
    '.pm-note{font-size:0.84em;color:#64748b;line-height:1.4;margin:0.4em 0 0 0;}' +
    '.pm-bar{height:8px;background:#e2e8f0;border-radius:4px;overflow:hidden;margin-top:0.6em;}' +
    '.pm-fill{height:100%;width:0;background:#06c;transition:width .3s;}' +
    '.pm-sum{display:flex;gap:0.6em;flex-wrap:wrap;margin-top:0.7em;}' +
    '.pm-stat{flex:1 1 150px;border:1px solid #e2e8f0;border-radius:6px;padding:0.55em 0.7em;background:#f8fafc;}' +
    '.pm-stat strong{display:block;font-size:1.05em;}' +
    '.pm-stat.good{border-color:#bbf7d0;background:#f0fdf4;} .pm-stat.warn{border-color:#fde68a;background:#fffbeb;} .pm-stat.bad{border-color:#fecaca;background:#fef2f2;}' +
    '.pm-attn{margin-top:0.7em;border-left:4px solid #d97706;background:#fffbeb;padding:0.5em 0.8em;border-radius:4px;font-size:0.86em;color:#78350f;}' +
    '.pm-attn.crit{border-left-color:#dc2626;background:#fef2f2;color:#7f1d1d;}' +
    '.pm-group{font-size:0.78em;text-transform:uppercase;letter-spacing:0.05em;color:#64748b;margin:0.9em 0 0.2em;}' +
    '.pm-row{border:1px solid #e2e8f0;border-radius:6px;padding:0.55em 0.7em;margin-top:0.45em;}' +
    '.pm-row.missing{border-color:#fcd34d;background:#fffdf5;} .pm-row.crit.missing{border-color:#fca5a5;background:#fff8f8;}' +
    '.pm-rhead{display:flex;justify-content:space-between;align-items:baseline;gap:0.5em;}' +
    '.pm-label{font-weight:600;color:#1e293b;}' +
    '.pm-tag{font-size:0.72em;padding:0.15em 0.45em;border-radius:4px;font-weight:bold;white-space:nowrap;}' +
    '.pm-tag.ok{background:#dcfce7;color:#15803d;} .pm-tag.warn{background:#fef3c7;color:#92400e;} .pm-tag.bad{background:#fee2e2;color:#b91c1c;} .pm-tag.info{background:#e0f2fe;color:#0369a1;} .pm-tag.neutral{background:#f1f5f9;color:#475569;}' +
    '.pm-desc{font-size:0.78em;color:#64748b;margin-top:0.15em;line-height:1.35;}' +
    '.pm-row input{width:100%;box-sizing:border-box;padding:0.45em;border:1px solid #cbd5e1;border-radius:4px;font-size:0.88em;margin-top:0.35em;}' +
    '.pm-row input.invalid{border-color:#dc2626;background:#fef2f2;}' +
    '.pm-meta{font-size:0.76em;color:#64748b;margin-top:0.25em;display:flex;gap:0.8em;flex-wrap:wrap;align-items:center;}' +
    '.pm-meta .chg{color:#b45309;font-weight:600;}' +
    '.pm-sugg{font-size:0.78em;margin-top:0.3em;color:#334155;}' +
    '.pm-sugg button,.pm-meta button{font-size:0.95em;padding:0.1em 0.5em;margin-left:0.3em;border:1px solid #cbd5e1;background:#fff;border-radius:4px;cursor:pointer;}' +
    '.pm-actions{display:flex;gap:0.5em;flex-wrap:wrap;margin-top:0.8em;align-items:center;}' +
    '.pm-actions button{padding:0.45em 0.9em;border:1px solid #cbd5e1;background:#fff;border-radius:4px;cursor:pointer;font-size:0.88em;}' +
    '.pm-actions button.primary{background:#06c;color:#fff;border-color:#06c;}' +
    '.pm-msg{font-size:0.84em;min-height:1.2em;margin-top:0.4em;}' +
    '.pm-msg.ok{color:#15803d;} .pm-msg.err{color:#dc2626;}' +
    'details.pm-details>summary{cursor:pointer;color:#06c;font-weight:600;font-size:0.88em;margin-top:0.8em;}';

  function injectStyle() {
    if (document.getElementById('pmStyle')) return;
    var s = document.createElement('style');
    s.id = 'pmStyle';
    s.textContent = STYLE;
    document.head.appendChild(s);
  }

  function getJson(url, opts) {
    return fetch(url, opts).then(function (r) { return r.json(); });
  }

  /* opts: {autoScan:bool, showSave:bool, onChange:fn(summary)} */
  function PointMapper(container, opts) {
    this.c = container;
    this.opts = opts || {};
    this.points = [];
    this.objects = [];
    this.byLabel = {};
    this.byKey = {};
    this.proposal = {};   /* point id -> {mapped, obj, how, score, status, match} */
    this.scanned = false;
    this.truncated = false;
    this.scanError = '';
    injectStyle();
  }

  PointMapper.prototype.start = function () {
    var self = this;
    this.renderShell();
    return getJson('/api/points').then(function (res) {
      if (!res || !res.ok) throw new Error('points unavailable');
      self.points = res.points;
      self.confirmedBefore = !!res.confirmed;
      return getJson('/api/objects');
    }).then(function (cat) {
      if (cat && cat.ok && cat.count > 0 && cat.objects && cat.objects.length) {
        self.useCatalog(cat.objects);
        return;
      }
      if (self.opts.autoScan) return self.scan();
      self.renderIdle();
    }).catch(function () {
      self.renderError('Could not load the point list from the bridge.');
    });
  };

  PointMapper.prototype.renderShell = function () {
    this.c.innerHTML = '';
    this.box = el('div', { 'class': 'pm-box' });
    this.box.appendChild(el('div', { 'class': 'pm-head' },
      '<span class="pm-title">Controller points</span><span class="pm-tag neutral" data-pm="state">Loading…</span>'));
    this.body = el('div');
    this.box.appendChild(this.body);
    this.c.appendChild(this.box);
  };

  PointMapper.prototype.setState = function (text, cls) {
    var t = this.box.querySelector('[data-pm="state"]');
    if (t) { t.textContent = text; t.className = 'pm-tag ' + (cls || 'neutral'); }
  };

  PointMapper.prototype.renderIdle = function () {
    var self = this;
    this.setState(this.confirmedBefore ? 'Configured' : 'Not checked', this.confirmedBefore ? 'ok' : 'neutral');
    this.body.innerHTML =
      '<p class="pm-note">The bridge needs to know which of your controller\'s objects switch the system on/off, ' +
      'run Boost, and report diagnostics. A scan reads every object name on the controller and matches them ' +
      'automatically; you then confirm.</p>';
    var actions = el('div', { 'class': 'pm-actions' });
    var btn = el('button', { type: 'button', 'class': 'primary' }, 'Scan controller &amp; match points');
    btn.onclick = function () { self.scan(); };
    actions.appendChild(btn);
    this.body.appendChild(actions);
  };

  PointMapper.prototype.renderError = function (msg) {
    this.setState('Unavailable', 'bad');
    this.body.innerHTML = '<p class="pm-msg err">' + esc(msg) + '</p>';
  };

  PointMapper.prototype.scan = function () {
    var self = this;
    this.setState('Scanning…', 'info');
    this.body.innerHTML =
      '<p class="pm-note">Reading every object name from the controller. This usually takes one to two minutes; ' +
      'other pages respond slowly until it finishes.</p>' +
      '<div class="pm-bar"><div class="pm-fill" data-pm="fill"></div></div>' +
      '<p class="pm-note" data-pm="prog">Starting scan…</p>';
    return getJson('/api/objects/scan-start', { method: 'POST' }).then(function (res) {
      if (!res || !res.ok) throw new Error((res && res.error) || 'scan could not start');
      return self.pollScan();
    }).catch(function (e) {
      self.scanFailed(e && e.message ? e.message : 'scan failed');
    });
  };

  PointMapper.prototype.pollScan = function () {
    var self = this;
    return new Promise(function (resolve) {
      function tick() {
        getJson('/api/objects/scan-status').then(function (st) {
          var fill = self.body.querySelector('[data-pm="fill"]');
          var prog = self.body.querySelector('[data-pm="prog"]');
          if (fill) fill.style.width = (st.percent || 0) + '%';
          if (prog) prog.textContent = 'Read ' + (st.current || 0) + ' of ' + (st.total || '?') +
            ' objects (' + (st.count || 0) + ' found)';
          if (st.state === 'scanning') { setTimeout(tick, 1500); return; }
          if (st.state === 'error') { self.scanFailed(st.error || 'scan error'); resolve(); return; }
          self.truncated = !!st.truncated;
          getJson('/api/objects').then(function (cat) {
            if (cat && cat.ok && cat.objects && cat.objects.length) {
              self.useCatalog(cat.objects);
              /* The scan refreshed the device's shared catalogue; let the
                 host page re-render anything it built from the old one. */
              if (self.opts.onScanned) self.opts.onScanned(cat.objects);
            } else {
              self.scanFailed('the scan returned no objects');
            }
            resolve();
          });
        }).catch(function () { setTimeout(tick, 2500); });
      }
      setTimeout(tick, 1200);
    });
  };

  PointMapper.prototype.scanFailed = function (msg) {
    var self = this;
    this.scanError = msg;
    this.setState('Not scanned', 'warn');
    this.body.innerHTML =
      '<p class="pm-msg err">Couldn\'t scan the controller: ' + esc(msg) + '.</p>' +
      '<p class="pm-note">' + (this.confirmedBefore
        ? 'The bridge keeps the point mapping you confirmed earlier.'
        : 'The bridge keeps its built-in point map (Delta DAC-1180E program) until you scan.') +
      ' You can retry now, or later from the Objects page.</p>';
    var actions = el('div', { 'class': 'pm-actions' });
    var btn = el('button', { type: 'button' }, 'Retry scan');
    btn.onclick = function () { self.scan(); };
    actions.appendChild(btn);
    this.body.appendChild(actions);
    this.changed();
  };

  PointMapper.prototype.useCatalog = function (objects) {
    var self = this;
    this.scanned = true;
    this.readCount = objects.length;
    this.objects = objects.filter(function (o) { return TYPE_SHORT[o.type]; });
    this.byLabel = {};
    this.byKey = {};
    this.objects.forEach(function (o) { self.byLabel[objLabel(o)] = o; self.byKey[objKey(o)] = o; });
    this.proposal = {};
    this.points.forEach(function (p) {
      var m = matchPoint(p, self.objects);
      self.proposal[p.id] = m.status === 'matched'
        ? { mapped: true, obj: m.best.obj, how: m.best.how, score: m.best.score, auto: true, match: m }
        : { mapped: false, obj: null, how: '', score: 0, auto: true, match: m };
    });
    this.renderProposal();
  };

  PointMapper.prototype.summary = function () {
    var s = { total: 0, matched: 0, control: 0, controlMatched: 0, health: 0, healthMatched: 0,
              missing: [], missingControl: [], scanned: this.scanned, duplicates: [] };
    var seen = {};
    var self = this;
    this.points.forEach(function (p) {
      var pr = self.proposal[p.id];
      var ok = !!(pr && pr.mapped && pr.obj);
      s.total++;
      if (ok) s.matched++;
      if (p.group === 'control') { s.control++; if (ok) s.controlMatched++; else s.missingControl.push(p.label); }
      else { s.health++; if (ok) s.healthMatched++; }
      if (!ok) s.missing.push(p.label);
      if (ok) {
        var k = objKey(pr.obj);
        if (seen[k]) s.duplicates.push(p.label + ' / ' + seen[k]);
        else seen[k] = p.label;
      }
    });
    s.rate = s.total ? Math.round((s.matched * 100) / s.total) : 0;
    return s;
  };

  PointMapper.prototype.changed = function () {
    if (this.opts.onChange) this.opts.onChange(this.summary());
  };

  PointMapper.prototype.currentText = function (p) {
    if (!p.mapped) return p.source === 'unmapped' ? 'not present' : 'none';
    var o = this.byKey[p.type + ':' + p.instance];
    var t = (TYPE_SHORT[p.type] || p.type) + ' ' + p.instance;
    return (o ? o.name + ' — ' : '') + t + (p.source === 'default' ? ' (built-in)' : '');
  };

  PointMapper.prototype.renderProposal = function () {
    var self = this;
    var s = this.summary();
    this.setState(s.missing.length ? (s.missing.length + ' need attention') : 'All matched',
                  s.missingControl.length ? 'bad' : (s.missing.length ? 'warn' : 'ok'));

    var html = '<p class="pm-note">Matched by name against the ' + this.objects.length +
      ' input/output/value objects among the ' + this.readCount + ' read from your controller' + (this.truncated
        ? ' <strong class="bad">(scan stopped at the catalogue limit - some objects were not read)</strong>' : '') +
      '. Review anything marked below, then confirm.</p>';
    html += '<div class="pm-sum">' +
      '<div class="pm-stat ' + (s.controlMatched === s.control ? 'good' : 'bad') + '">Essential controls<strong>' +
        s.controlMatched + ' of ' + s.control + ' matched</strong><span class="pm-desc">System power &amp; Boost</span></div>' +
      '<div class="pm-stat ' + (s.healthMatched === s.health ? 'good' : 'warn') + '">Diagnostics<strong>' +
        s.healthMatched + ' of ' + s.health + ' matched</strong><span class="pm-desc">Health page read-outs</span></div>' +
      '<div class="pm-stat ' + (s.rate === 100 ? 'good' : (s.rate >= 80 ? 'warn' : 'bad')) + '">Match rate<strong>' +
        s.rate + '%</strong><span class="pm-desc">' + s.matched + ' of ' + s.total + ' points</span></div>' +
      '</div>';
    if (s.missingControl.length) {
      html += '<div class="pm-attn crit"><strong>Needs your attention:</strong> ' + esc(s.missingControl.join(', ')) +
        ' could not be matched confidently. Until you pick the right object, the matching on/off or Boost ' +
        'control (dashboard, Home Assistant and Matter) will not work.</div>';
    }
    if (s.missing.length > s.missingControl.length) {
      html += '<div class="pm-attn"><strong>Missing diagnostics:</strong> ' +
        esc(s.missing.filter(function (m) { return s.missingControl.indexOf(m) < 0; }).join(', ')) +
        '. These just show "–" on the Health page if left unmapped.</div>';
    }
    if (s.duplicates.length) {
      html += '<div class="pm-attn"><strong>Same object used twice:</strong> ' + esc(s.duplicates.join('; ')) + '.</div>';
    }
    this.body.innerHTML = html;

    var details = el('details', { 'class': 'pm-details' });
    if (s.missing.length || s.duplicates.length || this.opts.expanded) details.open = true;
    details.appendChild(el('summary', null, 'Review or change individual points'));
    ['control', 'health'].forEach(function (group) {
      details.appendChild(el('div', { 'class': 'pm-group' }, group === 'control' ? 'Essential controls' : 'Diagnostics (Health page)'));
      self.points.filter(function (p) { return p.group === group; }).forEach(function (p) {
        details.appendChild(self.renderRow(p));
      });
    });
    this.body.appendChild(details);

    /* Datalists: one per point, compatible object types only. */
    this.points.forEach(function (p) {
      var dl = el('datalist', { id: 'pmList_' + p.id });
      var opts = self.objects.filter(function (o) { return p.types.indexOf(o.type) >= 0; })
        .map(function (o) { return '<option value="' + esc(objLabel(o)) + '"></option>'; });
      dl.innerHTML = opts.join('');
      self.body.appendChild(dl);
    });

    var actions = el('div', { 'class': 'pm-actions' });
    var rescan = el('button', { type: 'button' }, 'Rescan controller');
    rescan.onclick = function () { self.scan(); };
    actions.appendChild(rescan);
    if (this.opts.showSave) {
      var save = el('button', { type: 'button', 'class': 'primary' }, 'Confirm &amp; save mapping');
      save.onclick = function () { self.save(); };
      actions.appendChild(save);
    }
    this.body.appendChild(actions);
    this.msg = el('div', { 'class': 'pm-msg' });
    this.body.appendChild(this.msg);
    this.changed();
  };

  PointMapper.prototype.renderRow = function (p) {
    var self = this;
    var pr = this.proposal[p.id];
    var ok = pr.mapped && pr.obj;
    var row = el('div', { 'class': 'pm-row' + (ok ? '' : ' missing') + (p.group === 'control' ? ' crit' : '') });
    var tag;
    if (ok && pr.auto) tag = '<span class="pm-tag ok">' + esc(pr.how) + ' · ' + pr.score + '%</span>';
    else if (ok) tag = '<span class="pm-tag info">Chosen by you</span>';
    else if (pr.auto) tag = '<span class="pm-tag ' + (p.group === 'control' ? 'bad' : 'warn') + '">Missing</span>';
    else tag = '<span class="pm-tag neutral">Not present</span>';
    row.appendChild(el('div', { 'class': 'pm-rhead' }, '<span class="pm-label">' + esc(p.label) + '</span>' + tag));
    row.appendChild(el('div', { 'class': 'pm-desc' }, esc(p.description) +
      ' Usually called <em>' + esc(p.reference_name) + '</em>.'));

    var input = el('input', { type: 'text', list: 'pmList_' + p.id, autocomplete: 'off',
                              placeholder: 'Search your controller\'s objects (' +
                                p.types.map(function (t) { return TYPE_SHORT[t]; }).join('/') + ')…' });
    input.value = ok ? objLabel(pr.obj) : '';
    input.onchange = function () {
      var v = input.value.trim();
      if (!v) { self.proposal[p.id] = { mapped: false, obj: null, auto: false, match: pr.match }; self.renderProposal(); return; }
      var o = self.byLabel[v];
      if (!o) { input.className = 'invalid'; return; }
      self.proposal[p.id] = { mapped: true, obj: o, how: 'Chosen', score: 0, auto: false, match: pr.match };
      self.renderProposal();
    };
    row.appendChild(input);

    var cur = this.currentText(p);
    var next = ok ? objLabel(pr.obj) : 'not present';
    var same = (ok && p.mapped && p.type === pr.obj.type && p.instance === pr.obj.instance) || (!ok && !p.mapped);
    var meta = el('div', { 'class': 'pm-meta' }, '<span>In use now: ' + esc(cur) + '</span>' +
      (same ? '' : '<span class="chg">→ will change to: ' + esc(next) + '</span>'));
    if (ok) {
      var clear = el('button', { type: 'button', title: 'This controller has no such point' }, 'Not present');
      clear.onclick = function () { self.proposal[p.id] = { mapped: false, obj: null, auto: false, match: pr.match }; self.renderProposal(); };
      meta.appendChild(clear);
    }
    row.appendChild(meta);

    if (!ok && pr.match) {
      var best = pr.match.candidates.filter(function (c) { return c.score >= SUGGEST; }).slice(0, 2);
      if (best.length) {
        var sugg = el('div', { 'class': 'pm-sugg' }, (pr.match.ambiguous ? 'Several objects fit - pick one: ' : 'Possible match: '));
        best.forEach(function (c) {
          var b = el('button', { type: 'button' }, esc(objLabel(c.obj)) + ' (' + c.score + '%)');
          b.onclick = function () {
            self.proposal[p.id] = { mapped: true, obj: c.obj, how: 'Chosen', score: c.score, auto: false, match: pr.match };
            self.renderProposal();
          };
          sugg.appendChild(b);
        });
        row.appendChild(sugg);
      }
    }
    return row;
  };

  /* Body for POST /api/points (also embedded in the wizard's finish payload). */
  PointMapper.prototype.payload = function () {
    if (!this.scanned) return null;
    var self = this;
    return this.points.map(function (p) {
      var pr = self.proposal[p.id];
      return pr && pr.mapped && pr.obj
        ? { id: p.id, mapped: true, type: pr.obj.type, instance: pr.obj.instance }
        : { id: p.id, mapped: false };
    });
  };

  PointMapper.prototype.save = function () {
    var self = this;
    var body = this.payload();
    if (!body) return Promise.resolve(false);
    var s = this.summary();
    if (s.missingControl.length &&
        !window.confirm(s.missingControl.join(', ') + ' will be left unmapped, so that control will not work. Save anyway?')) {
      return Promise.resolve(false);
    }
    this.msg.className = 'pm-msg';
    this.msg.textContent = 'Saving…';
    return getJson('/api/points', { method: 'POST', headers: { 'Content-Type': 'application/json' },
                                    body: JSON.stringify({ points: body }) }).then(function (res) {
      if (!res || !res.ok) throw new Error((res && res.error) || 'save failed');
      return getJson('/api/points');
    }).then(function (res) {
      self.points = res.points;
      self.confirmedBefore = true;
      self.renderProposal();
      self.msg.className = 'pm-msg ok';
      self.msg.textContent = '✓ Saved. The bridge is using this mapping now.';
      return true;
    }).catch(function (e) {
      self.msg.className = 'pm-msg err';
      self.msg.textContent = '✗ ' + (e && e.message ? e.message : 'Save failed');
      return false;
    });
  };

  global.PointMapper = PointMapper;
  global.PointMapper.matchPoint = matchPoint; /* exposed for tests */
})(typeof window !== 'undefined' ? window : this);
