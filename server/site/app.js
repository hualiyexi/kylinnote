(function () {
  "use strict";

  var manifest = null;
  var loadingPromise = null;
  var lastFocused = null;

  var statusElement = document.getElementById("download-status");
  var primaryElement = document.getElementById("primary-downloads");
  var downloadMenu = null;
  var downloadToggle = null;
  var historyOpen = document.getElementById("history-open");
  var historyModal = document.getElementById("history-modal");
  var historyPanel = historyModal.querySelector(".history-panel");
  var historyClose = document.getElementById("history-close");
  var historyContent = document.getElementById("history-content");

  var targets = [
    { platform: "linux", arch: "arm64", variant: "system-qt", name: "下载 ARM64 版", detail: "银河麒麟 · 飞腾" },
    { platform: "linux", arch: "amd64", variant: "system-qt", name: "下载 x86_64 版", detail: "银河麒麟 · Intel / AMD" },
    { platform: "windows", arch: "amd64", variant: "setup", name: "下载 Windows 版", detail: "Windows x64 安装程序" },
    { platform: "linux", arch: "arm64", variant: "bundled-qt", name: "ARM64 自包含 Qt 版", detail: "银河麒麟 · 飞腾" },
    { platform: "linux", arch: "amd64", variant: "bundled-qt", name: "x86_64 自包含 Qt 版", detail: "银河麒麟 · Intel / AMD" },
    { platform: "windows", arch: "amd64", variant: "portable", name: "Windows x64 便携版", detail: "便携 ZIP 压缩包" }
  ];

  function clearElement(element) {
    while (element.firstChild) element.removeChild(element.firstChild);
  }

  function formatSize(bytes) {
    if (typeof bytes !== "number" || bytes < 0) return "";
    if (bytes >= 1024 * 1024) return (bytes / (1024 * 1024)).toFixed(1) + " MB";
    if (bytes >= 1024) return Math.round(bytes / 1024) + " KB";
    return bytes + " 字节";
  }

  function shortDate(value) {
    var text = typeof value === "string" ? value : "";
    return /^\d{4}-\d{2}-\d{2}/.test(text) ? text.slice(0, 10) : "";
  }

  function findArtifact(target) {
    var artifacts = manifest && manifest.artifacts;
    if (!Array.isArray(artifacts)) return null;
    for (var i = 0; i < artifacts.length; i += 1) {
      var artifact = artifacts[i];
      if (artifact && artifact.platform === target.platform &&
          artifact.arch === target.arch && artifact.variant === target.variant &&
          typeof artifact.path === "string") {
        return artifact;
      }
    }
    return null;
  }

  function closeDownloadMenu() {
    if (!downloadMenu || downloadMenu.hidden) return;
    downloadMenu.hidden = true;
    downloadToggle.setAttribute("aria-expanded", "false");
  }

  function menuDownloadItem(target, artifact) {
    var link = document.createElement("a");
    link.className = "download-menu-item";
    link.href = "updates/" + artifact.path;
    link.setAttribute("role", "menuitem");
    var name = document.createElement("span");
    name.textContent = target.name;
    var meta = document.createElement("small");
    var size = formatSize(artifact.size);
    meta.textContent = target.detail + (size ? " · " + size : "");
    link.appendChild(name);
    link.appendChild(meta);
    return link;
  }

  function renderDownloads() {
    clearElement(primaryElement);
    closeDownloadMenu();
    var defaultIndex = 0;
    var defaultArtifact = findArtifact(targets[defaultIndex]);
    if (!defaultArtifact) {
      defaultIndex = 3;  // 系统 Qt 包缺失时仍以 ARM64 自包含包为默认。
      defaultArtifact = findArtifact(targets[defaultIndex]);
    }
    if (!defaultArtifact) {
      for (var i = 1; i < targets.length; i += 1) {
        defaultArtifact = findArtifact(targets[i]);
        if (defaultArtifact) { defaultIndex = i; break; }
      }
    }
    statusElement.classList.remove("error");
    if (!defaultArtifact) {
      statusElement.textContent = "当前稳定版尚未提供可下载的安装包。";
      return;
    }

    var target = targets[defaultIndex];
    var split = document.createElement("div");
    split.className = "download-split";
    var main = document.createElement("a");
    main.className = "download-main";
    main.href = "updates/" + defaultArtifact.path;
    main.setAttribute("data-version", manifest.version);
    var name = document.createElement("span");
    name.className = "download-name";
    name.textContent = target.name;
    var meta = document.createElement("span");
    meta.className = "download-meta";
    var size = formatSize(defaultArtifact.size);
    meta.textContent = target.detail + (size ? " · " + size : "");
    main.appendChild(name);
    main.appendChild(meta);
    split.appendChild(main);

    downloadToggle = document.createElement("button");
    downloadToggle.className = "download-toggle";
    downloadToggle.type = "button";
    downloadToggle.setAttribute("aria-label", "选择其他版本");
    downloadToggle.setAttribute("aria-haspopup", "menu");
    downloadToggle.setAttribute("aria-expanded", "false");
    var chevron = document.createElement("span");
    chevron.className = "download-chevron";
    chevron.setAttribute("aria-hidden", "true");
    downloadToggle.appendChild(chevron);
    split.appendChild(downloadToggle);

    downloadMenu = document.createElement("div");
    downloadMenu.className = "download-menu";
    downloadMenu.setAttribute("role", "menu");
    downloadMenu.hidden = true;
    var optionCount = 0;
    for (var j = 0; j < targets.length; j += 1) {
      if (j === defaultIndex) continue;
      var artifact = findArtifact(targets[j]);
      if (!artifact) continue;
      downloadMenu.appendChild(menuDownloadItem(targets[j], artifact));
      optionCount += 1;
    }
    downloadToggle.disabled = optionCount === 0;
    split.appendChild(downloadMenu);
    primaryElement.appendChild(split);

    downloadToggle.addEventListener("click", function (event) {
      event.stopPropagation();
      if (downloadMenu.hidden) {
        downloadMenu.hidden = false;
        downloadToggle.setAttribute("aria-expanded", "true");
      } else {
        closeDownloadMenu();
      }
    });
    downloadMenu.addEventListener("click", closeDownloadMenu);
    var date = shortDate(manifest.publishedAt);
    statusElement.textContent = "当前稳定版 v" + manifest.version + (date ? " · " + date : "");
  }

  function validManifest(value) {
    return value && value.schemaVersion === 1 && value.channel === "stable" &&
      typeof value.version === "string" && Array.isArray(value.artifacts);
  }

  function loadManifest() {
    if (loadingPromise) return loadingPromise;
    statusElement.classList.remove("error");
    statusElement.textContent = "正在获取最新版本…";
    loadingPromise = fetch("updates/v1/stable/manifest.json", { cache: "no-cache" })
      .then(function (response) {
        if (!response.ok) throw new Error("HTTP " + response.status);
        return response.json();
      })
      .then(function (value) {
        if (!validManifest(value)) throw new Error("Invalid manifest");
        manifest = value;
        renderDownloads();
        if (!historyModal.hidden) renderHistory();
        return manifest;
      })
      .catch(function (error) {
        statusElement.classList.add("error");
        statusElement.textContent = "暂时无法读取下载列表，请稍后刷新重试。";
        if (!historyModal.hidden) renderHistoryError();
        throw error;
      });
    loadingPromise = loadingPromise.then(function (value) {
      loadingPromise = null;
      return value;
    }, function (error) {
      loadingPromise = null;
      throw error;
    });
    // 自动加载失败已由界面呈现，不产生未处理的 Promise 拒绝。
    loadingPromise.catch(function () {});
    return loadingPromise;
  }

  function versionParts(value) {
    var parts = String(value).split(".");
    return [Number(parts[0] || 0), Number(parts[1] || 0), Number(parts[2] || 0)];
  }

  function compareVersions(left, right) {
    var a = versionParts(left);
    var b = versionParts(right);
    for (var i = 0; i < 3; i += 1) {
      if (a[i] !== b[i]) return a[i] < b[i] ? -1 : 1;
    }
    return 0;
  }

  function collectHistory() {
    var entries = [];
    var seen = {};
    if (manifest && Array.isArray(manifest.history)) {
      for (var i = 0; i < manifest.history.length; i += 1) {
        var item = manifest.history[i];
        if (!item || typeof item.version !== "string" || seen[item.version]) continue;
        seen[item.version] = true;
        entries.push({
          version: item.version,
          publishedAt: typeof item.publishedAt === "string" ? item.publishedAt : "",
          notes: typeof item.notes === "string" ? item.notes : ""
        });
      }
    }
    if (manifest && !seen[manifest.version]) {
      entries.push({ version: manifest.version, publishedAt: manifest.publishedAt, notes: manifest.notes || "" });
    }
    entries.sort(function (left, right) { return compareVersions(left.version, right.version); });
    return entries;
  }

  function renderHistoryError() {
    clearElement(historyContent);
    var message = document.createElement("p");
    message.className = "history-empty";
    message.textContent = "暂时无法读取更新日志，请关闭后重试。";
    historyContent.appendChild(message);
  }

  function renderHistory() {
    var entries = collectHistory();
    clearElement(historyContent);
    if (!entries.length) {
      var empty = document.createElement("p");
      empty.className = "history-empty";
      empty.textContent = "暂无更新日志。";
      historyContent.appendChild(empty);
      return;
    }
    var list = document.createElement("ol");
    list.className = "history-list";
    for (var i = 0; i < entries.length; i += 1) {
      var entry = entries[i];
      var item = document.createElement("li");
      item.className = "history-item";
      var row = document.createElement("div");
      row.className = "history-version-row";
      var version = document.createElement("span");
      version.className = "history-version";
      version.textContent = "v" + entry.version;
      var date = document.createElement("time");
      date.className = "history-date";
      date.textContent = shortDate(entry.publishedAt);
      row.appendChild(version);
      row.appendChild(date);
      var notes = document.createElement("p");
      notes.className = "history-notes";
      notes.textContent = entry.notes || "该版本未填写更新说明。";
      item.appendChild(row);
      item.appendChild(notes);
      list.appendChild(item);
    }
    historyContent.appendChild(list);
    historyContent.scrollTop = 0;
  }

  function openHistory() {
    closeDownloadMenu();
    lastFocused = document.activeElement;
    historyModal.hidden = false;
    document.body.classList.add("modal-open");
    if (manifest) renderHistory();
    else {
      clearElement(historyContent);
      var loading = document.createElement("p");
      loading.className = "history-loading";
      loading.textContent = "正在读取更新日志…";
      historyContent.appendChild(loading);
      if (!loadingPromise) loadManifest();
    }
    historyPanel.focus();
  }

  function closeHistory() {
    if (historyModal.hidden) return;
    historyModal.hidden = true;
    document.body.classList.remove("modal-open");
    if (lastFocused && typeof lastFocused.focus === "function") lastFocused.focus();
  }

  historyOpen.addEventListener("click", openHistory);
  historyClose.addEventListener("click", closeHistory);
  historyModal.addEventListener("click", function (event) {
    if (event.target.hasAttribute("data-close-history")) closeHistory();
  });
  document.addEventListener("click", closeDownloadMenu);
  document.addEventListener("keydown", function (event) {
    if (event.key === "Escape" || event.key === "Esc") {
      closeHistory();
      if (downloadMenu && !downloadMenu.hidden) {
        closeDownloadMenu();
        downloadToggle.focus();
      }
    }
    if (event.key === "Tab" && !historyModal.hidden) {
      // 弹层只有一个可操作控件，保持键盘焦点不落入背景页面。
      event.preventDefault();
      historyClose.focus();
    }
  });

  window.KylinSite.initInteraction();
  window.KylinSite.loadBranding("");
  loadManifest();
}());
