// VietHUD SpeedMap Studio V5.3 - One-Click Permanent Persistence & Direct File Writer
let map;
const baseLayers = {};
let currentBase = "osm";
let basemapVisible = true;

let roadMasterGroup;
const roadClassGroups = {};
let roadMasterVisible = true;
const roadClassVisibility = { 0: true, 1: true, 2: true, 3: true, 4: true, 5: true };

const alertLayerGroups = {
  speed_cam: null, red_light_cam: null, sign_speed: null, sign_r420: null,
  sign_r421: null, sign_overtake: null, traffic_light: null, toll: null, danger: null
};
let connectorLayerGroup;
let rulerLayerGroup;

const alertLayerVisibility = {
  speed_cam: true, red_light_cam: true, sign_speed: true, sign_r420: true, sign_r421: true, sign_overtake: true,
  traffic_light: true, toll: true, danger: true
};

let rulerActive = false;
let rulerPoints = [];
let datasetInfo = null;

// ACTIVE EDITING STATE
let currentInspected = null; // { origType, id, lat, lon, speed, heading, category, desc, marker, is_new }
let addAlertMode = false;
let selectedNewAlertType = "traffic_light";
let latestInspectionResult = null;
let tempAddMarker = null;
let currentJunctionData = null;
let junctionPreviewMarkers = [];
let junctionSelectedType = "traffic_light";
let junctionPreviewDist = 15;
let junctionModeActive = false;
let selectedNewRoadClass = 3;

let activeAbortController = null;
let fetchTimeout = null;

const CATEGORY_META = {
  speed_cam: { icon: "📷", name: "Camera bắn tốc độ", type: "camera", defaultSpeed: 60, hasSpeed: true },
  red_light_cam: { icon: "📸", name: "Camera phạt nguội / đèn đỏ", type: "camera", defaultSpeed: 0, hasSpeed: false },
  traffic_light: { icon: "🚦", name: "Đèn giao thông", type: "sign", defaultSpeed: 0, hasSpeed: false },
  sign_speed: { icon: "🔴", name: "Biển giới hạn tốc độ (P.127)", type: "sign", defaultSpeed: 60, hasSpeed: true },
  sign_r420: { icon: "🏙️", name: "Vào khu dân cư (R.420)", type: "sign", defaultSpeed: 50, hasSpeed: true },
  sign_r421: { icon: "🏡", name: "Hết khu dân cư (R.421)", type: "sign", defaultSpeed: 60, hasSpeed: true },
  sign_overtake: { icon: "🚫", name: "Cấm vượt / Hết cấm", type: "sign", defaultSpeed: 0, hasSpeed: false },
  toll: { icon: "🎫", name: "Trạm thu phí", type: "sign", defaultSpeed: 0, hasSpeed: false },
  danger: { icon: "⚠️", name: "Cảnh báo khác", type: "sign", defaultSpeed: 0, hasSpeed: false }
};

document.addEventListener("DOMContentLoaded", () => {
  initMap();
  loadDatasetInfo();
  setupMapEvents();
});

function initMap() {
  baseLayers.osm = L.tileLayer('https://{s}.tile.openstreetmap.org/{z}/{x}/{y}.png', {
    attribution: '&copy; OpenStreetMap contributors',
    maxZoom: 19
  });

  baseLayers.cyclosm = L.tileLayer('https://{s}.tile-cyclosm.openstreetmap.fr/cyclosm/{z}/{x}/{y}.png', {
    attribution: '&copy; CyclOSM &copy; OpenStreetMap',
    maxZoom: 19
  });

  baseLayers.satellite = L.tileLayer('https://server.arcgisonline.com/ArcGIS/rest/services/World_Imagery/MapServer/tile/{z}/{y}/{x}', {
    attribution: '&copy; Esri World Imagery',
    maxZoom: 19
  });

  baseLayers.street = L.tileLayer('https://server.arcgisonline.com/ArcGIS/rest/services/World_Street_Map/MapServer/tile/{z}/{y}/{x}', {
    attribution: '&copy; Esri World Street Map',
    maxZoom: 19
  });

  map = L.map('map', {
    center: [21.0460, 105.7814], // Hanoi - Phạm Văn Đồng / Trần Cung
    zoom: 16,
    layers: [baseLayers.osm],
    zoomControl: false,
    preferCanvas: true
  });

  L.control.zoom({ position: 'bottomleft' }).addTo(map);

  roadMasterGroup = L.layerGroup().addTo(map);
  for (let c = 0; c <= 5; c++) {
    roadClassGroups[c] = L.layerGroup().addTo(roadMasterGroup);
  }

  Object.keys(alertLayerGroups).forEach(k => {
    alertLayerGroups[k] = L.layerGroup().addTo(map);
  });

  connectorLayerGroup = L.layerGroup().addTo(map);
  rulerLayerGroup = L.layerGroup().addTo(map);
}

function setupMapEvents() {
  map.on("moveend", () => {
    fetchViewportData();
  });

  map.on("click", (e) => {
    if (junctionModeActive) {
      detectJunctionAtCoords(e.latlng.lat, e.latlng.lng);
      toggleJunctionMode();
    } else if (rulerActive) {
      handleRulerClick(e.latlng);
    } else {
      // Direct 1-Click Drop: Clicking anywhere on the map initiates alert placement!
      handleMapClickAddAlert(e.latlng);
    }
  });
}

function loadDatasetInfo() {
  fetch("/api/info")
    .then(r => r.json())
    .then(info => {
      datasetInfo = info;
      
      document.getElementById("dispRoadTiles").textContent = info.road_files.tiles.split('\\').slice(-2).join('\\');
      document.getElementById("inputRoadTiles").value = info.road_files.tiles;
      document.getElementById("inputRoadIndex").value = info.road_files.index;
      document.getElementById("inputRoadNames").value = info.road_files.names;

      document.getElementById("dispCamFile").textContent = info.camera_filename;
      document.getElementById("inputCamPath").value = info.active_camera_file;

      document.getElementById("dispSignFile").textContent = info.sign_filename;
      document.getElementById("inputSignPath").value = info.active_sign_file;

      updateDirtyBadge(info.changes_count || 0);
      fetchViewportData();
    })
    .catch(err => console.error("Error loading info:", err));
}

