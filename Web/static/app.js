const API_BASE = "";  // empty = same origin (Flask serves frontend)

// ===== STATE =====
let allLogs = [];
let allUsers = [];
let currentFilter = "all";
let capturedImage = null;
let stream = null;
let currentDeleteTarget = null;
let emergencyTarget = "door1"; // which door the emergency is for
let vaultTimerInterval = null;     // interval cho định kỳ cập nhật thời gian mở két
let vaultStartTimestamp = 0;       // timestamp bắt đầu mở két

// ===== INIT =====
document.addEventListener("DOMContentLoaded", () => {
  initNavigation();
  loadStats();
  loadRecentLogs();
  loadUsers();
  loadLogs();
  startAutoRefresh();
  connectSSE();
});

// ===== SSE — REAL-TIME UPDATE =====
let sseSource = null;

function connectSSE() {
  if (sseSource) sseSource.close();

  sseSource = new EventSource("/events");

  sseSource.addEventListener("connected", () => {
    console.log("[SSE] Kết nối real-time thành công ✅");
    document.getElementById("server-status-dot").className = "status-indicator online";
    document.getElementById("server-status-text").textContent = "Live · Real-time";
  });

  // Nhận diện kếuôn mặt từ AI
  sseSource.addEventListener("recognize", (e) => {
    const data = JSON.parse(e.data);
    const { user, status, time, vault_open, confidence } = data;
    console.log("[SSE] Nhận diện:", data);

    if (status === "granted") {
      showToast("success", `🔓 <b>${user}</b> mở két lúc ${time} (${confidence||0}%)`, 6000);
      updateDoorStatus("door2", "open", user);
      setTimeout(() => updateDoorStatus("door2", "closed"), 30000);
    } else {
      showToast("warning", `⚠️ Nhận diện thất bại lúc ${time}`, 5000);
    }
    loadStats(); loadRecentLogs(); loadLogs();
    const activePage = document.querySelector(".page.active");
    if (activePage && activePage.id === "page-dashboard") flashDashboard(status);
  });

  // Vault session update
  sseSource.addEventListener("vault_update", (e) => {
    const data = JSON.parse(e.data);
    console.log("[SSE] Vault update:", data);
    
    if (data.open) {
      // Két mở thành công
      dismissFaceAuth();
      updateDoorStatus("door1", "open");
      updateDoorStatus("door2", "open", data.member);
      showToast("info", `🔓 Két mở bởi: <b>${data.member}</b>`, 6000);
    } else if (data.closed_by) {
      const mins = Math.floor((data.elapsed_sec || 0) / 60);
      showToast("success", `✅ Két đã tự động khóa`, 8000);
      updateDoorStatus("door1", "closed");
      updateDoorStatus("door2", "closed");
    }
    loadRecentLogs(); loadLogs();
  });

  // Cảnh báo xâm nhập
  sseSource.addEventListener("intrusion_alert", (e) => {
    const data = JSON.parse(e.data);
    console.log("[SSE] Cảnh báo xâm nhập!", data);
    showIntrusionAlert(data);
    loadLogs(); loadStats();
  });

  // Yêu cầu quét mặt từ STM32 (PIN đúng)
  sseSource.addEventListener("face_request", (e) => {
    console.log("[SSE] PIN OK → Yêu cầu Face ID!");
    showToast("info", "✅ Mã PIN chính xác! Vui lòng xác thực khuôn mặt.", 6000);
    updateDoorStatus("door1", "open");
    showFaceAuthPanel();
  });

  // Kết quả đăng ký vân tay
  sseSource.addEventListener("enroll_result", (e) => {
    const data = JSON.parse(e.data);
    console.log("[SSE] Kết quả đăng ký vân tay:", data);
    const statusEl = document.getElementById("fp-status");
    const iconEl = document.getElementById("fp-status-icon");
    const textEl = document.getElementById("fp-status-text");
    const subEl = document.getElementById("fp-status-sub");
    const btn = document.getElementById("fp-enroll-btn");

    if (statusEl) statusEl.style.display = "block";
    if (btn) btn.disabled = false;

    if (data.status === "ok") {
      if (iconEl) iconEl.textContent = "✅";
      if (textEl) textEl.textContent = `Đăng ký thành công! ID: ${data.finger_id}`;
      if (subEl) subEl.textContent = "Vân tay đã được lưu vào cảm biến.";
      if (statusEl) { statusEl.style.background = "rgba(34,197,94,0.08)"; statusEl.style.borderColor = "rgba(34,197,94,0.2)"; }
      showToast("success", `🖐️ Đăng ký vân tay ID ${data.finger_id} thành công!`, 6000);
    } else {
      if (iconEl) iconEl.textContent = "❌";
      if (textEl) textEl.textContent = "Đăng ký thất bại!";
      if (subEl) subEl.textContent = data.reason || "Vui lòng thử lại.";
      if (statusEl) { statusEl.style.background = "rgba(239,68,68,0.08)"; statusEl.style.borderColor = "rgba(239,68,68,0.2)"; }
      showToast("error", `❌ Đăng ký vân tay thất bại: ${data.reason || "Unknown"}`, 6000);
    }
  });

  sseSource.onerror = () => {
    console.warn("[SSE] Mất kết nối, thử lại sau 3s...");
    document.getElementById("server-status-dot").className = "status-indicator offline";
    document.getElementById("server-status-text").textContent = "Mất kết nối";
    sseSource.close();
    setTimeout(connectSSE, 3000);
  };
}

// ===== FINGERPRINT ENROLLMENT =====
async function startFingerprintEnroll() {
  const fpId = document.getElementById("fp-id").value;
  const label = document.getElementById("fp-label").value || "Unknown";
  const btn = document.getElementById("fp-enroll-btn");
  const statusEl = document.getElementById("fp-status");
  const iconEl = document.getElementById("fp-status-icon");
  const textEl = document.getElementById("fp-status-text");
  const subEl = document.getElementById("fp-status-sub");

  if (fpId === "" || fpId === null) {
    showToast("warning", "Vui lòng nhập ID vân tay (0-126)");
    return;
  }

  btn.disabled = true;
  if (statusEl) statusEl.style.display = "block";
  if (iconEl) iconEl.textContent = "⏳";
  if (textEl) textEl.textContent = "Đang gửi lệnh...";
  if (subEl) subEl.textContent = "Vui lòng đặt ngón tay lên cảm biến trên két sắt.";
  if (statusEl) { statusEl.style.background = "rgba(212,175,55,0.08)"; statusEl.style.borderColor = "rgba(212,175,55,0.2)"; }

  try {
    const res = await fetch("/fingerprint/enroll", {
      method: "POST",
      headers: { "Content-Type": "application/json" },
      body: JSON.stringify({ finger_id: parseInt(fpId), label: label })
    });
    const data = await res.json();

    if (data.status === "ok") {
      if (textEl) textEl.textContent = "Lệnh đã gửi!";
      if (subEl) subEl.textContent = "Đặt ngón tay lên cảm biến trên két → Nhấc ra → Đặt lại lần 2. Chờ kết quả...";
      showToast("info", "🖐️ " + data.message, 8000);
    } else {
      if (iconEl) iconEl.textContent = "❌";
      if (textEl) textEl.textContent = "Lỗi!";
      if (subEl) subEl.textContent = data.message;
      showToast("error", "❌ " + data.message);
      btn.disabled = false;
    }
  } catch (e) {
    if (iconEl) iconEl.textContent = "❌";
    if (textEl) textEl.textContent = "Lỗi kết nối!";
    if (subEl) subEl.textContent = e.message;
    showToast("error", "Lỗi kết nối server!");
    btn.disabled = false;
  }
}

