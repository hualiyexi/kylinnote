(function () {
  "use strict";

  var refresh = document.getElementById("refresh");
  var errorElement = document.getElementById("error");
  var updatedAt = document.getElementById("updated-at");
  var downloadsBody = document.getElementById("downloads-body");
  var loading = false;

  function setMetric(id, value) {
    document.getElementById(id).textContent = typeof value === "number" && value >= 0 ? String(value) : "—";
  }

  function renderDownloads(rows) {
    while (downloadsBody.firstChild) downloadsBody.removeChild(downloadsBody.firstChild);
    if (!rows.length) {
      var emptyRow = document.createElement("tr");
      var empty = document.createElement("td");
      empty.colSpan = 3;
      empty.className = "empty";
      empty.textContent = "暂无安装包下载记录。";
      emptyRow.appendChild(empty);
      downloadsBody.appendChild(emptyRow);
      return;
    }
    var maximum = 1;
    for (var i = 0; i < rows.length; i += 1) maximum = Math.max(maximum, Number(rows[i].count) || 0);
    for (var j = 0; j < rows.length; j += 1) {
      var row = rows[j];
      var tr = document.createElement("tr");
      var version = document.createElement("td");
      version.className = "version-cell";
      version.textContent = "v" + row.version;
      var count = document.createElement("td");
      count.className = "count-cell";
      count.textContent = String(row.count);
      var barCell = document.createElement("td");
      barCell.className = "bar-cell";
      var track = document.createElement("div");
      track.className = "bar-track";
      var fill = document.createElement("div");
      fill.className = "bar-fill";
      fill.style.width = Math.max(2, Math.round((Number(row.count) || 0) * 100 / maximum)) + "%";
      track.appendChild(fill);
      barCell.appendChild(track);
      tr.appendChild(version);
      tr.appendChild(count);
      tr.appendChild(barCell);
      downloadsBody.appendChild(tr);
    }
  }

  function loadSummary() {
    if (loading) return;
    loading = true;
    refresh.disabled = true;
    refresh.textContent = "正在刷新…";
    errorElement.hidden = true;
    fetch("../api/v1/stats/summary", { cache: "no-store" })
      .then(function (response) {
        if (!response.ok) throw new Error("HTTP " + response.status);
        return response.json();
      })
      .then(function (summary) {
        if (!summary || summary.schemaVersion !== 1 || !summary.usage || !summary.downloads)
          throw new Error("Invalid summary");
        setMetric("metric-users", summary.users);
        setMetric("metric-tasks", summary.usage.tasksCreated);
        setMetric("metric-notes", summary.usage.notesCreated);
        setMetric("metric-completed", summary.usage.tasksCompleted);
        setMetric("metric-days", summary.usage.longestUsageDays);
        setMetric("metric-downloads", summary.downloads.total);
        renderDownloads(Array.isArray(summary.downloads.byVersion) ? summary.downloads.byVersion : []);
        var date = new Date(summary.generatedAt);
        updatedAt.textContent = isNaN(date.getTime())
          ? "数据更新于 " + summary.generatedAt
          : "数据更新于 " + date.toLocaleString("zh-CN");
      })
      .catch(function (error) {
        errorElement.textContent = "暂时无法读取统计数据：" + error.message;
        errorElement.hidden = false;
        updatedAt.textContent = "保留上一次成功读取的结果。";
      })
      .then(function () {
        loading = false;
        refresh.disabled = false;
        refresh.textContent = "刷新数据";
      });
  }

  window.KylinSite.initInteraction();
  window.KylinSite.loadBranding("../").then(function () {
    document.title = document.getElementById("brand-name").textContent + " · 统计概览";
  });
  refresh.addEventListener("click", loadSummary);
  loadSummary();
}());