// -----------------------------------------------------------------------------
// TURBO VIEWPORT FETCH ENGINE
// -----------------------------------------------------------------------------
function fetchViewportData() {
  if (fetchTimeout) clearTimeout(fetchTimeout);
  fetchTimeout = setTimeout(() => {
    if (activeAbortController) {
      activeAbortController.abort();
    }
    activeAbortController = new AbortController();
    const signal = activeAbortController.signal;

    const bounds = map.getBounds();
    const zoom = map.getZoom();
    const min_lat = bounds.getSouth();
    const max_lat = bounds.getNorth();
    const min_lon = bounds.getWest();
    const max_lon = bounds.getEast();

    showLoader(true);
    const t0 = performance.now();

    const pRoads = fetch(`/api/roads?min_lat=${min_lat}&max_lat=${max_lat}&min_lon=${min_lon}&max_lon=${max_lon}&zoom=${zoom}`, { signal })
      .then(r => r.json())
      .then(data => renderRoads(data.segments, zoom));

    const pCams = fetch(`/api/cameras?min_lat=${min_lat}&max_lat=${max_lat}&min_lon=${min_lon}&max_lon=${max_lon}`, { signal })
      .then(r => r.json())
      .then(data => renderCameras(data.cameras, zoom));

    const pSigns = fetch(`/api/signs?min_lat=${min_lat}&max_lat=${max_lat}&min_lon=${min_lon}&max_lon=${max_lon}`, { signal })
      .then(r => r.json())
      .then(data => renderSigns(data.signs, zoom));

    Promise.all([pRoads, pCams, pSigns])
      .then(() => {
        const elapsed = Math.round(performance.now() - t0);
        document.getElementById("topStatPerf").textContent = `⚡ Tốc độ: ${elapsed}ms`;
      })
      .catch(err => {
        if (err.name !== 'AbortError') console.error("Fetch error:", err);
      })
      .finally(() => showLoader(false));
  }, 90);
}

// -----------------------------------------------------------------------------
// RENDERING WITH INTERACTIVE DRAG-ENABLED MARKERS
// -----------------------------------------------------------------------------
function renderRoads(segments, zoom) {
  for (let c = 0; c <= 5; c++) {
    roadClassGroups[c].clearLayers();
  }

  const counts = { 0: 0, 1: 0, 2: 0, 3: 0, 4: 0, 5: 0 };
  let total = 0;

  if (segments) {
    const classColors = {
      0: "#f59e0b", 1: "#ef4444", 2: "#0284c7",
      3: "#2563eb", 4: "#6366f1", 5: "#64748b"
    };

    segments.forEach(seg => {
      const cls = Math.min(seg.road_class, 5);
      counts[cls]++;
      total++;

      const color = classColors[cls] || "#64748b";
      const weight = cls <= 1 ? 4 : (cls <= 3 ? 3 : 2);
      
      const polyline = L.polyline([seg.start, seg.end], {
        color: color,
        weight: weight,
        opacity: 0.85
      });

      polyline.on("click", (e) => {
        L.DomEvent.stopPropagation(e);
        inspectRoadSegment(seg);
      });

      if (seg.name) {
        polyline.bindTooltip(seg.name, { sticky: true });
      }

      roadClassGroups[cls].addLayer(polyline);
    });
  }

  for (let c = 0; c <= 5; c++) {
    const el = document.getElementById(`count_rc_${c}`);
    if (el) el.textContent = counts[c].toLocaleString();
  }
  document.getElementById("countRoads").textContent = `${total.toLocaleString()} segs`;
}

function renderCameras(cameras, zoom) {
  alertLayerGroups.speed_cam.clearLayers();
  alertLayerGroups.red_light_cam.clearLayers();

  let cSpeed = 0, cRed = 0;

  if (cameras) {
    const useFastDots = zoom < 13;

    cameras.forEach(c => {
      const isSpeed = c.category === "speed_cam";
      const group = isSpeed ? alertLayerGroups.speed_cam : alertLayerGroups.red_light_cam;
      const color = isSpeed ? "#f59e0b" : "#06b6d4";
      const speedText = c.speed > 0 ? c.speed : "CAM";
      const hd = c.heading || 0;

      if (isSpeed) cSpeed++;
      else cRed++;

      let marker;
      const isEditingThis = currentInspected && currentInspected.id === c.id;

      if (useFastDots) {
        marker = L.circleMarker([c.lat, c.lon], {
          radius: 5, color: color, fillColor: "#000", fillOpacity: 0.9, weight: 2
        });
      } else {
        const iconHtml = `
          <div class="cam-marker-icon ${isEditingThis ? 'editing' : ''}" style="width: 26px; height: 26px; border: 2px solid ${color}; box-shadow: 0 0 8px ${color};">
            <span>${speedText}</span>
            ${hd > 0 ? `<div class="cam-arrow" style="border-bottom: 7px solid ${color}; transform: rotate(${hd}deg)"></div>` : ''}
          </div>
        `;
        const customIcon = L.divIcon({
          html: iconHtml, className: '', iconSize: [26, 26], iconAnchor: [13, 13]
        });
        marker = L.marker([c.lat, c.lon], { icon: customIcon, draggable: isEditingThis });
      }

      marker.on("click", (e) => {
        L.DomEvent.stopPropagation(e);
        selectAlertForEditing("camera", c, marker);
      });

      marker.on("drag", () => handleMarkerDrag(marker.getLatLng()));
      marker.on("dragend", () => handleMarkerDragEnd(marker.getLatLng()));

      group.addLayer(marker);
    });
  }

  document.getElementById("count_speed_cam").textContent = cSpeed.toLocaleString();
  document.getElementById("count_red_light_cam").textContent = cRed.toLocaleString();
  updateTopAlertStats();
}