// Cập nhật trạng thái cửa trên dashboard
function updateDoorStatus(door, state, doctorName) {
  if (door === "door1") {
    const badge = document.getElementById("door1-badge");
    const card  = document.getElementById("door1-card");
    if (!badge) return;
    if (state === "open") {
      badge.textContent = "🔓 Đang mở";
      badge.style.background = "rgba(74,222,128,0.15)";
      badge.style.color = "#4ade80";
      card.style.borderColor = "rgba(74,222,128,0.4)";
    } else {
      badge.textContent = "🔒 Đang đóng";
      badge.style.background = "";
      badge.style.color = "";
      card.style.borderColor = "";
    }
  } else {
    const badge  = document.getElementById("door2-badge");
    const card   = document.getElementById("door2-card");
    const docRow = document.getElementById("door2-doctor");
    const docName = document.getElementById("door2-doctor-name");
    if (!badge) return;
    if (state === "open") {
      badge.textContent = "🔓 Đang mở";
      badge.style.background = "rgba(139,92,246,0.2)";
      badge.style.color = "#a78bfa";
      card.style.borderColor = "rgba(139,92,246,0.5)";
      if (doctorName && docRow && docName) {
        docRow.style.display = "block";
        docName.textContent = doctorName;
      }
    } else {
      badge.textContent = "🔒 Đang đóng";
      badge.style.background = "";
      badge.style.color = "";
      card.style.borderColor = "";
      if (docRow) docRow.style.display = "none";
    }
  }
}

// Flash hiệu ứng khi có người quét
function flashDashboard(status) {
  const color = status === "granted"
    ? "rgba(74, 222, 128, 0.08)"
    : "rgba(248, 113, 113, 0.08)";
  const dashboard = document.getElementById("page-dashboard");
  dashboard.style.transition = "background 0.2s";
  dashboard.style.background = color;
  setTimeout(() => { dashboard.style.background = ""; }, 800);
}


// ===== NAVIGATION =====
function initNavigation() {
  document.querySelectorAll(".nav-item").forEach(item => {
    item.addEventListener("click", () => {
      const page = item.getAttribute("data-page");
      switchPage(page);
    });
  });
}

function switchPage(pageName) {
  document.querySelectorAll(".page").forEach(p => p.classList.remove("active"));
  document.querySelectorAll(".nav-item").forEach(n => n.classList.remove("active"));

  const targetPage = document.getElementById("page-" + pageName);
  const targetNav = document.getElementById("nav-" + pageName);
  if (targetPage) targetPage.classList.add("active");
  if (targetNav) targetNav.classList.add("active");

  const titles = {
    dashboard: "Dashboard",
    fingerprint: "Đăng ký Vân tay",
    face: "Đăng ký Khuôn mặt",
    users: "Thành viên",
    logs: "Lịch sử"
  };
  document.getElementById("page-title").textContent = titles[pageName] || pageName;

  if (pageName === "users") loadUsers();
  if (pageName === "logs") loadLogs();
  if (pageName === "dashboard") { loadStats(); loadRecentLogs(); }
}

// ===== REMOTE UNLOCK (Face Auth) =====
let remoteStream = null;
let remoteCdInterval = null;

async function startRemoteUnlock() {
  const idle = document.getElementById("remote-unlock-idle");
  const cam = document.getElementById("remote-unlock-camera");
  if (idle) idle.style.display = "none";
  if (cam) cam.style.display = "block";

  try {
    remoteStream = await navigator.mediaDevices.getUserMedia({ video: { facingMode: "user", width: 320, height: 240 } });
    document.getElementById("remote-video").srcObject = remoteStream;
  } catch (e) {
    showToast("error", "Không thể mở camera!");
    cancelRemoteUnlock();
    return;
  }

  let cd = 3;
  const cdEl = document.getElementById("remote-countdown");
  const statusEl = document.getElementById("remote-status");
  if (cdEl) cdEl.textContent = cd;
  if (statusEl) statusEl.textContent = "Giữ mặt trong khung — tự động chụp...";

  if (remoteCdInterval) clearInterval(remoteCdInterval);
  remoteCdInterval = setInterval(() => {
    cd--;
    if (cdEl) cdEl.textContent = cd;
    if (cd <= 0) {
      clearInterval(remoteCdInterval);
      remoteCdInterval = null;
      captureRemoteUnlock();
    }
  }, 1000);
}

async function captureRemoteUnlock() {
  const video = document.getElementById("remote-video");
  const canvas = document.getElementById("remote-canvas");
  const cdEl = document.getElementById("remote-countdown");
  const statusEl = document.getElementById("remote-status");

  canvas.width = video.videoWidth || 320;
  canvas.height = video.videoHeight || 240;
  canvas.getContext("2d").drawImage(video, 0, 0, canvas.width, canvas.height);
  const b64 = canvas.toDataURL("image/jpeg", 0.8).split(",")[1];

  if (cdEl) cdEl.textContent = "⏳";
  if (statusEl) statusEl.textContent = "Đang xử lý AI...";

  try {
    const res = await fetch("/mobile/scan", {
      method: "POST",
      headers: { "Content-Type": "application/json" },
      body: JSON.stringify({ image: b64 })
    });
    const data = await res.json();
    if (remoteStream) { remoteStream.getTracks().forEach(t => t.stop()); remoteStream = null; }

    if (data.status === "granted") {
      if (cdEl) cdEl.textContent = "✅";
      if (statusEl) statusEl.textContent = `${data.name} — Đang mở két...`;
      showToast("success", `🔓 ${data.name} đã xác thực! Két đang mở.`, 5000);
      setTimeout(() => cancelRemoteUnlock(), 3000);
    } else {
      if (cdEl) cdEl.textContent = "❌";
      if (statusEl) statusEl.textContent = data.reason || "Không nhận diện được";
      showToast("error", `❌ ${data.reason || "Xác thực thất bại"}`, 5000);
      setTimeout(() => cancelRemoteUnlock(), 3000);
    }
  } catch (e) {
    showToast("error", "Lỗi kết nối server!");
    cancelRemoteUnlock();
  }
}

