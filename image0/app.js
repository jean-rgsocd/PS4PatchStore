/* PS4 Patch Store - filtro do catálogo + instalação de PKG. ES5 para o WebKit do PS4. */
/* Remote Package Installer (homebrew do PS4) escuta aqui no próprio console: */
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
  xhr.setRequestHeader("Content-Type", "application/json");
  xhr.timeout = 15000;
  xhr.onload = function () {
    if (xhr.status >= 200 && xhr.status < 300) {
      showToast("Pedido aceito. Acompanhe em Notificações > Downloads.");
    } else {
      showToast("Instalador respondeu " + xhr.status + ". Tentando download direto...");
      window.location.href = url;
    }
  };
  xhr.onerror = xhr.ontimeout = function () {
    showToast("Remote Package Installer indisponível. Tentando download direto...");
    window.location.href = url;
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