function renderSigns(signs, zoom) {
  ['sign_speed', 'sign_r420', 'sign_r421', 'sign_overtake', 'traffic_light', 'toll', 'danger'].forEach(k => {
    alertLayerGroups[k].clearLayers();
  });

  const counts = { sign_speed: 0, sign_r420: 0, sign_r421: 0, sign_overtake: 0, traffic_light: 0, toll: 0, danger: 0 };

  if (signs) {
    const useFastDots = zoom < 13;

    signs.forEach(s => {
      const cat = s.category || "danger";
      if (counts[cat] !== undefined) counts[cat]++;

      let marker;
      const isEditingThis = currentInspected && currentInspected.id === s.id;

      if (useFastDots) {
        const dotColors = {
          sign_speed: "#ef4444", sign_r420: "#0284c7", sign_r421: "#38bdf8",
          sign_overtake: "#ef4444", traffic_light: "#eab308", toll: "#8b5cf6", danger: "#f97316"
        };
        marker = L.circleMarker([s.lat, s.lon], {
          radius: 4, color: dotColors[cat] || "#fff", fillColor: "#fff", fillOpacity: 0.9, weight: 1
        });
      } else {
        let iconHtml = "";
        let size = [22, 22];

        if (cat === "sign_speed") {
          iconHtml = `<div class="sign-chip-icon red-circle ${isEditingThis ? 'editing' : ''}" style="width:22px;height:22px;font-size:10px;">${s.value || 60}</div>`;
        } else if (cat === "sign_r420") {
          iconHtml = `<div class="sign-chip-icon blue-box" style="width:26px;height:18px;font-size:9px;">R.420</div>`;
          size = [26, 18];
        } else if (cat === "sign_r421") {
          iconHtml = `<div class="sign-chip-icon blue-box slash" style="width:26px;height:18px;font-size:9px;">R.421</div>`;
          size = [26, 18];
        } else if (cat === "sign_overtake") {
          iconHtml = `<div class="sign-chip-icon red-circle" style="width:22px;height:22px;font-size:12px;">🚫</div>`;
        } else if (cat === "traffic_light") {
          iconHtml = `<div class="traffic-light-icon ${isEditingThis ? 'editing' : ''}" style="font-size:22px;filter:drop-shadow(0 0 6px rgba(234,179,8,0.8));">🚦</div>`;
          size = [24, 24];
        } else if (cat === "toll") {
          iconHtml = `<div style="font-size:20px;">🎫</div>`;
        } else {
          iconHtml = `<div style="font-size:20px;">⚠️</div>`;
        }

        const customIcon = L.divIcon({
          html: iconHtml, className: '', iconSize: size, iconAnchor: [size[0]/2, size[1]/2]
        });
        marker = L.marker([s.lat, s.lon], { icon: customIcon, draggable: isEditingThis });
      }

      marker.on("click", (e) => {
        L.DomEvent.stopPropagation(e);
        selectAlertForEditing("sign", s, marker);
      });

      marker.on("drag", () => handleMarkerDrag(marker.getLatLng()));
      marker.on("dragend", () => handleMarkerDragEnd(marker.getLatLng()));

      if (alertLayerGroups[cat]) {
        alertLayerGroups[cat].addLayer(marker);
      }
    });
  }

  Object.entries(counts).forEach(([k, v]) => {
    const el = document.getElementById(`count_${k}`);
    if (el) el.textContent = v.toLocaleString();
  });
  updateTopAlertStats();
}

function updateTopAlertStats() {
  let total = 0;
  Object.keys(alertLayerGroups).forEach(k => {
    if (alertLayerVisibility[k]) {
      const el = document.getElementById(`count_${k}`);
      if (el) total += parseInt(el.textContent.replace(/,/g, '')) || 0;
    }
  });
  document.getElementById("topStatAlerts").textContent = `🚨 ${total.toLocaleString()} cảnh báo`;
}

// -----------------------------------------------------------------------------
// SELECTION & INSTANT VISUAL EDITING
// -----------------------------------------------------------------------------
function selectAlertForEditing(origType, item, marker) {
  if (tempAddMarker) {
    map.removeLayer(tempAddMarker);
    tempAddMarker = null;
  }
  if (currentInspected && currentInspected.marker && currentInspected.marker.dragging) {
    currentInspected.marker.dragging.disable();
  }

  const cat = item.category || (origType === "camera" ? (item.speed > 0 ? "speed_cam" : "red_light_cam") : "traffic_light");
  const meta = CATEGORY_META[cat] || CATEGORY_META.speed_cam;

  currentInspected = {
    origType: origType,
    id: item.id,
    lat: item.lat,
    lon: item.lon,
    speed: item.speed !== undefined ? item.speed : (item.value || meta.defaultSpeed),
    heading: item.heading || 0,
    category: cat,
    desc: item.desc || meta.name,
    marker: marker,
    is_new: false
  };

  if (marker && marker.dragging) {
    marker.dragging.enable();
  }

  populateEditorUI(currentInspected);
  fetchRoadInspection(item.lat, item.lon, item.heading || 0);
}

function populateEditorUI(data) {
  const inspector = document.getElementById("hudInspector");
  inspector.style.display = "flex";

  const meta = CATEGORY_META[data.category] || CATEGORY_META.speed_cam;
  document.getElementById("inspBadgeIcon").textContent = meta.icon;
  document.getElementById("inspTypeBadge").textContent = meta.name.toUpperCase();
  document.getElementById("inspDatasetTag").textContent = meta.type === "camera" ? "📁 cameras.csv" : "📁 signs.bin";

  document.querySelectorAll(".cat-card").forEach(c => {
    c.classList.toggle("active", c.getAttribute("data-cat") === data.category);
  });

  const speedSec = document.getElementById("speedSection");
  if (meta.hasSpeed) {
    speedSec.style.display = "block";
    document.getElementById("editSpeed").value = data.speed;
    highlightSpeedPill(data.speed);
  } else {
    speedSec.style.display = "none";
    document.getElementById("editSpeed").value = 0;
  }

  document.getElementById("editHeading").value = data.heading;
  document.getElementById("dispHeadingVal").textContent = `${data.heading}°`;
  document.getElementById("editDesc").value = data.desc || "";
  document.getElementById("editLat").value = data.lat.toFixed(7);
  document.getElementById("editLon").value = data.lon.toFixed(7);
  document.getElementById("dispCurrentCoords").textContent = `${data.lat.toFixed(6)}, ${data.lon.toFixed(6)}`;
}

function fetchRoadInspection(lat, lon, heading=0) {
  fetch(`/api/inspect?lat=${lat}&lon=${lon}&heading=${heading}`)
    .then(r => r.json())
    .then(res => {
      latestInspectionResult = res;
      if (res.segment) {
        document.getElementById("inspRoadName").textContent = res.segment.name || "(Đường chưa đặt tên / Đường nội bộ)";
        document.getElementById("inspSegId").textContent = `#${res.segment.id}`;
        document.getElementById("inspRoadClass").textContent = res.road_class_name;
        document.getElementById("dispSegIdText").textContent = res.segment.id;
        document.getElementById("dispScopeRoadName").textContent = res.segment.name || "Đoạn này";
        selectNewRoadClass(res.segment.road_class);

        // Check if junction banner should appear
        fetch(`/api/detect_junction?lat=${lat}&lon=${lon}&radius=30`)
          .then(r => r.ok ? r.json() : null)
          .then(junc => {
            const b = document.getElementById("junctionDetectedBanner");
            if (junc && junc.arms_count >= 3) {
              b.style.display = "flex";
              document.getElementById("junctionBannerType").textContent = `${junc.arms_count} nhánh (${junc.type_name})`;
            } else {
              b.style.display = "none";
            }
          })
          .catch(() => { document.getElementById("junctionDetectedBanner").style.display = "none"; });
        document.getElementById("inspRoadLimit").textContent = res.segment.speed_limit > 0 ? `${res.segment.speed_limit} km/h` : "Mặc định";
        document.getElementById("inspDist").textContent = res.offset_meters;

        const pill = document.getElementById("inspStatus");
        pill.className = `status-pill ${res.status === 'optimal' ? 'green' : (res.status === 'acceptable' ? 'yellow' : 'red')}`;
        pill.textContent = res.status === 'optimal' ? 'CHUẨN LỀ ĐƯỜNG' : (res.status === 'acceptable' ? 'LỀ RỘNG / GANTRY' : 'LỆCH CAO (>15M)');

        drawConnectorLine([lat, lon], res.projection);
      }
    })
    .catch(err => console.error("Inspection error:", err));
}