function cancelRemoteUnlock() {
  if (remoteCdInterval) { clearInterval(remoteCdInterval); remoteCdInterval = null; }
  if (remoteStream) { remoteStream.getTracks().forEach(t => t.stop()); remoteStream = null; }
  const idle = document.getElementById("remote-unlock-idle");
  const cam = document.getElementById("remote-unlock-camera");
  if (idle) idle.style.display = "block";
  if (cam) cam.style.display = "none";
}

// ===== AUTO REFRESH =====
function startAutoRefresh() {
  setInterval(() => {
    loadStats();
    loadRecentLogs();
    // quietly update badges
  }, 10000); // every 10s
}

function refreshAll() {
  loadStats();
  loadRecentLogs();
  loadUsers();
  loadLogs();
  showToast("success", "🔄 Đã làm mới dữ liệu!");
}

// ===== OTP GENERATION =====
let otpStream = null;
let otpCountdownInterval = null;

async function startOTPGenerate() {
  const idleEl = document.getElementById("otp-idle");
  const camEl  = document.getElementById("otp-camera");
  const resEl  = document.getElementById("otp-result");
  idleEl.style.display = "none";
  camEl.style.display = "block";
  resEl.style.display = "none";

  try {
    otpStream = await navigator.mediaDevices.getUserMedia({ video: { facingMode: "user", width: 320, height: 240 } });
    document.getElementById("otp-video").srcObject = otpStream;
  } catch (e) {
    showToast("error", "Không thể mở camera!");
    resetOTPPanel();
    return;
  }

  // Tự động đếm ngược 3s rồi chụp
  let countdown = 3;
  const cdBox = document.getElementById("otp-countdown-box");
  const statusText = document.getElementById("otp-status-text");
  if (cdBox) cdBox.textContent = countdown;
  if (statusText) statusText.textContent = "Giữ mặt trong khung — tự động chụp...";

  if (otpCountdownInterval) clearInterval(otpCountdownInterval);
  otpCountdownInterval = setInterval(() => {
    countdown--;
    if (cdBox) cdBox.textContent = countdown;
    if (countdown <= 0) {
      clearInterval(otpCountdownInterval);
      otpCountdownInterval = null;
      captureOTP();
    }
  }, 1000);
}

async function captureOTP() {
  const video  = document.getElementById("otp-video");
  const canvas = document.getElementById("otp-canvas");
  const statusText = document.getElementById("otp-status-text");

  canvas.width = video.videoWidth || 320;
  canvas.height = video.videoHeight || 240;
  const ctx = canvas.getContext("2d");
  ctx.drawImage(video, 0, 0, canvas.width, canvas.height);
  const b64 = canvas.toDataURL("image/jpeg", 0.8).split(",")[1];

  if (statusText) statusText.textContent = "Đang xác thực khuôn mặt...";
  const cdBox = document.getElementById("otp-countdown-box");
  if (cdBox) cdBox.textContent = "⏳";

  try {
    const res = await fetch("/otp/generate", {
      method: "POST",
      headers: { "Content-Type": "application/json" },
      body: JSON.stringify({ image: b64 })
    });
    const data = await res.json();

    // Tắt camera
    if (otpStream) { otpStream.getTracks().forEach(t => t.stop()); otpStream = null; }

    if (data.status === "ok") {
      showOTPResult(data.otp, data.expires_in || 300);
      showToast("success", `🔑 Mã OTP đã tạo bởi ${data.creator}`);
    } else {
      showToast("error", `❌ ${data.message || "Xác thực thất bại"}`);
      resetOTPPanel();
    }
  } catch (e) {
    showToast("error", "Lỗi kết nối server!");
    resetOTPPanel();
  }
}

function showOTPResult(code, expiresSec) {
  const idleEl = document.getElementById("otp-idle");
  const camEl  = document.getElementById("otp-camera");
  const resEl  = document.getElementById("otp-result");
  idleEl.style.display = "none";
  camEl.style.display = "none";
  resEl.style.display = "block";

  document.getElementById("otp-code-display").textContent = code;

  let remaining = expiresSec;
  const cdEl = document.getElementById("otp-countdown");
  if (otpCountdownInterval) clearInterval(otpCountdownInterval);
  otpCountdownInterval = setInterval(() => {
    remaining--;
    if (remaining <= 0) {
      clearInterval(otpCountdownInterval);
      cdEl.textContent = "Đã hết hạn!";
      cdEl.style.color = "var(--red)";
      setTimeout(resetOTPPanel, 2000);
      return;
    }
    const m = Math.floor(remaining / 60);
    const s = String(remaining % 60).padStart(2, "0");
    cdEl.textContent = `Hết hạn sau: ${m}:${s}`;
  }, 1000);
}

function cancelOTP() {
  if (otpStream) { otpStream.getTracks().forEach(t => t.stop()); otpStream = null; }
  resetOTPPanel();
}

function resetOTPPanel() {
  if (otpStream) { otpStream.getTracks().forEach(t => t.stop()); otpStream = null; }
  if (otpCountdownInterval) { clearInterval(otpCountdownInterval); otpCountdownInterval = null; }
  const idleEl = document.getElementById("otp-idle");
  const camEl  = document.getElementById("otp-camera");
  const resEl  = document.getElementById("otp-result");
  if (idleEl) idleEl.style.display = "block";
  if (camEl)  camEl.style.display = "none";
  if (resEl)  resEl.style.display = "none";
  const cdEl = document.getElementById("otp-countdown");
  if (cdEl) { cdEl.textContent = "Hết hạn sau: 5:00"; cdEl.style.color = "var(--text-muted)"; }
}

// ===== INTRUSION ALERT =====
function showIntrusionAlert(data) {
  const panel = document.getElementById("intrusion-panel");
  if (!panel) return;
  const timeEl = document.getElementById("intrusion-time");
  const msgEl  = document.getElementById("intrusion-msg");
  if (timeEl) timeEl.textContent = data.time || "";
  if (msgEl)  msgEl.textContent  = data.message || "Phát hiện xâm nhập!";
  panel.style.display = "block";
  showToast("error", `🚨 Cảnh báo xâm nhập két sắt! Lúc ${data.time}`, 10000);
}

