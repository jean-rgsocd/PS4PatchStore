/* PS4 Patch Store
 * Catalogo, leitura de metadados via Range e instalacao nativa BGFT.
 * Compatibilidade: ES5 / WebKit PS4.
 */

(function () {
  "use strict";

  var INSTALL_ENDPOINT = "/api/install";

  var searchInput = document.getElementById("search");
  var categorySelect = document.getElementById("category");
  var resultCount = document.getElementById("result-count");
  var emptyState = document.getElementById("empty-state");
  var toast = document.getElementById("toast");
  var yearElement = document.getElementById("year");

  var cards = Array.prototype.slice.call(
    document.querySelectorAll(".game-card")
  );

  var toastTimer = null;
  var installBusy = false;

  function getAttribute(element, name) {
    if (!element || !element.getAttribute) return "";
    return element.getAttribute(name) || "";
  }

  function hasClass(element, className) {
    return !!(
      element &&
      element.classList &&
      element.classList.contains(className)
    );
  }

  function findCard(element) {
    var current = element;
    while (current && current !== document) {
      if (hasClass(current, "game-card")) {
        return current;
      }
      current = current.parentNode;
    }
    return null;
  }

  function showToast(message) {
    if (!toast) return;
    toast.textContent = message;
    toast.hidden = false;
    if (toastTimer) {
      clearTimeout(toastTimer);
    }
    toastTimer = setTimeout(function () {
      toast.hidden = true;
    }, 6000);
  }

  function filterCatalog() {
    var query = searchInput ?
      (searchInput.value || "").replace(/^\s+|\s+$/g, "").toLowerCase() :
      "";

    var category = categorySelect ?
      categorySelect.value :
      "all";

    var visible = 0;

    for (var i = 0; i < cards.length; i++) {
      var card = cards[i];
      var name = getAttribute(card, "data-name").toLowerCase();
      var cardCategory = getAttribute(card, "data-category");

      var show =
        name.indexOf(query) !== -1 &&
        (category === "all" || cardCategory === category);

      card.hidden = !show;

      if (show) {
        visible++;
      }
    }

    if (resultCount) {
      resultCount.textContent =
        visible + " " + (visible === 1 ? "colecao" : "colecoes");
    }

    if (emptyState) {
      emptyState.hidden = visible !== 0;
    }
  }

  function packageValue(button, card, attribute) {
    return (
      getAttribute(button, attribute) ||
      getAttribute(card, attribute)
    );
  }

  function installPkg(url, button) {
    if (!url) {
      showToast("URL do pacote nao configurada.");
      return;
    }

    if (installBusy) {
      showToast("Ja existe um pedido de instalacao em andamento.");
      return;
    }

    var card = findCard(button) || button;

    var pkgName = packageValue(button, card, "data-name") || "PS4 Package";
    var pkgContentId = packageValue(button, card, "data-content-id");
    var pkgType = packageValue(button, card, "data-package-type") || "application";
    var pkgSize = packageValue(button, card, "data-size");
    var pkgIcon = packageValue(button, card, "data-icon");

    installBusy = true;
    if (button && button.setAttribute) {
      button.setAttribute("aria-disabled", "true");
    }

    showToast("Enviando pedido ao instalador nativo BGFT...");

    var packageData = {
      type: "direct",
      url: url,
      name: pkgName,
      content_id: pkgContentId,
      package_type: pkgType,
      size: pkgSize,
      icon: pkgIcon
    };

    var xhr = new XMLHttpRequest();
    xhr.open("POST", INSTALL_ENDPOINT, true);
    xhr.setRequestHeader("Content-Type", "text/plain;charset=UTF-8");
    xhr.timeout = 30000;

    function finish() {
      installBusy = false;
      if (button && button.removeAttribute) {
        button.removeAttribute("aria-disabled");
      }
    }

    xhr.onload = function () {
      var response = null;
      try {
        response = JSON.parse(xhr.responseText);
      } catch (error) {
        response = null;
      }

      finish();

      if (
        xhr.status >= 200 &&
        xhr.status < 300 &&
        response &&
        response.status === "success"
      ) {
        showToast(
          "Download registrado! Acompanhe nas Notificacoes do PS4."
        );
        return;
      }

      if (response && response.error) {
        showToast("Erro: " + response.error);
        return;
      }

      showToast(
        "Falha HTTP " + xhr.status + ". " + (xhr.responseText || "Resposta invalida.")
      );
    };

    xhr.onerror = function () {
      finish();
      showToast("Nao foi possivel conectar ao servidor de instalacao nativa.");
    };

    xhr.ontimeout = function () {
      finish();
      showToast("Tempo esgotado aguardando o instalador.");
    };

    try {
      xhr.send(JSON.stringify(packageData));
    } catch (error) {
      finish();
      showToast("Erro ao enviar pedido: " + error.message);
    }
  }

  function onDownloadClick(event) {
    if (event && event.preventDefault) {
      event.preventDefault();
    }

    var target = (event && event.currentTarget) ? event.currentTarget : this;
    var url = getAttribute(target, "data-pkg") || (target && target.dataset ? target.dataset.pkg : "");

    if (!url) {
      showToast("URL do pacote nao configurada no data-pkg.");
      return;
    }

    installPkg(url, target);
  }

  var buttons = document.querySelectorAll("a[data-pkg]");
  for (var b = 0; b < buttons.length; b++) {
    buttons[b].addEventListener("click", onDownloadClick, false);
  }

  if (searchInput) {
    searchInput.addEventListener("input", filterCatalog, false);
  }

  if (categorySelect) {
    categorySelect.addEventListener("change", filterCatalog, false);
  }

  if (yearElement) {
    yearElement.textContent = new Date().getFullYear();
  }

  filterCatalog();

  // Foco e controle (mantidos intactos)
  var focusStyle = document.createElement("style");
  focusStyle.appendChild(
    document.createTextNode(
      "a:focus, #search:focus, #category:focus {" +
      "outline: 3px solid #ffc857;" +
      "outline-offset: 3px;" +
      "}" +
      ".download-button:focus, .primary-button:focus {" +
      "background: #75f4c0;" +
      "}"
    )
  );
  document.head.appendChild(focusStyle);

  function focusables() {
    var all = document.querySelectorAll(
      "a.download-button, a.primary-button, #search, #category"
    );
    var output = [];
    for (var i = 0; i < all.length; i++) {
      var element = all[i];
      var rect = element.getBoundingClientRect();
      if (
        rect.width > 0 &&
        rect.height > 0 &&
        !element.disabled &&
        element.getAttribute("aria-disabled") !== "true"
      ) {
        output.push(element);
      }
    }
    return output;
  }

  function centerOf(element) {
    var rect = element.getBoundingClientRect();
    return {
      x: rect.left + rect.width / 2,
      y: rect.top + rect.height / 2
    };
  }

  function moveFocus(direction) {
    var list = focusables();
    if (!list.length) return;
    var current = document.activeElement;
    if (list.indexOf(current) === -1) {
      list[0].focus();
      return;
    }
    var center = centerOf(current);
    var candidates = [];
    var vertical = direction === "up" || direction === "down";

    for (var i = 0; i < list.length; i++) {
      var element = list[i];
      if (element === current) continue;
      var position = centerOf(element);
      var dx = position.x - center.x;
      var dy = position.y - center.y;
      var primary, secondary, valid;

      if (vertical) {
        primary = direction === "down" ? dy : -dy;
        secondary = Math.abs(dx);
        valid = primary > 24;
      } else {
        primary = direction === "right" ? dx : -dx;
        secondary = Math.abs(dy);
        valid = primary > 4 && secondary < 40;
      }

      if (valid) {
        candidates.push({
          element: element,
          primary: primary,
          secondary: secondary
        });
      }
    }

    if (!candidates.length) return;
    var minimum = 1000000000;
    for (var j = 0; j < candidates.length; j++) {
      if (candidates[j].primary < minimum) {
        minimum = candidates[j].primary;
      }
    }

    var best = null;
    for (var k = 0; k < candidates.length; k++) {
      var candidate = candidates[k];
      if (vertical) {
        if (
          candidate.primary <= minimum + 24 &&
          (!best || candidate.secondary < best.secondary)
        ) {
          best = candidate;
        }
      } else {
        if (!best || candidate.primary < best.primary) {
          best = candidate;
        }
      }
    }

    if (best) {
      best.element.focus();
    }
  }

  function stepCategory(direction) {
    if (
      !categorySelect ||
      !categorySelect.options ||
      !categorySelect.options.length
    ) {
      return;
    }
    var count = categorySelect.options.length;
    categorySelect.selectedIndex =
      (categorySelect.selectedIndex + direction + count) % count;
    filterCatalog();
    showToast(
      "Categoria: " + categorySelect.options[categorySelect.selectedIndex].text
    );
  }

  function pressAction(action) {
    var current = document.activeElement;
    if (
      action === "up" ||
      action === "down" ||
      action === "left" ||
      action === "right"
    ) {
      moveFocus(action);
      return;
    }
    if (action === "cross") {
      if (
        current &&
        current.click &&
        current.getAttribute("aria-disabled") !== "true"
      ) {
        current.click();
      }
      return;
    }
    if (action === "circle") {
      if (current && current.blur) {
        current.blur();
      }
      window.scrollTo(0, 0);
      return;
    }
    if (action === "triangle") {
      if (searchInput) {
        searchInput.focus();
      }
      return;
    }
    if (action === "square" || action === "r1") {
      stepCategory(1);
      return;
    }
    if (action === "l1") {
      stepCategory(-1);
    }
  }

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

  document.addEventListener(
    "keydown",
    function (event) {
      var tag = document.activeElement ? document.activeElement.tagName : "";
      var typing = tag === "INPUT" || tag === "SELECT" || tag === "TEXTAREA";
      var action = KEYS[event.keyCode];
      if (!action) return;
      if (typing && event.keyCode !== 27 && event.keyCode !== 38 && event.keyCode !== 40) {
        return;
      }
      event.preventDefault();
      pressAction(action);
    },
    false
  );

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

  var REPEAT = { up: true, down: true, left: true, right: true };
  var previousButtons = {};
  var nextRepeat = {};
  var AXIS_THRESHOLD = 0.65;
  var previousAxis = { horizontal: "", vertical: "" };

  function getAxisDirection(value, negative, positive) {
    if (value < -AXIS_THRESHOLD) return negative;
    if (value > AXIS_THRESHOLD) return positive;
    return "";
  }

  function pollPad() {
    var pads = [];
    if (navigator.getGamepads) {
      try {
        pads = navigator.getGamepads() || [];
      } catch (e) {
        pads = [];
      }
    }
    var gamepad = null;
    for (var i = 0; i < pads.length; i++) {
      if (pads[i] && pads[i].connected !== false) {
        gamepad = pads[i];
        break;
      }
    }

    if (gamepad) {
      var now = Date.now();
      for (var key in PAD) {
        if (!PAD.hasOwnProperty(key)) continue;
        var button = gamepad.buttons[key];
        var pressed = !!(button && button.pressed);
        var action = PAD[key];

        if (pressed && !previousButtons[key]) {
          pressAction(action);
          nextRepeat[key] = now + 350;
        } else if (pressed && REPEAT[action] && now >= nextRepeat[key]) {
          pressAction(action);
          nextRepeat[key] = now + 110;
        }
        previousButtons[key] = pressed;
      }

      if (gamepad.axes && gamepad.axes.length >= 2) {
        var h = getAxisDirection(gamepad.axes[0], "left", "right");
        var v = getAxisDirection(gamepad.axes[1], "up", "down");
        if (h && h !== previousAxis.horizontal) pressAction(h);
        if (v && v !== previousAxis.vertical) pressAction(v);
        previousAxis.horizontal = h;
        previousAxis.vertical = v;
      }
    } else {
      previousButtons = {};
      previousAxis.horizontal = "";
      previousAxis.vertical = "";
    }

    (window.requestAnimationFrame || function (cb) { setTimeout(cb, 33); })(pollPad);
  }

  pollPad();
})();