function inspectCoordinates(lat, lon, heading=0, label="ĐIỂM TRÊN BẢN ĐỒ", sourceName="") {
  fetchRoadInspection(lat, lon, heading);
  const inspector = document.getElementById("hudInspector");
  inspector.style.display = "flex";
  document.getElementById("inspBadgeIcon").textContent = "📍";
  document.getElementById("inspTypeBadge").textContent = label;
  document.getElementById("inspDatasetTag").textContent = sourceName;
}

function inspectRoadSegment(seg) {
  closeInspector();
  fetchRoadInspection((seg.start[0] + seg.end[0])/2, (seg.start[1] + seg.end[1])/2, seg.heading);
  const inspector = document.getElementById("hudInspector");
  inspector.style.display = "flex";
  document.getElementById("inspBadgeIcon").textContent = "🛣️";
  document.getElementById("inspTypeBadge").textContent = "TIM ĐƯỜNG (ROAD SEGMENT)";
  document.getElementById("inspDatasetTag").textContent = "📁 Nguồn: tiles.bin";
}

function drawConnectorLine(targetLatLng, projLatLng) {
  connectorLayerGroup.clearLayers();
  const connector = L.polyline([targetLatLng, projLatLng], {
    color: "#06b6d4", weight: 3, dashArray: "4, 6", opacity: 1
  });
  const targetDot = L.circleMarker(projLatLng, {
    radius: 5, color: "#06b6d4", fillColor: "#fff", fillOpacity: 1
  });
  connectorLayerGroup.addLayer(connector);
  connectorLayerGroup.addLayer(targetDot);
}

function closeInspector() {
  document.getElementById("hudInspector").style.display = "none";
  connectorLayerGroup.clearLayers();
  if (tempAddMarker) {
    map.removeLayer(tempAddMarker);
    tempAddMarker = null;
  }
  if (currentInspected && currentInspected.marker && currentInspected.marker.dragging) {
    currentInspected.marker.dragging.disable();
  }
  currentInspected = null;
}

// -----------------------------------------------------------------------------
// EDIT CONTROLS: CATEGORY, SPEED, HEADING & SNAPPING
// -----------------------------------------------------------------------------
function setEditCategory(catKey) {
  if (!currentInspected) return;
  currentInspected.category = catKey;
  const meta = CATEGORY_META[catKey] || CATEGORY_META.speed_cam;

  if (meta.hasSpeed && currentInspected.speed === 0) {
    currentInspected.speed = (latestInspectionResult && latestInspectionResult.segment && latestInspectionResult.segment.speed_limit > 0)
      ? latestInspectionResult.segment.speed_limit : meta.defaultSpeed;
  }
  currentInspected.desc = meta.name;
  populateEditorUI(currentInspected);
}

function setEditSpeed(spd) {
  if (!currentInspected) return;
  currentInspected.speed = parseInt(spd);
  document.getElementById("editSpeed").value = spd;
  highlightSpeedPill(spd);
}

function onSpeedCustomInput(val) {
  if (!currentInspected) return;
  const spd = parseInt(val) || 0;
  currentInspected.speed = spd;
  highlightSpeedPill(spd);
}

function highlightSpeedPill(spd) {
  document.querySelectorAll(".speed-pill").forEach(p => {
    p.classList.toggle("active", parseInt(p.textContent) === parseInt(spd));
  });
}

function alignHeadingToRoad(offsetDeg=0) {
  if (!latestInspectionResult || latestInspectionResult.road_bearing === undefined) return;
  let newHd = (Math.round(latestInspectionResult.road_bearing) + offsetDeg) % 360;
  if (newHd < 0) newHd += 360;
  setHeadingValue(newHd);
}

function adjustHeading(deltaDeg) {
  let cur = parseInt(document.getElementById("editHeading").value) || 0;
  let newHd = (cur + deltaDeg) % 360;
  if (newHd < 0) newHd += 360;
  setHeadingValue(newHd);
}

function setHeadingValue(val) {
  if (!currentInspected) return;
  currentInspected.heading = val;
  document.getElementById("editHeading").value = val;
  document.getElementById("dispHeadingVal").textContent = `${val}°`;
  
  if (currentInspected.marker && currentInspected.marker._icon) {
    const arrow = currentInspected.marker._icon.querySelector(".cam-arrow");
    if (arrow) {
      arrow.style.transform = `rotate(${val}deg)`;
    }
  }
}

function snapCurrentPoint(type) {
  if (!latestInspectionResult) {
    alert("Chưa có thông tin tim đường gần nhất để kéo!");
    return;
  }

  let snapPos;
  if (type === 'right' && latestInspectionResult.roadside_snap_right) {
    snapPos = latestInspectionResult.roadside_snap_right;
  } else if (type === 'left' && latestInspectionResult.roadside_snap_left) {
    snapPos = latestInspectionResult.roadside_snap_left;
  } else if (type === 'center' && latestInspectionResult.projection) {
    snapPos = latestInspectionResult.projection;
  }

  if (snapPos && currentInspected) {
    currentInspected.lat = snapPos[0];
    currentInspected.lon = snapPos[1];
    document.getElementById("editLat").value = snapPos[0].toFixed(7);
    document.getElementById("editLon").value = snapPos[1].toFixed(7);
    document.getElementById("dispCurrentCoords").textContent = `${snapPos[0].toFixed(6)}, ${snapPos[1].toFixed(6)}`;

    if (currentInspected.marker) {
      currentInspected.marker.setLatLng(snapPos);
      handleMarkerDragEnd(L.latLng(snapPos[0], snapPos[1]));
    }
  }
}