function dismissIntrusion() {
  const panel = document.getElementById("intrusion-panel");
  if (panel) panel.style.display = "none";
}

// ===== FACE AUTH PANEL (thay thế mobile-link-card) =====
let faceAuthTimerInterval = null;
let faceAuthCountdown = 15;

function showFaceAuthPanel() {
  const panel = document.getElementById("face-auth-panel");
  if (!panel) return;
  panel.style.display = "block";
  // Reset về trạng thái idle
  const idle = document.getElementById("face-auth-idle");
  const cam = document.getElementById("face-auth-camera");
  if (idle) idle.style.display = "block";
  if (cam) cam.style.display = "none";

  faceAuthCountdown = 15;

  // Cập nhật timer mỗi giây
  clearInterval(faceAuthTimerInterval);
  const timerEl = document.getElementById("face-auth-timer");
  faceAuthTimerInterval = setInterval(() => {
    faceAuthCountdown--;
    if (timerEl) timerEl.textContent = `Còn ${faceAuthCountdown}s`;
    if (faceAuthCountdown <= 0) {
      dismissFaceAuth();
      showToast("warning", "⏱ Hết thời gian xác thực khuôn mặt.", 5000);
    }
  }, 1000);
}

let faceAuthStream = null;
let faceAuthCdInterval = null;

async function startFaceAuthInline() {
  const idle = document.getElementById("face-auth-idle");
  const cam = document.getElementById("face-auth-camera");
  if (idle) idle.style.display = "none";
  if (cam) cam.style.display = "block";

  try {
    faceAuthStream = await navigator.mediaDevices.getUserMedia({ video: { facingMode: "user", width: 320, height: 240 } });
    document.getElementById("face-auth-video").srcObject = faceAuthStream;
  } catch (e) {
    showToast("error", "Không thể mở camera!");
    dismissFaceAuth();
    return;
  }

  // Đếm ngược 3s rồi tự động chụp
  let cd = 3;
  const cdEl = document.getElementById("face-auth-countdown");
  const statusEl = document.getElementById("face-auth-status");
  if (cdEl) cdEl.textContent = cd;
  if (statusEl) statusEl.textContent = "Giữ mặt trong khung — tự động chụp...";

  if (faceAuthCdInterval) clearInterval(faceAuthCdInterval);
  faceAuthCdInterval = setInterval(() => {
    cd--;
    if (cdEl) cdEl.textContent = cd;
    if (cd <= 0) {
      clearInterval(faceAuthCdInterval);
      faceAuthCdInterval = null;
      captureFaceAuthInline();
    }
  }, 1000);
}

async function captureFaceAuthInline() {
  const video = document.getElementById("face-auth-video");
  const canvas = document.getElementById("face-auth-canvas");
  const statusEl = document.getElementById("face-auth-status");
  const cdEl = document.getElementById("face-auth-countdown");

  canvas.width = video.videoWidth || 320;
  canvas.height = video.videoHeight || 240;
  const ctx = canvas.getContext("2d");
  ctx.drawImage(video, 0, 0, canvas.width, canvas.height);
  const b64 = canvas.toDataURL("image/jpeg", 0.8).split(",")[1];

  if (cdEl) cdEl.textContent = "⏳";
  if (statusEl) statusEl.textContent = "Đang xử lý AI...";

  try {
    const res = await fetch("/mobile/scan", {
      method: "POST",
      headers: { "Content-Type": "application/json" },
      body: JSON.stringify({ image: b64 })
    });
    const data = await res.json();

    if (faceAuthStream) { faceAuthStream.getTracks().forEach(t => t.stop()); faceAuthStream = null; }

    if (data.status === "granted") {
      if (cdEl) cdEl.textContent = "✅";
      if (statusEl) statusEl.textContent = `Thành công! ${data.name} (${data.confidence}%)`;
      showToast("success", `🔓 ${data.name} đã xác thực thành công!`, 5000);
      setTimeout(() => dismissFaceAuth(), 2000);
    } else {
      if (cdEl) cdEl.textContent = "❌";
      if (statusEl) statusEl.textContent = data.reason || "Không nhận diện được";
      showToast("error", `❌ ${data.reason || "Xác thực thất bại"}`, 5000);
      setTimeout(() => {
        // Cho phép thử lại
        const idle = document.getElementById("face-auth-idle");
        const cam = document.getElementById("face-auth-camera");
        if (idle) idle.style.display = "block";
        if (cam) cam.style.display = "none";
      }, 2000);
    }
  } catch (e) {
    showToast("error", "Lỗi kết nối server!");
    dismissFaceAuth();
  }
}

function dismissFaceAuth() {
  clearInterval(faceAuthTimerInterval);
  if (faceAuthCdInterval) { clearInterval(faceAuthCdInterval); faceAuthCdInterval = null; }
  if (faceAuthStream) { faceAuthStream.getTracks().forEach(t => t.stop()); faceAuthStream = null; }
  const panel = document.getElementById("face-auth-panel");
  if (panel) panel.style.display = "none";
}

// ===== API FUNCTIONS =====
async function apiFetch(url, options = {}) {
  try {
    const res = await fetch(API_BASE + url, {
      headers: { "Content-Type": "application/json" },
      ...options
    });
    return await res.json();
  } catch (err) {
    console.error("API Error:", err);
    return null;
  }
}

// ===== STATS =====
async function loadStats() {
  const data = await apiFetch("/stats");
  if (!data) return;

  animateNumber("total-users", data.total_users || 0);
  animateNumber("total-access", data.total_access || 0);
  animateNumber("total-granted", data.granted || 0);
  animateNumber("total-denied", data.denied || 0);

  // Update badges
  document.getElementById("users-badge").textContent = data.total_users || 0;
  document.getElementById("logs-badge").textContent = data.total_access || 0;

  // Update server status
  document.getElementById("server-status-dot").className = "status-indicator online";
  document.getElementById("server-status-text").textContent = "Máy chủ online";
}

function animateNumber(id, target) {
  const el = document.getElementById(id);
  if (!el) return;
  const current = parseInt(el.textContent) || 0;
  if (current === target) return;

  const diff = target - current;
  const steps = 20;
  const step = diff / steps;
  let count = current;
  let i = 0;

  const timer = setInterval(() => {
    count += step;
    i++;
    el.textContent = Math.round(count);
    el.classList.add("counting");
    setTimeout(() => el.classList.remove("counting"), 200);
    if (i >= steps) {
      clearInterval(timer);
      el.textContent = target;
    }
  }, 20);
}

