(function () {
  "use strict";

  function initInteraction() {
    // Chrome 78 不支持 :focus-visible：只在键盘 Tab 导航时显示焦点框，鼠标点击不残留边框。
    document.addEventListener("keydown", function (event) {
      if (event.key === "Tab") document.body.classList.add("keyboard-navigation");
    });
    document.addEventListener("mousedown", function () {
      document.body.classList.remove("keyboard-navigation");
    });
    document.addEventListener("touchstart", function () {
      document.body.classList.remove("keyboard-navigation");
    });
  }

  function applyBranding(config, prefix) {
    if (!config || typeof config !== "object") return;
    var nameElement = document.getElementById("brand-name");
    var descriptionElement = document.getElementById("brand-description");
    var favicon = document.getElementById("brand-favicon");
    var name = typeof config.displayName === "string" ? config.displayName.trim() : "";
    var title = typeof config.title === "string" ? config.title.trim() : "";
    var description = typeof config.description === "string" ? config.description.trim() : "";
    if (nameElement && name) nameElement.textContent = name.slice(0, 80);
    if (title || name) document.title = title || (name + " · 简单好用的本地清单");
    if (descriptionElement && description) descriptionElement.setAttribute("content", description);

    var logo = typeof config.logo === "string" ? config.logo.trim() : "";
    if (!logo || logo.charAt(0) === "/" || logo.indexOf("\\") >= 0) return;
    var segments = logo.split("/");
    for (var i = 0; i < segments.length; i += 1) {
      if (!segments[i] || segments[i] === "." || segments[i] === "..") return;
      segments[i] = encodeURIComponent(segments[i]);
    }
    var url = prefix + "local/" + segments.join("/");
    var images = document.querySelectorAll("[data-brand-logo]");
    for (var j = 0; j < images.length; j += 1) {
      images[j].addEventListener("error", function () {
        var fallback = this.getAttribute("data-default-logo") || (prefix + "assets/kylinlist.svg");
        if (this.getAttribute("src") === fallback) return;
        this.src = fallback;
      });
      images[j].src = url;
    }
    if (favicon) {
      favicon.href = url;
      favicon.type = /\.png$/i.test(logo) ? "image/png" : "image/svg+xml";
    }
  }

  function loadBranding(prefix) {
    prefix = prefix || "";
    return fetch(prefix + "local/config.json", { cache: "no-cache" })
      .then(function (response) {
        if (!response.ok) return null;
        return response.json();
      })
      .then(function (config) { applyBranding(config, prefix); })
      .catch(function () { return null; });
  }

  window.KylinSite = {
    initInteraction: initInteraction,
    loadBranding: loadBranding
  };
}());