function handleMarkerDrag(latlng) {
  document.getElementById("editLat").value = latlng.lat.toFixed(7);
  document.getElementById("editLon").value = latlng.lng.toFixed(7);
  document.getElementById("dispCurrentCoords").textContent = `${latlng.lat.toFixed(6)}, ${latlng.lng.toFixed(6)}`;
  
  if (latestInspectionResult && latestInspectionResult.projection) {
    drawConnectorLine([latlng.lat, latlng.lng], latestInspectionResult.projection);
  }
}

function handleMarkerDragEnd(latlng) {
  if (!currentInspected) return;
  currentInspected.lat = latlng.lat;
  currentInspected.lon = latlng.lng;
  fetchRoadInspection(latlng.lat, latlng.lng, currentInspected.heading);
}

// -----------------------------------------------------------------------------
// ONE-CLICK SAVE & PERMANENT FILE WRITER
// -----------------------------------------------------------------------------
function saveCurrentAlertChanges(saveMode='overwrite_with_backup') {
  if (!currentInspected) {
    alert("⚠️ Chưa có điểm cảnh báo nào được chọn!\n\nVui lòng bấm vào bất kỳ vị trí đường nào trên bản đồ để cắm điểm cần thêm trước khi bấm Lưu.");
    return;
  }

  const lat = parseFloat(document.getElementById("editLat").value);
  const lon = parseFloat(document.getElementById("editLon").value);
  const speed = parseInt(document.getElementById("editSpeed").value) || 0;
  const heading = parseInt(document.getElementById("editHeading").value) || 0;
  const category = currentInspected.category;
  const desc = document.getElementById("editDesc").value.trim();

  if (isNaN(lat) || isNaN(lon)) {
    alert("⚠️ Toạ độ không hợp lệ! Vui lòng bấm vào bản đồ để chọn toạ độ.");
    return;
  }

  showLoader(true);

  const payload = {
    orig_type: currentInspected.origType || "sign",
    id: currentInspected.id,
    lat: lat,
    lon: lon,
    speed: speed,
    heading: heading,
    category: category,
    desc: desc,
    seg_id: (latestInspectionResult && latestInspectionResult.segment) ? latestInspectionResult.segment.id : 0
  };

  const endpoint = currentInspected.is_new ? "/api/add_alert" : "/api/update_alert";

  fetch(endpoint, {
    method: "POST",
    headers: { "Content-Type": "application/json" },
    body: JSON.stringify(payload)
  })
  .then(r => r.json())
  .then(res => {
    updateDirtyBadge(res.changes_count);

    if (tempAddMarker) {
      map.removeLayer(tempAddMarker);
      tempAddMarker = null;
    }

    // Immediately persist to disk!
    return fetch("/api/save_all", {
      method: "POST",
      headers: { "Content-Type": "application/json" },
      body: JSON.stringify({ mode: saveMode })
    })
    .then(r => r.json())
    .then(saveRes => {
      alert(`[✓] ĐÃ LƯU THÀNH CÔNG!\n\n${saveRes.results.msg}`);
      closeInspector();
      loadDatasetInfo();
      fetchViewportData();
    });
  })
  .catch(err => alert("Lỗi khi lưu điểm: " + err))
  .finally(() => showLoader(false));
}

function deleteCurrentAlert() {
  if (!currentInspected) return;
  if (!confirm("Bạn có chắc chắn muốn xóa điểm cảnh báo này?")) return;

  showLoader(true);
  fetch("/api/delete_alert", {
    method: "POST",
    headers: { "Content-Type": "application/json" },
    body: JSON.stringify({ type: currentInspected.origType, id: currentInspected.id })
  })
  .then(r => r.json())
  .then(res => {
    updateDirtyBadge(res.changes_count);
    closeInspector();
    fetchViewportData();
  })
  .catch(err => alert("Lỗi khi xóa: " + err))
  .finally(() => showLoader(false));
}

// -----------------------------------------------------------------------------
// SMART ADD ALERT MODE (SELECT TYPE FIRST, THEN 1-CLICK DROP)
// -----------------------------------------------------------------------------

// -----------------------------------------------------------------------------
// ROAD CLASS / HIERARCHY EDITOR
// -----------------------------------------------------------------------------
const ROAD_CLASS_LABELS = {
  0: "Cấp 0: Cao tốc (Motorway)",
  1: "Cấp 1: Quốc lộ (Trunk / National)",
  2: "Cấp 2: Tỉnh lộ (Primary / Provincial)",
  3: "Cấp 3: Đường chính (Secondary)",
  4: "Cấp 4: Đường phụ (Tertiary)",
  5: "Cấp 5: Đô thị / Nhánh (Residential)",
  6: "Cấp 6: Nội bộ / Dịch vụ (Service)"
};

function selectNewRoadClass(cls) {
  selectedNewRoadClass = parseInt(cls);
  document.querySelectorAll(".class-pill").forEach(p => {
    p.classList.toggle("active", parseInt(p.getAttribute("data-cls")) === selectedNewRoadClass);
  });
  const lbl = ROAD_CLASS_LABELS[selectedNewRoadClass] || `Cấp ${selectedNewRoadClass}`;
  document.getElementById("dispRoadClassBadge").textContent = lbl;
}

function saveRoadClassChange() {
  if (!latestInspectionResult || !latestInspectionResult.segment) {
    alert("⚠️ Chưa chọn đoạn đường nào để cập nhật cấp độ!");
    return;
  }

  const seg = latestInspectionResult.segment;
  const scopeEl = document.querySelector('input[name="roadClassScope"]:checked');
  const scope = scopeEl ? scopeEl.value : "single";
  const rName = seg.name || "đoạn này";

  const confirmMsg = scope === "named_road"
    ? `Bạn có chắc muốn đổi toàn tuyến đường "${rName}" sang ${ROAD_CLASS_LABELS[selectedNewRoadClass]}?`
    : `Bạn có chắc muốn đổi đoạn đường #${seg.id} sang ${ROAD_CLASS_LABELS[selectedNewRoadClass]}?`;

  if (!confirm(confirmMsg)) return;

  showLoader(true);
  fetch("/api/update_road_class", {
    method: "POST",
    headers: { "Content-Type": "application/json" },
    body: JSON.stringify({
      segment_id: seg.id,
      road_class: selectedNewRoadClass,
      scope: scope,
      lat: (seg.start[0] + seg.end[0]) / 2,
      lon: (seg.start[1] + seg.end[1]) / 2
    })
  })
  .then(r => r.json())
  .then(res => {
    alert(`[✓] ĐÃ CẬP NHẬT CẤP ĐỘ ĐƯỜNG THÀNH CÔNG!\n\n+ Cấp mới: ${ROAD_CLASS_LABELS[selectedNewRoadClass]}\n+ Số đoạn đường đã ghi vào tiles.bin: ${res.updated_count}`);
    seg.road_class = selectedNewRoadClass;
    fetchRoadInspection((seg.start[0] + seg.end[0]) / 2, (seg.start[1] + seg.end[1]) / 2);
    fetchViewportData();
  })
  .catch(err => alert("Lỗi khi cập nhật cấp độ đường: " + err))
  .finally(() => showLoader(false));
}