// ===== RECENT LOGS (Dashboard) - chỉ hiện bác sĩ phòng mổ =====
async function loadRecentLogs() {
  const data = await apiFetch("/logs");
  if (!data) return;

  const container = document.getElementById("recent-logs");
  // Chỉ lấy log access phòng mổ (type = access, granted)
  const recent = data
    .filter(l => l.type === "access" && l.status === "granted")
    .slice(0, 6);

  if (recent.length === 0) {
    container.innerHTML = `
      <div class="empty-state">
        <div class="empty-icon">🔐</div>
        <p>Chưa có lần mở két nào</p>
      </div>`;
    return;
  }

  container.innerHTML = recent.map(log => createRecentLogHTML(log)).join("");
}

function createRecentLogHTML(log) {
  const initial = (log.user || "?")[0].toUpperCase();
  return `
    <div class="recent-log-item">
      <div class="log-avatar" style="background:linear-gradient(135deg,#D4AF37,#9A7D0A)">${initial}</div>
      <div class="log-info">
        <div class="log-name">🔑 ${escapeHtml(log.user || "Unknown")}</div>
        <div class="log-time">${formatTime(log.time)}</div>
      </div>
      <span class="badge badge-granted">✓ Mở két</span>
    </div>`;
}

// ===== USERS =====
async function loadUsers() {
  const data = await apiFetch("/users");
  if (!data) return;
  allUsers = data;
  renderUsers(data);
  // Load fingerprints too
  loadFingerprints();
}

// ===== FINGERPRINT DATA =====
let allFingerprints = [];

async function loadFingerprints() {
  try {
    const res = await fetch("/fingerprints");
    const data = await res.json();
    allFingerprints = data;
    renderFingerprints(data);
  } catch (e) {
    console.error("Load fingerprints error:", e);
  }
}

function renderFingerprints(fps) {
  const container = document.getElementById("fingerprints-grid");
  if (!container) return;

  if (!fps || fps.length === 0) {
    container.innerHTML = `<div class="empty-state" style="grid-column:1/-1;"><div class="empty-icon">🖐️</div><p>Chưa có vân tay nào</p><button class="btn btn-sm btn-primary" onclick="switchPage('fingerprint')">+ Đăng ký ngay</button></div>`;
    return;
  }

  container.innerHTML = fps.map(fp => `
    <div class="user-card" style="border-left:3px solid var(--green-400);">
      <div class="user-avatar" style="background:linear-gradient(135deg,#22c55e,#16a34a);color:#fff;font-size:20px;">🖐️</div>
      <div class="user-name">${escapeHtml(fp.label || 'Fingerprint_' + fp.id)}</div>
      <div class="user-reg-date">🆔 Slot ID: ${fp.id}</div>
      <div class="user-reg-date">📅 ${formatDateShort(fp.registered_at)}</div>
      <div class="user-card-actions">
        <button class="btn btn-sm btn-danger" onclick="deleteFingerprint(${fp.id})">🗑 Xóa</button>
      </div>
    </div>
  `).join("");
}

async function deleteFingerprint(fpId) {
  if (!confirm(`Xóa vân tay ID ${fpId} khỏi danh sách?\n(Lưu ý: Vân tay vẫn còn trên chip AS608)`)) return;
  try {
    const res = await fetch(`/fingerprints/${fpId}`, { method: "DELETE" });
    const data = await res.json();
    if (data.status === "success") {
      showToast("success", `🗑 Đã xóa vân tay ID ${fpId}`);
      loadFingerprints();
    } else {
      showToast("error", data.message || "Lỗi xóa");
    }
  } catch (e) {
    showToast("error", "Lỗi kết nối server!");
  }
}

function renderUsers(users) {
  const grid = document.getElementById("users-grid");

  if (users.length === 0) {
    grid.innerHTML = `
      <div class="empty-state" style="grid-column: 1/-1;">
        <div class="empty-icon">👥</div>
        <p>Chưa có người dùng nào</p>
        <button class="btn btn-sm btn-primary" onclick="switchPage('register')">+ Đăng ký ngay</button>
      </div>`;
    return;
  }

  grid.innerHTML = users.map(user => createUserCardHTML(user)).join("");
}

function createUserCardHTML(user) {
  const initial = (user.name || "?")[0].toUpperCase();
  const lastAccess = user.last_access || "Chưa truy cập";

  let avatarHTML = initial;
  if (user.avatar && user.avatar.length > 50) {
    avatarHTML = `<img src="data:image/jpeg;base64,${user.avatar}" alt="${escapeHtml(user.name)}" onerror="this.style.display='none'" />`;
  }

  return `
    <div class="user-card" id="uc-${encodeURIComponent(user.name)}">
      <div class="user-avatar">
        ${avatarHTML}
      </div>
      <div class="user-name">${escapeHtml(user.name)}</div>
      <div class="user-reg-date">📅 ${formatDateShort(user.registered_at)}</div>
      <div class="user-stats">
        <div class="user-stat">
          <div class="user-stat-value">${user.access_count || 0}</div>
          <div class="user-stat-label">Lần vào</div>
        </div>
        <div class="user-stat">
          <div class="user-stat-value" style="font-size:11px;color:var(--text-muted)">${formatTimeShort(user.last_access)}</div>
          <div class="user-stat-label">Lần cuối</div>
        </div>
      </div>
      <div class="user-card-actions">
        <button class="btn btn-sm btn-outline" onclick="viewUserLogs('${escapeHtml(user.name)}')">
          📋 Log
        </button>
        <button class="btn btn-sm btn-danger" onclick="promptDeleteUser('${escapeHtml(user.name)}')">
          🗑 Xóa
        </button>
      </div>
    </div>`;
}

function filterUsers() {
  const query = document.getElementById("user-search").value.toLowerCase();
  const filtered = allUsers.filter(u => u.name.toLowerCase().includes(query));
  renderUsers(filtered);
}

function viewUserLogs(name) {
  switchPage("logs");
  setTimeout(() => {
    const tbody = document.getElementById("logs-tbody");
    const filtered = allLogs.filter(l => l.user === name);
    renderLogsTable(filtered);
  }, 100);
}

// ===== DELETE USER =====
function promptDeleteUser(name) {
  currentDeleteTarget = name;
  document.getElementById("delete-name").textContent = name;
  document.getElementById("delete-modal").classList.add("show");
}

function closeDeleteModal() {
  document.getElementById("delete-modal").classList.remove("show");
  currentDeleteTarget = null;
}

