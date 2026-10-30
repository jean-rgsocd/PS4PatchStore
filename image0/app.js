
/* PS4 Patch Store - catalogo, instalacao nativa e navegacao por controle.
   ES5 compativel com o WebKit do PS4. */

/* Servidor HTTP do proprio aplicativo PS4 */
var INSTALL_ENDPOINT = "/api/install";

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
    var show = name.indexOf(query) !== -1 &&
      (category === "all" || cardCategory === category);

    card.hidden = !show;
    if (show) visible += 1;
  }

  resultCount.textContent = visible + " " +
    (visible === 1 ? "coleção" : "coleções");

  emptyState.hidden = visible !== 0;
}

function showToast(msg) {
  toast.textContent = msg;
  toast.hidden = false;

  if (toastTimer) clearTimeout(toastTimer);

  toastTimer = setTimeout(function () {
    toast.hidden = true;
  }, 6000);
}

function getAttribute(element, name) {
  if (!element) return "";
  return element.getAttribute(name) || "";
}

/* Envia o pedido para o servidor nativo do aplicativo */
function installPkg(url, button) {
  if (!url) {
    showToast("URL do pacote não configurada.");
    return;
  }

  var card = button;

  while (card && card !== document && !(
    card.classList && card.classList.contains("game-card")
  )) {
    card = card.parentNode;
  }

  if (!card || card === document) card = button;

  var packageData = {
    type: "direct",
    url: url,
    name: getAttribute(button, "data-name") ||
          getAttribute(card, "data-name"),
    content_id: getAttribute(button, "data-content-id") ||
                getAttribute(card, "data-content-id"),
    package_type: getAttribute(button, "data-package-type") ||
                  getAttribute(card, "data-package-type"),
    size: getAttribute(button, "data-size") ||
          getAttribute(card, "data-size"),
    icon: getAttribute(button, "data-icon") ||
          getAttribute(card, "data-icon")
  };

  showToast("Enviando pedido ao instalador nativo do PS4...");

  var xhr = new XMLHttpRequest();

  xhr.open("POST", INSTALL_ENDPOINT, true);
  xhr.setRequestHeader("Content-Type", "text/plain;charset=UTF-8");
  xhr.timeout = 30000;

  xhr.onload = function () {
    var response = null;

    try {
      response = JSON.parse(xhr.responseText);
    } catch (e) {}

    if (xhr.status >= 200 && xhr.status < 300 &&
        response && response.status === "success") {
      showToast("Pedido recebido pelo aplicativo. Acompanhe as Notificações do PS4.");
    } else if (response && response.error) {
      showToast("Erro: " + response.error);
    } else {
      showToast("Servidor respondeu HTTP " + xhr.status +
        ". " + (xhr.responseText || "Sem detalhes."));
    }
  };

  xhr.onerror = function () {
    showToast("Não foi possível conectar ao instalador nativo. Verifique o servidor do aplicativo.");
  };

  xhr.ontimeout = function () {
    showToast("Tempo esgotado aguardando o servidor do aplicativo.");
  };

  xhr.send(JSON.stringify(packageData));
}

function onDownloadClick(e) {
  e.preventDefault();

  var url = this.getAttribute("data-pkg");

  if (!url) {
    showToast("URL do pacote não configurada (data-pkg no index.html).");
    return;
  }

  installPkg(url, this);
}

var buttons = document.querySelectorAll("a[data-pkg]");

for (var b = 0; b < buttons.length; b++) {
  buttons[b].addEventListener("click", onDownloadClick);
}

searchInput.addEventListener("input", filterCatalog);
categorySelect.addEventListener("change", filterCatalog);

document.getElementById("year").textContent = new Date().getFullYear();

filterCatalog();

/* ---------- Navegacao por controle / teclado ---------- */

var focusStyle = document.createElement("style");

focusStyle.appendChild(document.createTextNode(
  "a:focus, #search:focus, #category:focus {" +
  "outline: 3px solid #ffc857; outline-offset: 3px;}" +
  ".download-button:focus, .primary-button:focus {" +
  "background: #75f4c0;}"
));

document.head.appendChild(focusStyle);

function focusables() {
  var all = document.querySelectorAll(
    "a.download-button, a.primary-button, #search, #category"
  );

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

  return {
    x: r.left + r.width / 2,
    y: r.top + r.height / 2
  };
}