// -----------------------------------------------------------------------------
// MULTI-ARM INTERSECTION TRAFFIC LIGHT & CAMERA GENERATOR
// -----------------------------------------------------------------------------
function toggleJunctionMode() {
  junctionModeActive = !junctionModeActive;
  const btn = document.getElementById("junctionModeBtn");
  if (junctionModeActive) {
    btn.classList.add("active");
    map.getContainer().style.cursor = "crosshair";
    alert("👉 CHẾ ĐỘ CẮM GIAO LỘ: Hãy nhấp chuột vào tâm ngã ba hoặc ngã tư trên bản đồ!");
  } else {
    btn.classList.remove("active");
    map.getContainer().style.cursor = "";
  }
}

function detectJunctionAtCoords(lat, lon) {
  showLoader(true);
  fetch(`/api/detect_junction?lat=${lat}&lon=${lon}&radius=35`)
    .then(r => {
      if (!r.ok) throw new Error("Không phát hiện ngã 3/ngã 4 nào đủ rõ trong phạm vi này.");
      return r.json();
    })
    .then(data => {
      currentJunctionData = data;
      openJunctionModal(data);
    })
    .catch(err => alert("⚠️ " + err.message))
    .finally(() => showLoader(false));
}

function openJunctionModalForCurrentPoint() {
  if (!latestInspectionResult) return;
  const center = latestInspectionResult.projection || latestInspectionResult.target;
  detectJunctionAtCoords(center[0], center[1]);
}

function openJunctionModal(data) {
  document.getElementById("juncModalTitle").textContent = `Giao lộ: ${data.arms_count} nhánh hội tụ (${data.type_name.toUpperCase()})`;
  document.getElementById("juncModalSub").textContent = `Toạ độ tâm ngã tư: ${data.center[0].toFixed(6)}, ${data.center[1].toFixed(6)}`;
  document.getElementById("juncDistRange").value = 15;
  document.getElementById("dispJuncDist").textContent = "15 mét";
  junctionPreviewDist = 15;

  const container = document.getElementById("juncArmsList");
  container.innerHTML = "";

  data.arms.forEach((arm, i) => {
    const item = document.createElement("div");
    item.className = "junc-arm-item";
    item.id = `juncArmItem_${i}`;
    item.innerHTML = `
      <div class="arm-name-group">
        <input type="checkbox" id="juncArmChk_${i}" checked onchange="toggleArmCheckbox(${i}, this.checked)">
        <span class="arm-badge">Nhánh ${arm.arm_index}</span>
        <b class="text-white">${arm.name}</b>
      </div>
      <div class="arm-heading-val">🧭 ${arm.heading}°</div>
    `;
    container.appendChild(item);
  });

  document.getElementById("junctionModal").style.display = "flex";
  renderJunctionPreviewOnMap();
}

function closeJunctionModal() {
  document.getElementById("junctionModal").style.display = "none";
  clearJunctionPreviewMarkers();
}

function selectJunctionTargetType(typeKey) {
  junctionSelectedType = typeKey;
  document.getElementById("juncTypeTL").classList.toggle("active", typeKey === "traffic_light");
  document.getElementById("juncTypeCam").classList.toggle("active", typeKey === "red_light_cam");
  document.getElementById("juncTypeBoth").classList.toggle("active", typeKey === "both");
  renderJunctionPreviewOnMap();
}

function toggleArmCheckbox(armIdx, checked) {
  const el = document.getElementById(`juncArmItem_${armIdx}`);
  if (el) el.classList.toggle("disabled", !checked);
  renderJunctionPreviewOnMap();
}

function updateJunctionPreviewDist(val) {
  junctionPreviewDist = parseInt(val);
  document.getElementById("dispJuncDist").textContent = `${val} mét`;
  renderJunctionPreviewOnMap();
}

function clearJunctionPreviewMarkers() {
  junctionPreviewMarkers.forEach(m => map.removeLayer(m));
  junctionPreviewMarkers = [];
}

function renderJunctionPreviewOnMap() {
  clearJunctionPreviewMarkers();
  if (!currentJunctionData) return;

  const jCenter = currentJunctionData.center;
  const m_lat = 110540.0;
  const m_lon = 111320.0 * Math.cos(jCenter[0] * Math.PI / 180.0);

  currentJunctionData.arms.forEach((arm, i) => {
    const chk = document.getElementById(`juncArmChk_${i}`);
    if (chk && !chk.checked) return;

    const hd = arm.heading;
    const app_rad = (hd + 180) * Math.PI / 180.0;
    const stop_lat = jCenter[0] + (junctionPreviewDist * Math.cos(app_rad)) / m_lat;
    const stop_lon = jCenter[1] + (junctionPreviewDist * Math.sin(app_rad)) / m_lon;

    const r_rad = (hd + 90.0) * Math.PI / 180.0;
    const post_lat = stop_lat + (3.5 * Math.cos(r_rad)) / m_lat;
    const post_lon = stop_lon + (3.5 * Math.sin(r_rad)) / m_lon;

    const iconChar = junctionSelectedType === "traffic_light" ? "🚦" : (junctionSelectedType === "red_light_cam" ? "📸" : "🚦📸");
    const customIcon = L.divIcon({
      html: `
        <div class="cam-marker-icon" style="width:28px;height:28px;border:2px solid #eab308;box-shadow:0 0 10px #eab308;">
          <span style="font-size:12px;">${iconChar}</span>
          <div class="cam-arrow" style="border-bottom: 7px solid #eab308; transform: rotate(${hd}deg)"></div>
        </div>
      `,
      className: '',
      iconSize: [28, 28],
      iconAnchor: [14, 14]
    });

    const m = L.marker([post_lat, post_lon], { icon: customIcon }).addTo(map);
    junctionPreviewMarkers.push(m);
  });
}