async function confirmDelete() {
  if (!currentDeleteTarget) return;
  const name = currentDeleteTarget;
  closeDeleteModal();

  const data = await apiFetch(`/users/${encodeURIComponent(name)}`, { method: "DELETE" });

  if (data && data.status === "success") {
    showToast("success", `✅ Đã xóa ${name}`);
    loadUsers();
    loadStats();
  } else {
    showToast("error", "❌ Xóa thất bại");
  }
}

// ===== LOGS =====
async function loadLogs() {
  const data = await apiFetch("/logs");
  if (!data) return;
  allLogs = data;
  applyLogFilter();
}

function filterLogs(filter, btn) {
  currentFilter = filter;
  document.querySelectorAll(".filter-btn").forEach(b => b.classList.remove("active"));
  btn.classList.add("active");
  applyLogFilter();
}

function applyLogFilter() {
  let filtered = allLogs;
  if (currentFilter !== "all") {
    if (currentFilter === "register") {
      filtered = allLogs.filter(l => l.type === "register");
    } else {
      filtered = allLogs.filter(l => l.status === currentFilter);
    }
  }
  renderLogsTable(filtered);
}

function renderLogsTable(logs) {
  const tbody = document.getElementById("logs-tbody");

  const filtered = logs.filter(l => l.type === "access");

  if (filtered.length === 0) {
    tbody.innerHTML = `
      <tr><td colspan="4">
        <div class="empty-state">
          <div class="empty-icon">🔐</div>
          <p>Chưa có dữ liệu mở két</p>
        </div>
      </td></tr>`;
    return;
  }

  tbody.innerHTML = filtered.map((log, i) => {
    const badgeClass = log.status === "granted" ? "badge-granted" : "badge-denied";
    const badgeText  = log.status === "granted" ? "✓ Vào" : "✗ Từ chối";
    const initial = (log.user || "?")[0].toUpperCase();

    return `
      <tr class="row-enter">
        <td class="log-num">${i + 1}</td>
        <td>
          <div class="log-user-cell">
            <div class="icon" style="background:linear-gradient(135deg,#D4AF37,#9A7D0A)">${initial}</div>
            <span>🔑 ${escapeHtml(log.user || "Unknown")}</span>
          </div>
        </td>
        <td><span class="badge ${badgeClass}">${badgeText}</span></td>
        <td class="log-time-cell">${formatTime(log.time)}</td>
      </tr>`;
  }).join("");
}

async function clearLogs() {
  if (!confirm("Xóa toàn bộ lịch sử? Hành động không thể hoàn tác!")) return;
  const data = await apiFetch("/logs/clear", { method: "DELETE" });
  if (data && data.status === "success") {
    allLogs = [];
    renderLogsTable([]);
    showToast("success", "🗑 Đã xóa toàn bộ log");
    loadStats();
  }
}

// ===== CAMERA (với polyfill cho Pi / non-HTTPS) =====
let capturedImages = [];        // Mảng chứa nhiều ảnh base64
const MAX_CAPTURES = 10;        // Tăng lên 10 để thu đủ mẫu đa dạng cho LBPH

// Hướng dẫn góc chụp theo thứ tự tối ưu cho LBPH
const CAPTURE_HINTS = [
  { label: "Thẳng mặt",        icon: "😐", desc: "Nhìn thẳng vào camera, ánh sáng bình thường" },
  { label: "Nghư trái 15°",     icon: "↖️",  desc: "Xoay mặt sang trái nhẹ" },
  { label: "Nghư phải 15°",    icon: "↗️",  desc: "Xoay mặt sang phải nhẹ" },
  { label: "Cúi nhẹ",           icon: "⬇️",  desc: "Gật đầu xuống khoảng 10°" },
  { label: "NgỚ nhẹ",           icon: "⬆️",  desc: "NgỚ đầu lên khoảng 10°" },
  { label: "Cười nhẹ",          icon: "🙂",  desc: "Mặt thẳng, biểu cảm tươi" },
  { label: "Sáng từ một bên",  icon: "💡",  desc: "Quay đèn/cửa sổ về một bên trái" },
  { label: "Sáng từ bên kia",  icon: "🔆",  desc: "Quay đèn/cửa sổ về một bên phải" },
  { label: "Giảm ánh sáng",     icon: "🌑",  desc: "Tắt bớt đèn, chụp trong phòng tối hơn" },
  { label: "Tự nhiên nhất",     icon: "🌟",  desc: "ảnh cuối — vị trí và biểu cảm tùy ý" },
];

function _getUserMedia(constraints) {
  // Polyfill: một số trình duyệt cũ / trên Pi không có navigator.mediaDevices
  if (navigator.mediaDevices && navigator.mediaDevices.getUserMedia) {
    return navigator.mediaDevices.getUserMedia(constraints);
  }
  // Fallback cho trình duyệt cũ
  const legacyGetUserMedia = navigator.getUserMedia
    || navigator.webkitGetUserMedia
    || navigator.mozGetUserMedia
    || navigator.msGetUserMedia;
  if (legacyGetUserMedia) {
    return new Promise((resolve, reject) => {
      legacyGetUserMedia.call(navigator, constraints, resolve, reject);
    });
  }
  return Promise.reject(new Error("Trình duyệt không hỗ trợ getUserMedia"));
}

async function startCamera() {
  try {
    // Kiểm tra secure context (HTTPS hoặc localhost)
    if (window.isSecureContext === false) {
      showToast("warning",
        "⚠️ Camera cần HTTPS! Hãy truy cập qua <b>https://</b> hoặc <b>localhost</b>",
        8000);
      console.warn("[CAM] Trang không phải Secure Context → getUserMedia bị chặn.");
      // Vẫn thử tiếp — một số trình duyệt cho phép qua flag
    }

    stream = await _getUserMedia({
      video: { width: 640, height: 480, facingMode: "user" },
      audio: false
    });

    const video = document.getElementById("webcam");
    video.srcObject = stream;
    video.style.display = "block";
    document.getElementById("camera-placeholder").style.display = "none";
    document.getElementById("face-overlay").style.display = "block";
    document.getElementById("camera-actions").style.display = "block";
    document.getElementById("start-cam-btn").style.display = "none";
    document.getElementById("stop-cam-btn").style.display = "inline-flex";

    // Reset bộ đếm chụp
    capturedImages = [];
    updateCaptureCounter();

    showToast("success", "📷 Camera đã bật — Hãy chụp 5 ảnh các góc khác nhau");
  } catch (err) {
    let hint = err.message;
    if (err.name === "NotAllowedError" || err.name === "PermissionDeniedError") {
      hint = "Quyền camera bị từ chối. Vui lòng cấp quyền trong trình duyệt.";
    } else if (err.name === "NotFoundError" || err.name === "DevicesNotFoundError") {
      hint = "Không tìm thấy camera. Kiểm tra kết nối USB Webcam.";
    } else if (!navigator.mediaDevices && !navigator.getUserMedia) {
      hint = "Trình duyệt không hỗ trợ camera. Truy cập bằng HTTPS hoặc localhost.";
    }
    showToast("error", "❌ Không thể truy cập camera: " + hint, 8000);
    console.error("[CAM] getUserMedia error:", err);
  }
}