function moveFocus(dir) {
  var list = focusables();

  if (!list.length) return;

  var cur = document.activeElement;

  if (list.indexOf(cur) === -1) {
    list[0].focus();
    return;
  }

  var c = centerOf(cur);
  var cand = [];
  var vertical = (dir === "up" || dir === "down");

  for (var i = 0; i < list.length; i++) {
    var el = list[i];

    if (el === cur) continue;

    var p = centerOf(el);
    var dx = p.x - c.x;
    var dy = p.y - c.y;
    var primary, secondary, ok;

    if (vertical) {
      primary = dir === "down" ? dy : -dy;
      secondary = Math.abs(dx);
      ok = primary > 24;
    } else {
      primary = dir === "right" ? dx : -dx;
      secondary = Math.abs(dy);
      ok = primary > 4 && secondary < 40;
    }

    if (ok) {
      cand.push({
        el: el,
        primary: primary,
        secondary: secondary
      });
    }
  }

  if (!cand.length) return;

  var minP = 1e9;
  var best = null;

  for (var i2 = 0; i2 < cand.length; i2++) {
    if (cand[i2].primary < minP) minP = cand[i2].primary;
  }

  for (var j = 0; j < cand.length; j++) {
    var k = cand[j];

    if (vertical) {
      if (k.primary <= minP + 24 &&
          (!best || k.secondary < best.secondary)) {
        best = k;
      }
    } else if (!best || k.primary < best.primary) {
      best = k;
    }
  }

  if (best) best.el.focus();
}

function stepCategory(d) {
  var n = categorySelect.options.length;

  categorySelect.selectedIndex =
    (categorySelect.selectedIndex + d + n) % n;

  filterCatalog();

  showToast(
    "Categoria: " +
    categorySelect.options[categorySelect.selectedIndex].text
  );
}

function pressAction(a) {
  var cur = document.activeElement;

  if (a === "up" || a === "down" ||
      a === "left" || a === "right") {
    moveFocus(a);
  } else if (a === "cross") {
    if (cur && cur.click) cur.click();
  } else if (a === "circle") {
    if (cur && cur.blur) cur.blur();
    window.scrollTo(0, 0);
  } else if (a === "triangle") {
    searchInput.focus();
  } else if (a === "square") {
    stepCategory(1);
  } else if (a === "l1") {
    stepCategory(-1);
  } else if (a === "r1") {
    stepCategory(1);
  }
}

/* Teclado para testes no PC */
var KEYS = {
  37: "left",
  38: "up",
  39: "right",
  40: "down",
  27: "circle",
  84: "triangle",
  83: "square",
  81: "l1",
  69: "r1"
};

document.addEventListener("keydown", function (e) {
  var tag = document.activeElement ?
    document.activeElement.tagName : "";

  var typing = tag === "INPUT" || tag === "SELECT";
  var a = KEYS[e.keyCode];

  if (!a) return;

  if (typing && e.keyCode !== 27 &&
      e.keyCode !== 38 && e.keyCode !== 40) return;

  if (tag === "SELECT" &&
      (e.keyCode === 38 || e.keyCode === 40)) return;

  e.preventDefault();
  pressAction(a);
});

/* Controle DualShock 4 */
var PAD = {
  0: "cross",
  1: "circle",
  2: "square",
  3: "triangle",
  4: "l1",
  5: "r1",
  12: "up",
  13: "down",
  14: "left",
  15: "right"
};

var REPEAT = {
  up: true,
  down: true,
  left: true,
  right: true
};

var prevBtn = {};
var nextAt = {};

function pollPad() {
  var pads = (navigator.getGamepads &&
    navigator.getGamepads()) || [];

  var gp = null;

  for (var i = 0; i < pads.length; i++) {
    if (pads[i]) {
      gp = pads[i];
      break;
    }
  }

  if (gp) {
    var now = Date.now();

    for (var k in PAD) {
      if (!PAD.hasOwnProperty(k)) continue;

      var down = !!(
        gp.buttons[k] && gp.buttons[k].pressed
      );

      var name = PAD[k];

      if (down && !prevBtn[k]) {
        pressAction(name);
        nextAt[k] = now + 350;
      } else if (down && REPEAT[name] &&
                 now >= nextAt[k]) {
        pressAction(name);
        nextAt[k] = now + 110;
      }

      prevBtn[k] = down;
    }
  }

  (window.requestAnimationFrame ||
    function (f) {
      setTimeout(f, 33);
    })(pollPad);
}

pollPad();