function executeBatchJunctionAlerts() {
  if (!currentJunctionData) return;

  const jCenter = currentJunctionData.center;
  const m_lat = 110540.0;
  const m_lon = 111320.0 * Math.cos(jCenter[0] * Math.PI / 180.0);

  const alerts = [];
  currentJunctionData.arms.forEach((arm, i) => {
    const chk = document.getElementById(`juncArmChk_${i}`);
    if (chk && !chk.checked) return;

    const hd = arm.heading;
    const app_rad = (hd + 180) * Math.PI / 180.0;
    const stop_lat = jCenter[0] + (junctionPreviewDist * Math.cos(app_rad)) / m_lat;
    const stop_lon = jCenter[1] + (junctionPreviewDist * Math.sin(app_rad)) / m_lon;

    const r_rad = (hd + 90.0) * Math.PI / 180.0;
    const post_lat = stop_lat + (3.5 * Math.cos(r_rad)) / m_lat;
    const post_lon = stop_lon + (3.5 * Math.sin(r_rad)) / m_lon;

    if (junctionSelectedType === "traffic_light" || junctionSelectedType === "both") {
      alerts.append ? null : alerts.push({
        category: "traffic_light",
        lat: round7(post_lat),
        lon: round7(post_lon),
        heading: hd,
        speed: 0,
        desc: `Đèn giao thông ngã 4 (${arm.name})`
      });
    }

    if (junctionSelectedType === "red_light_cam" || junctionSelectedType === "both") {
      alerts.push({
        category: "red_light_cam",
        lat: round7(post_lat),
        lon: round7(post_lon),
        heading: hd,
        speed: 0,
        desc: `Camera phạt nguội vượt đèn (${arm.name})`
      });
    }
  });

  if (alerts.length === 0) {
    alert("⚠️ Vui lòng chọn ít nhất một nhánh để cắm!");
    return;
  }

  showLoader(true);
  fetch("/api/batch_add_junction_alerts", {
    method: "POST",
    headers: { "Content-Type": "application/json" },
    body: JSON.stringify({
      alerts: alerts,
      save_mode: "overwrite_with_backup"
    })
  })
  .then(r => r.json())
  .then(res => {
    alert(`[✓] ĐÃ CẮM THÀNH CÔNG BỘ ${alerts.length} ĐIỂM CHO NGÃ TƯ!\n\n${res.save_results.msg}`);
    closeJunctionModal();
    loadDatasetInfo();
    fetchViewportData();
  })
  .catch(err => alert("Lỗi khi cắm giao lộ: " + err))
  .finally(() => showLoader(false));
}

function round7(num) {
  return Math.round(num * 1e7) / 1e7;
}

function toggleAddAlertMode() {
  addAlertMode = !addAlertMode;
  const btn = document.getElementById("addAlertModeBtn");
  const toolbar = document.getElementById("addModeToolbar");

  if (addAlertMode) {
    btn.classList.add("active");
    toolbar.style.display = "flex";
    map.getContainer().style.cursor = "crosshair";
    rulerActive = false;
    document.getElementById("rulerBtn").classList.remove("active");
    closeInspector();
  } else {
    btn.classList.remove("active");
    toolbar.style.display = "none";
    map.getContainer().style.cursor = "";
  }
}

function selectNewAlertType(typeKey, btnEl) {
  selectedNewAlertType = typeKey;
  document.querySelectorAll(".add-type-pill").forEach(p => p.classList.remove("active"));
  if (btnEl) btnEl.classList.add("active");
}

function handleMapClickAddAlert(latlng) {
  toggleAddAlertMode();

  showLoader(true);
  fetch(`/api/inspect?lat=${latlng.lat}&lon=${latlng.lng}&heading=0`)
    .then(r => r.json())
    .then(res => {
      latestInspectionResult = res;

      const placePos = res.roadside_snap_right || [latlng.lat, latlng.lng];
      const roadLimit = res.segment ? res.segment.speed_limit : 60;
      const roadBearing = res.road_bearing ? Math.round(res.road_bearing) : 0;
      const meta = CATEGORY_META[selectedNewAlertType] || CATEGORY_META.traffic_light;

      if (tempAddMarker) {
        map.removeLayer(tempAddMarker);
      }

      const customIcon = L.divIcon({
        html: `<div class="cam-marker-icon editing" style="width: 28px; height: 28px; border: 2px solid #10b981; box-shadow: 0 0 16px #10b981;"><span>${meta.icon}</span></div>`,
        className: '',
        iconSize: [28, 28],
        iconAnchor: [14, 14]
      });

      tempAddMarker = L.marker(placePos, { icon: customIcon, draggable: true }).addTo(map);

      currentInspected = {
        origType: meta.type,
        id: "new",
        lat: placePos[0],
        lon: placePos[1],
        speed: meta.hasSpeed ? roadLimit : 0,
        heading: roadBearing,
        category: selectedNewAlertType,
        desc: meta.name,
        marker: tempAddMarker,
        is_new: true
      };

      tempAddMarker.on("drag", () => handleMarkerDrag(tempAddMarker.getLatLng()));
      tempAddMarker.on("dragend", () => handleMarkerDragEnd(tempAddMarker.getLatLng()));

      populateEditorUI(currentInspected);
      fetchRoadInspection(placePos[0], placePos[1], roadBearing);
    })
    .finally(() => showLoader(false));
}

// -----------------------------------------------------------------------------
// UNIFIED SAVE ALL & EXPORT
// -----------------------------------------------------------------------------
function updateDirtyBadge(cnt) {
  document.getElementById("dirtyChangesBadge").textContent = cnt;
  document.getElementById("modalChangesCount").textContent = `${cnt} chỉnh sửa`;
}

function openSaveModal() {
  document.getElementById("saveModal").style.display = "flex";
}

function closeSaveModal() {
  document.getElementById("saveModal").style.display = "none";
}

function executeSaveAll(mode) {
  showLoader(true);
  fetch("/api/save_all", {
    method: "POST",
    headers: { "Content-Type": "application/json" },
    body: JSON.stringify({ mode })
  })
  .then(r => r.json())
  .then(res => {
    alert(`[✓] ĐÃ LƯU THÀNH CÔNG!\n\n${res.results.msg}`);
    closeSaveModal();
    loadDatasetInfo();
    fetchViewportData();
  })
  .catch(err => alert("Lỗi khi lưu: " + err))
  .finally(() => showLoader(false));
}

function downloadDataset(dataset, format) {
  window.location.href = `/api/export_download?dataset=${dataset}&format=${format}`;
}

// -----------------------------------------------------------------------------
// LAYER TOGGLES (INSTANT 0ms)
// -----------------------------------------------------------------------------
function toggleMasterBasemap(checked) {
  basemapVisible = checked;
  const card = document.getElementById("basemapCard");
  if (checked) {
    card.classList.remove("disabled");
    if (!map.hasLayer(baseLayers[currentBase])) map.addLayer(baseLayers[currentBase]);
  } else {
    card.classList.add("disabled");
    if (map.hasLayer(baseLayers[currentBase])) map.removeLayer(baseLayers[currentBase]);
  }
}