function stopCamera() {
  if (stream) {
    stream.getTracks().forEach(t => t.stop());
    stream = null;
  }
  document.getElementById("webcam").style.display = "none";
  document.getElementById("camera-placeholder").style.display = "flex";
  document.getElementById("face-overlay").style.display = "none";
  document.getElementById("camera-actions").style.display = "none";
  document.getElementById("start-cam-btn").style.display = "inline-flex";
  document.getElementById("stop-cam-btn").style.display = "none";

  showToast("info", "📷 Camera đã tắt");
}

function updateCaptureCounter() {
  const counter = document.getElementById("capture-counter");
  if (counter) {
    counter.textContent = `${capturedImages.length} / ${MAX_CAPTURES}`;
    counter.style.color = capturedImages.length >= MAX_CAPTURES ? "#4ade80" : "#f59e0b";
  }
  // Cập nhật thanh preview grid
  const grid = document.getElementById("multi-preview-grid");
  if (grid) {
    grid.innerHTML = capturedImages.map((img, i) => `
      <div class="multi-preview-item">
        <img src="data:image/jpeg;base64,${img}" alt="Ảnh ${i + 1}" />
        <span class="multi-preview-num">${i + 1}</span>
        <button class="multi-preview-del" onclick="removeCapture(${i})" title="Xóa">✕</button>
      </div>
    `).join("");
  }
}

function removeCapture(index) {
  capturedImages.splice(index, 1);
  updateCaptureCounter();
  if (capturedImages.length === 0) {
    document.getElementById("step-photo").classList.remove("done");
    document.getElementById("step-photo").querySelector(".step-icon").textContent = "2";
  }
}

function capturePhoto() {
  if (capturedImages.length >= MAX_CAPTURES) {
    showToast("warning", `📸 Đã chụp đủ ${MAX_CAPTURES} ảnh. Nhấn "Xóa hết" nếu muốn chụp lại.`);
    return;
  }

  const video = document.getElementById("webcam");
  const canvas = document.getElementById("snapshot-canvas");
  const ctx = canvas.getContext("2d");

  canvas.width = video.videoWidth;
  canvas.height = video.videoHeight;
  ctx.drawImage(video, 0, 0);

  // Get base64
  const imgBase64 = canvas.toDataURL("image/jpeg", 0.85).split(",")[1];
  capturedImages.push(imgBase64);

  // Flash effect
  const overlay = document.getElementById("face-overlay");
  overlay.style.background = "rgba(255,255,255,0.3)";
  setTimeout(() => { overlay.style.background = "none"; }, 200);

  updateCaptureCounter();

  // Hiện preview grid
  document.getElementById("snapshot-preview").style.display = "block";

  const remaining = MAX_CAPTURES - capturedImages.length;
  if (remaining > 0) {
    const nextHint = CAPTURE_HINTS[capturedImages.length] || { label: "Góc khác", icon: "📸", desc: "Đổi góc hoặc ánh sáng" };
    showToast("info", `📸 Ảnh ${capturedImages.length}/${MAX_CAPTURES} ✓ — Tiếp theo: <b>${nextHint.icon} ${nextHint.label}</b>`, 4000);
  } else {
    showToast("success", `📸 Đã chụp đủ ${MAX_CAPTURES} ảnh! Có thể đăng ký.`);
    // Update step
    document.getElementById("step-photo").classList.add("done");
    document.getElementById("step-photo").querySelector(".step-icon").textContent = "✓";
  }
}

function retakePhoto() {
  capturedImages = [];
  capturedImage = null;
  document.getElementById("snapshot-preview").style.display = "none";
  document.getElementById("step-photo").classList.remove("done");
  document.getElementById("step-photo").querySelector(".step-icon").textContent = "2";
  updateCaptureCounter();
}

// ===== REGISTER USER =====
async function registerUser() {
  const name = document.getElementById("reg-name").value.trim();

  if (!name) {
    showToast("error", "⚠️ Vui lòng nhập tên người dùng");
    document.getElementById("reg-name").focus();
    return;
  }

  if (capturedImages.length < 5) {
    showToast("warning", `📷 Cần chụp ít nhất 5 ảnh đa dạng góc/sáng (đã chụp ${capturedImages.length}/${MAX_CAPTURES})`);
    return;
  }

  const btn = document.getElementById("register-btn");
  btn.disabled = true;
  btn.innerHTML = `<span class="spinner"></span> Đang đăng ký ${capturedImages.length} ảnh...`;

  // Gửi mảng ảnh — backend sẽ lưu tất cả
  const data = await apiFetch("/register", {
    method: "POST",
    body: JSON.stringify({ name, images: capturedImages, image: capturedImages[0] })
  });

  btn.disabled = false;
  btn.innerHTML = `
    <svg viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" width="20" height="20">
      <path d="M16 21v-2a4 4 0 0 0-4-4H6a4 4 0 0 0-4 4v2"/>
      <circle cx="9" cy="7" r="4"/>
      <line x1="19" y1="8" x2="19" y2="14"/>
      <line x1="22" y1="11" x2="16" y2="11"/>
    </svg>
    Đăng ký`;

  if (data && data.status === "success") {
    showToast("success", `✅ ${data.message}`);

    // Complete step
    document.getElementById("step-done").classList.add("done");
    document.getElementById("step-done").querySelector(".step-icon").textContent = "✓";

    // Reset form after 2s
    setTimeout(() => {
      document.getElementById("reg-name").value = "";
      capturedImages = [];
      capturedImage = null;
      document.getElementById("snapshot-preview").style.display = "none";
      document.getElementById("step-photo").classList.remove("done");
      document.getElementById("step-done").classList.remove("done");
      document.getElementById("step-photo").querySelector(".step-icon").textContent = "2";
      document.getElementById("step-done").querySelector(".step-icon").textContent = "3";
      updateCaptureCounter();
    }, 2000);

    loadStats();
  } else {
    const msg = data?.message || "Đăng ký thất bại";
    showToast("error", `❌ ${msg}`);
  }
}

