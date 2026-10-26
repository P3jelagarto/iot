(function () {
  const MAX_POINTS = 50;
  const ONLINE_THRESHOLD_SECONDS = 15;
  const POLL_INTERVAL_MS = 2500;
  const VALUE_TOLERANCE = 0.001;

  const root = document.getElementById("dashboard-root");
  if (!root) return;

  const apiUrl = root.dataset.apiUrl;
  const commandUrlTemplate = root.dataset.commandUrlTemplate || "";
  const seedDataEl = document.getElementById("seed-data");
  const seedData = seedDataEl ? JSON.parse(seedDataEl.textContent) : {};

  function cssVar(name) {
    return getComputedStyle(document.documentElement).getPropertyValue(name).trim();
  }

  function hexToRgba(hex, alpha) {
    const clean = hex.replace("#", "");
    const bigint = parseInt(clean, 16);
    const r = (bigint >> 16) & 255;
    const g = (bigint >> 8) & 255;
    const b = bigint & 255;
    return `rgba(${r}, ${g}, ${b}, ${alpha})`;
  }

  function formatTime(iso) {
    const d = new Date(iso);
    return d.toLocaleTimeString([], { hour: "2-digit", minute: "2-digit", second: "2-digit" });
  }

  function formatValue(v) {
    return Number.isInteger(v) ? String(v) : v.toFixed(1);
  }

  function percentOf(value, el) {
    const min = parseFloat(el.dataset.min);
    const max = parseFloat(el.dataset.max);
    if (!(max > min)) return 0;
    return Math.max(0, Math.min(100, ((value - min) / (max - min)) * 100));
  }

  function getCookie(name) {
    const match = document.cookie.match(new RegExp("(^| )" + name + "=([^;]+)"));
    return match ? decodeURIComponent(match[2]) : null;
  }

  function commandUrl(sensorId) {
    return commandUrlTemplate.replace(/\/0\/command\//, `/${sensorId}/command/`);
  }

  async function postCommand(sensorId, value) {
    try {
      const res = await fetch(commandUrl(sensorId), {
        method: "POST",
        headers: {
          "Content-Type": "application/json",
          "X-CSRFToken": getCookie("csrftoken"),
        },
        body: JSON.stringify({ value: value }),
      });
      return res.ok;
    } catch (err) {
      console.error("Command failed", err);
      return false;
    }
  }

  function createChart(canvas, seedPoints) {
    const seriesColor = cssVar("--series-1");
    const grid = cssVar("--gridline");
    const muted = cssVar("--text-muted");
    const surface = cssVar("--surface-1");

    return new Chart(canvas, {
      type: "line",
      data: {
        labels: seedPoints.map((p) => formatTime(p.t)),
        datasets: [
          {
            data: seedPoints.map((p) => p.v),
            borderColor: seriesColor,
            backgroundColor: hexToRgba(seriesColor, 0.1),
            fill: true,
            borderWidth: 2,
            borderCapStyle: "round",
            borderJoinStyle: "round",
            pointRadius: 0,
            pointHoverRadius: 4,
            pointHoverBackgroundColor: seriesColor,
            pointHoverBorderColor: surface,
            pointHoverBorderWidth: 2,
            tension: 0.3,
            cubicInterpolationMode: "monotone",
          },
        ],
      },
      options: {
        responsive: true,
        maintainAspectRatio: false,
        animation: false,
        interaction: { mode: "nearest", intersect: false },
        plugins: {
          legend: { display: false },
          tooltip: { displayColors: false },
        },
        scales: {
          x: { display: false, grid: { display: false } },
          y: {
            grid: { color: grid },
            ticks: { color: muted, maxTicksLimit: 4 },
          },
        },
      },
    });
  }

  const charts = {};
  const lastTimestamps = {};
  const pendingCommands = {}; // sensorId -> value we told the device to reach, cleared once confirmed

  document.querySelectorAll(".sensor-card").forEach((card) => {
    const sensorId = card.dataset.sensorId;
    const widgetType = card.dataset.widgetType;

    if (widgetType === "line_chart" || widgetType === "tank") {
      const canvas = card.querySelector('[data-role="chart"]');
      const seed = seedData[sensorId] || [];
      charts[sensorId] = createChart(canvas, seed);
      if (seed.length) {
        lastTimestamps[sensorId] = seed[seed.length - 1].t;
      }
    }

    if (widgetType === "button") {
      const btn = card.querySelector('[data-role="gadget-button"]');
      btn.addEventListener("click", async () => {
        const onValue = parseFloat(btn.dataset.onValue);
        const offValue = parseFloat(btn.dataset.offValue);
        const isLit = btn.classList.contains("lit");
        const target = isLit ? offValue : onValue;
        btn.classList.add("pending");
        pendingCommands[sensorId] = target;
        await postCommand(sensorId, target);
      });
    }

    if (widgetType === "slider") {
      const slider = card.querySelector('[data-role="gadget-slider"]');
      slider.addEventListener("change", async () => {
        pendingCommands[sensorId] = parseFloat(slider.value);
        await postCommand(sensorId, parseFloat(slider.value));
      });
    }
  });

  function isSliderBeingDragged(slider) {
    return document.activeElement === slider;
  }

  async function poll() {
    try {
      const res = await fetch(apiUrl, { headers: { "X-Requested-With": "XMLHttpRequest" } });
      if (!res.ok) return;
      const data = await res.json();
      const serverNow = new Date(data.server_time).getTime();
      const valuesById = {};
      data.sensors.forEach((s) => {
        if (s.value !== null) valuesById[s.id] = s.value;
      });

      updateKpis(valuesById);
      updateSetpoints(valuesById);
      updateLastSeen(data.devices, serverNow);

      data.sensors.forEach((s) => {
        if (s.value === null) return;
        const card = document.querySelector(`.sensor-card[data-sensor-id="${s.id}"]`);
        if (!card) return;
        const widgetType = card.dataset.widgetType;

        const valueEl = card.querySelector('[data-role="latest-value"]');
        if (valueEl) valueEl.textContent = `${formatValue(s.value)} ${s.unit || ""}`.trim();

        if (widgetType === "tank") {
          const tank = card.querySelector('[data-role="tank"]');
          card.querySelector('[data-role="tank-fill"]').style.height = `${percentOf(s.value, tank)}%`;
        }

        if (widgetType === "gauge") {
          const pct = percentOf(s.value, card.querySelector('[data-role="gauge"]'));
          card.querySelector('[data-role="gauge-fill"]').style.strokeDasharray = `${pct} 100`;
          card.querySelector('[data-role="gauge-needle"]').style.transform = `rotate(${pct * 1.8 - 90}deg)`;
        }

        if (widgetType === "line_chart" || widgetType === "tank") {
          const chart = charts[s.id];
          if (!chart || lastTimestamps[s.id] === s.timestamp) return;
          lastTimestamps[s.id] = s.timestamp;
          chart.data.labels.push(formatTime(s.timestamp));
          chart.data.datasets[0].data.push(s.value);
          if (chart.data.datasets[0].data.length > MAX_POINTS) {
            chart.data.labels.shift();
            chart.data.datasets[0].data.shift();
          }
          chart.update("none");
        }

        if (widgetType === "meter") {
          const fill = card.querySelector('[data-role="meter-fill"]');
          fill.style.width = `${percentOf(s.value, fill)}%`;
        }

        if (widgetType === "led") {
          const dot = card.querySelector('[data-role="led-dot"]');
          const threshold = parseFloat(dot.dataset.threshold);
          dot.classList.toggle("lit", s.value >= threshold);
          card.classList.toggle("alarm", s.value >= threshold);
        }

        if (widgetType === "button") {
          const btn = card.querySelector('[data-role="gadget-button"]');
          const onValue = parseFloat(btn.dataset.onValue);
          const isOn = Math.abs(s.value - onValue) < VALUE_TOLERANCE;
          btn.textContent = isOn ? "Encendido" : "Apagado";
          btn.classList.toggle("lit", isOn);

          const pending = pendingCommands[s.id];
          if (pending !== undefined && Math.abs(s.value - pending) < VALUE_TOLERANCE) {
            delete pendingCommands[s.id];
            btn.classList.remove("pending");
          }
        }

        if (widgetType === "slider") {
          const slider = card.querySelector('[data-role="gadget-slider"]');
          if (!isSliderBeingDragged(slider)) {
            slider.value = s.value;
          }
          const pending = pendingCommands[s.id];
          if (pending !== undefined && Math.abs(s.value - pending) < VALUE_TOLERANCE) {
            delete pendingCommands[s.id];
          }
        }
      });

      data.devices.forEach((dev) => {
        const card = document.querySelector(`.device-card[data-device-id="${dev.id}"]`);
        if (!card) return;
        const badge = card.querySelector('[data-role="status-badge"]');
        if (!badge) return;

        if (!dev.last_seen_at) {
          badge.textContent = "sin datos";
          badge.className = "badge";
          return;
        }
        const ageSeconds = (serverNow - new Date(dev.last_seen_at).getTime()) / 1000;
        if (ageSeconds <= ONLINE_THRESHOLD_SECONDS) {
          badge.textContent = "en línea";
          badge.className = "badge online";
        } else {
          badge.textContent = "sin señal";
          badge.className = "badge offline";
        }
      });
    } catch (err) {
      console.error("Polling failed", err);
    }
  }

  function updateKpis(valuesById) {
    document.querySelectorAll(".kpi").forEach((kpi) => {
      const value = valuesById[kpi.dataset.sensorId];
      if (value === undefined) return;

      if (kpi.dataset.kind === "value") {
        kpi.querySelector('[data-role="kpi-value"]').textContent = formatValue(value);
        kpi.querySelector('[data-role="kpi-bar"]').style.width = `${percentOf(value, kpi)}%`;
        return;
      }

      const chip = kpi.querySelector('[data-role="kpi-state"]');
      if (kpi.dataset.kind === "button") {
        const isOn = Math.abs(value - parseFloat(kpi.dataset.onValue)) < VALUE_TOLERANCE;
        chip.textContent = isOn ? "Encendido" : "Apagado";
        chip.className = isOn ? "state-chip on" : "state-chip";
      } else {
        const isLit = value >= parseFloat(kpi.dataset.threshold);
        chip.textContent = isLit ? "Activa" : "Normal";
        chip.className = isLit ? "state-chip alarm" : "state-chip on";
      }
    });
  }

  function updateSetpoints(valuesById) {
    document.querySelectorAll('[data-role="tank-setpoint"]').forEach((line) => {
      const value = valuesById[line.dataset.setpointSensorId];
      if (value === undefined) return;
      const tank = line.closest('[data-role="tank"]');
      line.style.bottom = `${percentOf(value, tank)}%`;
      line.querySelector('[data-role="tank-setpoint-label"]').textContent = `SP ${formatValue(value)}`;
      line.hidden = false;
    });
  }

  function updateLastSeen(devices, serverNow) {
    const el = document.querySelector('[data-role="last-update"]');
    if (!el) return;
    const times = devices.filter((d) => d.last_seen_at).map((d) => new Date(d.last_seen_at).getTime());
    if (!times.length) return;
    const ageSeconds = Math.max(0, Math.round((serverNow - Math.max(...times)) / 1000));
    el.textContent = ageSeconds <= 1 ? "Actualizado ahora" : `Último dato hace ${ageSeconds} s`;
  }

  poll();
  setInterval(poll, POLL_INTERVAL_MS);
})();