function setBaseMap(key) {
  if (baseLayers[currentBase] && map.hasLayer(baseLayers[currentBase])) {
    map.removeLayer(baseLayers[currentBase]);
  }
  currentBase = key;
  if (basemapVisible && baseLayers[key]) {
    map.addLayer(baseLayers[key]);
  }
  document.querySelectorAll(".basemap-option").forEach(el => el.classList.remove("active"));
  const activeLbl = document.getElementById(`lbl_${key}`);
  if (activeLbl) activeLbl.classList.add("active");
}

function toggleMasterRoads(checked) {
  roadMasterVisible = checked;
  const card = document.getElementById("roadCard");
  if (checked) {
    card.classList.remove("disabled");
    if (!map.hasLayer(roadMasterGroup)) map.addLayer(roadMasterGroup);
  } else {
    card.classList.add("disabled");
    if (map.hasLayer(roadMasterGroup)) map.removeLayer(roadMasterGroup);
  }
}

function toggleRoadClass(cls, checked) {
  roadClassVisibility[cls] = checked;
  const group = roadClassGroups[cls];
  if (checked) {
    if (!roadMasterGroup.hasLayer(group)) roadMasterGroup.addLayer(group);
  } else {
    if (roadMasterGroup.hasLayer(group)) roadMasterGroup.removeLayer(group);
  }
}

function setAllRoadClasses(enable) {
  for (let c = 0; c <= 5; c++) {
    const chk = document.getElementById(`rc_${c}`);
    if (chk) chk.checked = enable;
    toggleRoadClass(c, enable);
  }
}

function toggleAlertLayer(catKey, checked) {
  alertLayerVisibility[catKey] = checked;
  const group = alertLayerGroups[catKey];
  if (checked) {
    if (!map.hasLayer(group)) map.addLayer(group);
  } else {
    if (map.hasLayer(group)) map.removeLayer(group);
  }
  updateTopAlertStats();
}

function setAllAlerts(enable) {
  Object.keys(alertLayerGroups).forEach(k => {
    const chk = document.getElementById(`layer_${k}`);
    if (chk) chk.checked = enable;
    toggleAlertLayer(k, enable);
  });
}

// -----------------------------------------------------------------------------
// RULER & MODALS
// -----------------------------------------------------------------------------
function toggleRuler() {
  rulerActive = !rulerActive;
  const btn = document.getElementById("rulerBtn");
  if (rulerActive) {
    btn.classList.add("active");
    rulerPoints = [];
    rulerLayerGroup.clearLayers();
    map.getContainer().style.cursor = "crosshair";
    addAlertMode = false;
    document.getElementById("addAlertModeBtn").classList.remove("active");
    document.getElementById("addModeToolbar").style.display = "none";
  } else {
    btn.classList.remove("active");
    map.getContainer().style.cursor = "";
  }
}

function handleRulerClick(latlng) {
  rulerPoints.push(latlng);
  const dot = L.circleMarker(latlng, { radius: 6, color: "#f59e0b", fillColor: "#fff", fillOpacity: 1 });
  rulerLayerGroup.addLayer(dot);

  if (rulerPoints.length >= 2) {
    const p1 = rulerPoints[rulerPoints.length - 2];
    const p2 = rulerPoints[rulerPoints.length - 1];
    const line = L.polyline([p1, p2], { color: "#f59e0b", weight: 3, dashArray: "6,6" });
    const dist = p1.distanceTo(p2);
    line.bindTooltip(`${dist.toFixed(1)} mét`, { permanent: true, direction: "center" });
    rulerLayerGroup.addLayer(line);
  }
}

function jumpToHotspot(coordsStr) {
  const parts = coordsStr.split(",");
  const lat = parseFloat(parts[0]);
  const lon = parseFloat(parts[1]);
  const zoom = parseInt(parts[2]) || 15;
  map.setView([lat, lon], zoom);
}

function searchCoordinates() {
  const val = document.getElementById("coordInput").value.trim();
  const parts = val.split(/[\s,]+/);
  if (parts.length >= 2) {
    const lat = parseFloat(parts[0]);
    const lon = parseFloat(parts[1]);
    if (!isNaN(lat) && !isNaN(lon)) {
      map.setView([lat, lon], 16);
      inspectCoordinates(lat, lon, 0, "TOẠ ĐỘ TÌM KIẾM", "Tìm kiếm");
      return;
    }
  }
  alert("Vui lòng nhập định dạng: Vĩ độ, Kinh độ (Ví dụ: 21.0285, 105.8542)");
}

function refreshCurrentView() { fetchViewportData(); }
function showLoader(show) { document.getElementById("hudLoader").style.display = show ? "flex" : "none"; }

function openRoadModal() { document.getElementById("roadModal").style.display = "flex"; }
function closeRoadModal() { document.getElementById("roadModal").style.display = "none"; }
function openCamModal() { document.getElementById("camModal").style.display = "flex"; }
function closeCamModal() { document.getElementById("camModal").style.display = "none"; }
function openSignModal() { document.getElementById("signModal").style.display = "flex"; }
function closeSignModal() { document.getElementById("signModal").style.display = "none"; }

function submitRoadFiles() {
  const tiles = document.getElementById("inputRoadTiles").value.trim();
  const index = document.getElementById("inputRoadIndex").value.trim();
  const names = document.getElementById("inputRoadNames").value.trim();
  showLoader(true);
  fetch("/api/set_road_files", {
    method: "POST",
    headers: { "Content-Type": "application/json" },
    body: JSON.stringify({ tiles, index, names })
  })
  .then(r => r.json())
  .then(() => { closeRoadModal(); loadDatasetInfo(); })
  .finally(() => showLoader(false));
}

function submitCamFile() {
  const path = document.getElementById("inputCamPath").value.trim();
  if (!path) return;
  showLoader(true);
  fetch("/api/set_camera_file", {
    method: "POST",
    headers: { "Content-Type": "application/json" },
    body: JSON.stringify({ path })
  })
  .then(r => r.json())
  .then(() => { closeCamModal(); loadDatasetInfo(); })
  .finally(() => showLoader(false));
}

function submitSignFile() {
  const path = document.getElementById("inputSignPath").value.trim();
  if (!path) return;
  showLoader(true);
  fetch("/api/set_sign_file", {
    method: "POST",
    headers: { "Content-Type": "application/json" },
    body: JSON.stringify({ path })
  })
  .then(r => r.json())
  .then(() => { closeSignModal(); loadDatasetInfo(); })
  .finally(() => showLoader(false));
}