// ===== MANUAL UNLOCK (legacy, không dùng nữa) =====
function manualUnlock() { openEmergency('door1'); }
function closeModal() {}
async function confirmUnlock() {}


// ===== TOAST NOTIFICATION =====
function showToast(type, message, duration = 4000) {
  const container = document.getElementById("toast-container");
  const icons = {
    success: "✅",
    error: "❌",
    warning: "⚠️",
    info: "ℹ️"
  };

  const toast = document.createElement("div");
  toast.className = `toast toast-${type}`;
  toast.innerHTML = `
    <span class="toast-icon">${icons[type] || "ℹ️"}</span>
    <span class="toast-msg">${message}</span>
  `;

  container.appendChild(toast);

  setTimeout(() => {
    toast.classList.add("out");
    setTimeout(() => toast.remove(), 300);
  }, duration);
}

// ===== HELPER FUNCTIONS =====
function getBadgeClass(status) {
  if (status === "granted") return "badge-granted";
  if (status === "denied") return "badge-denied";
  if (status === "registered") return "badge-register";
  if (status === "manual_unlock") return "badge-manual";
  return "badge-register";
}

function getBadgeText(status) {
  if (status === "granted") return "✓ Granted";
  if (status === "denied") return "✗ Denied";
  if (status === "registered") return "📋 Đăng ký";
  return status || "N/A";
}

function getTypeIcon(type) {
  const icons = {
    access: "🚪",
    register: "📝",
    manual_unlock: "🔓"
  };
  return icons[type] || "📌";
}

function formatTime(timeStr) {
  if (!timeStr) return "N/A";
  return timeStr;
}

function formatDateShort(dateStr) {
  if (!dateStr) return "N/A";
  return dateStr.split(" ")[0];
}

function formatTimeShort(dateStr) {
  if (!dateStr || dateStr === "Chưa truy cập") return "—";
  const parts = dateStr.split(" ");
  return parts[1] || parts[0];
}

function escapeHtml(text) {
  const div = document.createElement("div");
  div.appendChild(document.createTextNode(text || ""));
  return div.innerHTML;
}

// ===== KEYBOARD SHORTCUTS =====
document.addEventListener("keydown", e => {
  if (e.key === "Escape") {
    closeModal();
    closeDeleteModal();
    if (window.innerWidth <= 768) {
      document.getElementById("sidebar").classList.remove("open");
    }
  }
  if (e.key === "F5") {
    e.preventDefault();
    refreshAll();
  }
});

// Close modal on overlay click
document.getElementById("delete-modal").addEventListener("click", function(e) {
  if (e.target === this) closeDeleteModal();
});
document.getElementById("emergency-modal").addEventListener("click", function(e) {
  if (e.target === this) closeEmergency();
});

// Make sure modals are hidden on init
document.getElementById("delete-modal").classList.remove("show");

// ===== EMERGENCY UNLOCK =====
const EMERGENCY_COUNTDOWN = 5;
const CIRCUMFERENCE = 2 * Math.PI * 33;
let emergencyTimer  = null;
let emergencyCount  = EMERGENCY_COUNTDOWN;

function openEmergency(door) {
  emergencyTarget = door || "door1";
  emergencyCount = EMERGENCY_COUNTDOWN;

  // Cập nhật tiêu đề modal theo cửa
  const title = document.getElementById("emergency-modal-title");
  const desc  = document.getElementById("emergency-modal-desc");
  if (emergencyTarget === "door2") {
    if (title) title.textContent = "⚠️ Mở khẩn cấp Két Sắt (Lớp 2)";
    if (desc) desc.textContent = "Bypass Face ID và mở két ngay lập tức qua MQTT";
  } else {
    if (title) title.textContent = "⚠️ Mở khẩn cấp Két Sắt (Lớp 1)";
    if (desc)  desc.textContent = "Mở chốt khóa lớp 1 ngay lập tức qua MQTT";
  }

  const numEl     = document.getElementById("countdown-number");
  const circleEl  = document.getElementById("countdown-circle");
  const confirmBtn = document.getElementById("emergency-confirm-btn");
  const cancelBtn  = document.getElementById("emergency-cancel-btn");

  numEl.textContent = emergencyCount;
  circleEl.style.strokeDashoffset = 0;
  confirmBtn.disabled = false;
  cancelBtn.disabled  = false;

  document.getElementById("emergency-modal").classList.add("show");

  clearInterval(emergencyTimer);
  emergencyTimer = setInterval(() => {
    emergencyCount--;
    numEl.textContent = emergencyCount;
    const progress = emergencyCount / EMERGENCY_COUNTDOWN;
    circleEl.style.strokeDashoffset = CIRCUMFERENCE * (1 - progress);
    if (emergencyCount <= 0) {
      clearInterval(emergencyTimer);
      closeEmergency();
      showToast("info", "⏱ Đã hủy tự động — hết thời gian xác nhận");
    }
  }, 1000);
}

function closeEmergency() {
  clearInterval(emergencyTimer);
  document.getElementById("emergency-modal").classList.remove("show");
}

async function confirmEmergency() {
  clearInterval(emergencyTimer);

  const confirmBtn = document.getElementById("emergency-confirm-btn");
  const cancelBtn  = document.getElementById("emergency-cancel-btn");
  confirmBtn.disabled = true;
  cancelBtn.disabled  = true;
  confirmBtn.innerHTML = `<span class="spinner"></span> Đang gửi...`;

  // Gửi đến đúng route tùy theo cửa
  const url = emergencyTarget === "door2" ? "/unlock_door2" : "/unlock";
  const data = await apiFetch(url, { method: "POST", body: JSON.stringify({user: "Khẩn cấp (Web)"}) });

  closeEmergency();

  if (data && data.status === "success") {
    const mqttOk = data.mqtt_sent;
    const doorLabel = emergencyTarget === "door2" ? "Phòng Mổ" : "Cửa ngoài";
    showToast(
      "success",
      mqttOk
        ? `🔓 Đã gửi lệnh MQTT! ${doorLabel} đang mở...`
        : `⚠️ Ghi nhận nhưng MQTT chưa kết nối ESP32`,
      5000
    );
    if (emergencyTarget === "door1") updateDoorStatus("door1", "open");
    if (emergencyTarget === "door2") updateDoorStatus("door2", "open", "Khẩn cấp");
    loadRecentLogs();
    loadStats();
  } else {
    showToast("error", "❌ Không thể gửi lệnh mở khóa");
  }
}
