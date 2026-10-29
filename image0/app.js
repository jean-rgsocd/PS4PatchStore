/* PS4 Patch Store - filtro do catalogo, instalacao de PKG e navegacao por controle. ES5 para o WebKit do PS4. */
/* Remote Package Installer (homebrew do PS4) escuta aqui no proprio console: */
var RPI_ENDPOINT = "http://127.0.0.1:12800/api/install";

var searchInput = document.getElementById("search");
var categorySelect = document.getElementById("category");
var cards = Array.prototype.slice.call(document.querySelectorAll(".game-card"));
var resultCount = document.getElementById("result-count");
var emptyState = document.getElementById("empty-state");
var toast = document.getElementById("toast");
var toastTimer = null;

function filterCatalog() {
  var query = (searchInput.value || "").replace(/^\s+|\s+$/g, "").toLowerCase();
  var category = categorySelect.value;
  var visible = 0;
  for (var i = 0; i < cards.length; i++) {
    var card = cards[i];
    var name = (card.getAttribute("data-name") || "").toLowerCase();
    var cardCategory = card.getAttribute("data-category") || "";
    var show = name.indexOf(query) !== -1 && (category === "all" || cardCategory === category);
    card.hidden = !show;
    if (show) visible += 1;
  }
  resultCount.textContent = visible + " " + (visible === 1 ? "coleção" : "coleções");
  emptyState.hidden = visible !== 0;
}

function showToast(msg) {
  toast.textContent = msg;
  toast.hidden = false;
  if (toastTimer) clearTimeout(toastTimer);
  toastTimer = setTimeout(function () { toast.hidden = true; }, 6000);
}

function installPkg(url) {
  showToast("Enviando pedido de instalação ao PS4...");
  var xhr = new XMLHttpRequest();
  xhr.open("POST", RPI_ENDPOINT, true);
  /* text/plain evita o preflight (OPTIONS) de CORS, que o instalador original nao responde. */
  xhr.setRequestHeader("Content-Type", "text/plain;charset=UTF-8");
  xhr.timeout = 15000;
  xhr.onload = function () {
    var r = null;
    try { r = JSON.parse(xhr.responseText); } catch (e) {}
    if (r && r.status === "success") {
      showToast("Instalação iniciada. Acompanhe em Notificações > Downloads.");
    } else if (r && r.status) {
      showToast("O instalador respondeu: " + (r.error || r.error_code || r.status));
    } else {
      showToast("Pedido enviado. Veja as Notificações do PS4.");
    }
  };
  /* Erro de rede pode ser so a resposta bloqueada por CORS: o pedido pode ter chegado. */
  xhr.onerror = function () {
    showToast("Pedido enviado, sem confirmação. Veja as Notificações; se nada aparecer, abra o Remote Package Installer e tente de novo.");
  };
  xhr.ontimeout = function () {
    showToast("O instalador não respondeu. Ele precisa estar aberto no PS4 (porta 12800).");
  };
  xhr.send(JSON.stringify({ type: "direct", packages: [url] }));
}

function onDownloadClick(e) {
  e.preventDefault();
  var url = this.getAttribute("data-pkg");
  if (!url) { showToast("URL do patch não configurada (data-pkg no index.html)."); return; }
  installPkg(url);
}

var buttons = document.querySelectorAll("a[data-pkg]");
for (var b = 0; b < buttons.length; b++) buttons[b].addEventListener("click", onDownloadClick);

searchInput.addEventListener("input", filterCatalog);
categorySelect.addEventListener("change", filterCatalog);
document.getElementById("year").textContent = new Date().getFullYear();
filterCatalog();

/* ---------- Navegacao por controle / teclado ---------- */
var focusStyle = document.createElement("style");
focusStyle.appendChild(document.createTextNode(
  "a:focus, #search:focus, #category:focus { outline: 3px solid #ffc857; outline-offset: 3px; }" +
  ".download-button:focus, .primary-button:focus { background: #75f4c0; }"));
document.head.appendChild(focusStyle);

function focusables() {
  var all = document.querySelectorAll("a.download-button, a.primary-button, #search, #category");
  var out = [];
  for (var i = 0; i < all.length; i++) {
    var el = all[i];
    var r = el.getBoundingClientRect();
    if (r.width > 0 && r.height > 0) out.push(el);
  }
  return out;
}

function centerOf(el) {
  var r = el.getBoundingClientRect();
  return { x: r.left + r.width / 2, y: r.top + r.height / 2 };
}

function moveFocus(dir) {
  var list = focusables();
  if (!list.length) return;
  var cur = document.activeElement;
  if (list.indexOf(cur) === -1) { list[0].focus(); return; }
  var c = centerOf(cur), cand = [], vertical = (dir === "up" || dir === "down");
  for (var i = 0; i < list.length; i++) {
    var el = list[i];
    if (el === cur) continue;
    var p = centerOf(el), dx = p.x - c.x, dy = p.y - c.y, primary, secondary, ok;
    if (vertical) {
      primary = dir === "down" ? dy : -dy; secondary = Math.abs(dx); ok = primary > 24;
    } else {
      primary = dir === "right" ? dx : -dx; secondary = Math.abs(dy); ok = primary > 4 && secondary < 40;
    }
    if (ok) cand.push({ el: el, primary: primary, secondary: secondary });
  }
  if (!cand.length) return;
  var minP = 1e9, best = null, i2;
  for (i2 = 0; i2 < cand.length; i2++) if (cand[i2].primary < minP) minP = cand[i2].primary;
  for (i2 = 0; i2 < cand.length; i2++) {
    var k = cand[i2];
    /* vertical: fica na linha mais proxima e escolhe o mais alinhado; horizontal: o mais proximo */
    if (vertical) {
      if (k.primary <= minP + 24 && (!best || k.secondary < best.secondary)) best = k;
    } else if (!best || k.primary < best.primary) best = k;
  }
  best.el.focus();
}

function stepCategory(d) {
  var n = categorySelect.options.length;
  categorySelect.selectedIndex = (categorySelect.selectedIndex + d + n) % n;
  filterCatalog();
  showToast("Categoria: " + categorySelect.options[categorySelect.selectedIndex].text);
}

function pressAction(a) {
  var cur = document.activeElement;
  if (a === "up" || a === "down" || a === "left" || a === "right") moveFocus(a);
  else if (a === "cross") { if (cur && cur.click) cur.click(); }
  else if (a === "circle") { if (cur && cur.blur) cur.blur(); window.scrollTo(0, 0); }
  else if (a === "triangle") searchInput.focus();
  else if (a === "square") stepCategory(1);
  else if (a === "l1") stepCategory(-1);
  else if (a === "r1") stepCategory(1);
}

/* Teclado (teste no PC): setas, Enter = X nos botoes, Esc = Bola, T = Triangulo, S = Quadrado, Q/E = L1/R1 */
var KEYS = { 37: "left", 38: "up", 39: "right", 40: "down", 27: "circle", 84: "triangle", 83: "square", 81: "l1", 69: "r1" };
document.addEventListener("keydown", function (e) {
  var tag = document.activeElement ? document.activeElement.tagName : "";
  var typing = tag === "INPUT" || tag === "SELECT";
  var a = KEYS[e.keyCode];
  if (!a) return;
  if (typing && e.keyCode !== 27 && e.keyCode !== 38 && e.keyCode !== 40) return;
  if (tag === "SELECT" && (e.keyCode === 38 || e.keyCode === 40)) return;
  e.preventDefault();
  pressAction(a);
});

/* Controle (Gamepad API, mapeamento padrao do DualShock 4) */
var PAD = { 0: "cross", 1: "circle", 2: "square", 3: "triangle", 4: "l1", 5: "r1", 12: "up", 13: "down", 14: "left", 15: "right" };
var REPEAT = { up: true, down: true, left: true, right: true };
var prevBtn = {}, nextAt = {};

function pollPad() {
  var pads = (navigator.getGamepads && navigator.getGamepads()) || [];
  var gp = null;
  for (var i = 0; i < pads.length; i++) { if (pads[i]) { gp = pads[i]; break; } }
  if (gp) {
    var now = Date.now();
    for (var k in PAD) {
      if (!PAD.hasOwnProperty(k)) continue;
      var down = !!(gp.buttons[k] && gp.buttons[k].pressed);
      var name = PAD[k];
      if (down && !prevBtn[k]) { pressAction(name); nextAt[k] = now + 350; }
      else if (down && REPEAT[name] && now >= nextAt[k]) { pressAction(name); nextAt[k] = now + 110; }
      prevBtn[k] = down;
    }
  }
  (window.requestAnimationFrame || function (f) { setTimeout(f, 33); })(pollPad);
}
pollPad();
