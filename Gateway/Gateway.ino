/*
 * ══════════════════════════════════════════════════════════════
 *  SafeVault Gateway — ESP32
 *  Giao tiếp UART với STM32  ←→  MQTT với Web Dashboard
 * ══════════════════════════════════════════════════════════════
 *
 * UART (Serial2): RX=16, TX=17 ←→ STM32 PA9/PA10
 * MQTT Broker:    broker.hivemq.com:1883
 *
 * Luồng:
 *   STM32 → "F\n"          → ESP32 → MQTT "request_face"  → Web
 *   STM32 → "LOCKED_TEMP"  → ESP32 → MQTT "temp_lock"     → Web
 *   Web   → MQTT "unlock_door2"    → ESP32 → UART "UNLOCK:<tên>" → STM32
 *   Web   → MQTT "close_vault"     → ESP32 → UART "LOCK"         → STM32
 *   Web   → MQTT "fail_door2"      → ESP32 → UART "FAIL"         → STM32
 *   Web   → MQTT "temp_lock"       → ESP32 → UART "TEMP_LOCK"    → STM32
 */

#include <ArduinoJson.h>
#include <PubSubClient.h>
#include <WiFi.h>

// ══════════════════════════════════════
//  CẤU HÌNH WIFI
// ══════════════════════════════════════
const char *WIFI_SSID = "Xoi Banh My Chi Nga"; // ← Thay SSID WiFi
const char *WIFI_PASSWORD = "13071982";        // ← Thay mật khẩu WiFi

// ══════════════════════════════════════
//  CẤU HÌNH MQTT
// ══════════════════════════════════════
const char *MQTT_BROKER = "broker.hivemq.com";
const int MQTT_PORT = 1883;
const char *MQTT_PREFIX = "smartlock/quang_1307";
String TOPIC_COMMAND;   // smartlock/quang_1307/command   (subscribe)
String TOPIC_STATUS;    // smartlock/quang_1307/status    (publish)
String TOPIC_HEARTBEAT; // smartlock/quang_1307/heartbeat (publish)

WiFiClient wifiClient;
PubSubClient mqttClient(wifiClient);

// ══════════════════════════════════════
//  UART ← STM32
// ══════════════════════════════════════
#define STM32_SERIAL Serial2
#define STM32_BAUD 115200
#define STM32_RX 16 // ESP32 RX ← STM32 TX (PA9)
#define STM32_TX 17 // ESP32 TX → STM32 RX (PA10)

String stm32Buffer = "";
String lastCommand = "";  // Lưu lệnh cuối cùng để gửi lại khi STM32 thức dậy

// ══════════════════════════════════════
//  HEARTBEAT
// ══════════════════════════════════════
unsigned long lastHeartbeat = 0;
const unsigned long HEARTBEAT_INTERVAL = 10000; // 10 giây

// ══════════════════════════════════════
//  SETUP
// ══════════════════════════════════════
void setup() {
  Serial.begin(115200);
  Serial.println("\n[SafeVault Gateway] Khởi động...");

  // UART → STM32
  STM32_SERIAL.begin(STM32_BAUD, SERIAL_8N1, STM32_RX, STM32_TX);
  Serial.println("[UART] Kết nối STM32 @ 115200 baud");

  // MQTT topics
  TOPIC_COMMAND = String(MQTT_PREFIX) + "/command";
  TOPIC_STATUS = String(MQTT_PREFIX) + "/status";
  TOPIC_HEARTBEAT = String(MQTT_PREFIX) + "/heartbeat";

  // WiFi
  connectWiFi();

  // MQTT
  mqttClient.setServer(MQTT_BROKER, MQTT_PORT);
  mqttClient.setCallback(mqttCallback);
  mqttClient.setBufferSize(1024);
  connectMQTT();
}

// ══════════════════════════════════════
//  LOOP
// ══════════════════════════════════════
void loop() {
  // Kiểm tra WiFi
  if (WiFi.status() != WL_CONNECTED) {
    connectWiFi();
  }

  // Kiểm tra MQTT
  if (!mqttClient.connected()) {
    connectMQTT();
  }
  mqttClient.loop();

  // ── Đọc UART từ STM32 ──
  while (STM32_SERIAL.available()) {
    char c = STM32_SERIAL.read();
    if (c == '\n' || c == '\r') {
      if (stm32Buffer.length() > 0) {
        processSTM32Message(stm32Buffer);
        stm32Buffer = "";
      }
    } else {
      stm32Buffer += c;
    }
  }

  // ── Heartbeat ──
  if (millis() - lastHeartbeat >= HEARTBEAT_INTERVAL) {
    lastHeartbeat = millis();
    publishHeartbeat();
  }
}

// ══════════════════════════════════════
//  WIFI
// ══════════════════════════════════════
void connectWiFi() {
  Serial.print("[WiFi] Kết nối đến ");
  Serial.println(WIFI_SSID);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  int attempts = 0;
  while (WiFi.status() != WL_CONNECTED && attempts < 30) {
    delay(500);
    Serial.print(".");
    attempts++;
  }
  if (WiFi.status() == WL_CONNECTED) {
    Serial.println();
    Serial.print("[WiFi] Đã kết nối! IP: ");
    Serial.println(WiFi.localIP());
  } else {
    Serial.println("\n[WiFi] Kết nối thất bại! Thử lại sau...");
  }
}

// ══════════════════════════════════════
//  MQTT
// ══════════════════════════════════════
void connectMQTT() {
  int retries = 0;
  while (!mqttClient.connected() && retries < 5) {
    String clientId = "esp32-safevault-" + String(random(1000, 9999));
    Serial.print("[MQTT] Kết nối broker... ");

    if (mqttClient.connect(clientId.c_str())) {
      Serial.println("OK ✅");
      // Subscribe command topic
      mqttClient.subscribe(TOPIC_COMMAND.c_str());
      Serial.print("[MQTT] Subscribed: ");
      Serial.println(TOPIC_COMMAND);
      // Gửi heartbeat ngay
      publishHeartbeat();
    } else {
      Serial.print("FAILED (rc=");
      Serial.print(mqttClient.state());
      Serial.println(") Thử lại sau 3s...");
      delay(3000);
    }
    retries++;
  }
}

// ══════════════════════════════════════
//  MQTT CALLBACK — Lệnh từ Web Dashboard
// ══════════════════════════════════════
void mqttCallback(char *topic, byte *payload, unsigned int length) {
  // Parse JSON
  char jsonBuf[512];
  if (length >= sizeof(jsonBuf))
    length = sizeof(jsonBuf) - 1;
  memcpy(jsonBuf, payload, length);
  jsonBuf[length] = '\0';

  Serial.print("[MQTT] Nhận [");
  Serial.print(topic);
  Serial.print("]: ");
  Serial.println(jsonBuf);

  StaticJsonDocument<512> doc;
  DeserializationError err = deserializeJson(doc, jsonBuf);
  if (err) {
    Serial.print("[MQTT] JSON parse error: ");
    Serial.println(err.c_str());
    return;
  }

  const char *action = doc["action"];
  const char *user = doc["user"] | "Unknown";

  if (strcmp(action, "unlock_door2") == 0) {
    // Mở két — Gửi tên người qua UART đến STM32
    // Gửi 3 lần: Lần 1 đánh thức STM32 (nếu đang ngủ),
    // Lần 2-3 đảm bảo lệnh đến được sau khi UART khôi phục
    String cmd = "UNLOCK:" + String(user) + "\n";
    lastCommand = cmd;

    for (int i = 0; i < 3; i++) {
      STM32_SERIAL.print(cmd);
      Serial.print("[UART→STM32] Lần ");
      Serial.print(i + 1);
      Serial.print(": ");
      Serial.print(cmd);
      if (i < 2) delay(500);  // Chờ 500ms giữa mỗi lần
    }

    // Publish trạng thái
    publishStatus("unlocked", user);

  } else if (strcmp(action, "close_vault") == 0) {
    // Khóa két
    STM32_SERIAL.print("LOCK\n");
    Serial.println("[UART→STM32] LOCK");
    publishStatus("locked", user);

  } else if (strcmp(action, "fail_door2") == 0) {
    // Xác thực mặt thất bại
    STM32_SERIAL.print("FAIL\n");
    Serial.println("[UART→STM32] FAIL");

  } else if (strcmp(action, "temp_lock") == 0) {
    // Khóa tạm do sai quá nhiều
    STM32_SERIAL.print("TEMP_LOCK\n");
    Serial.println("[UART→STM32] TEMP_LOCK");

  } else if (strcmp(action, "unlock") == 0) {
    STM32_SERIAL.print("UNLOCK:Emergency\n");
    Serial.println("[UART→STM32] UNLOCK:Emergency");
    publishStatus("unlocked", "Emergency");

  } else if (strcmp(action, "set_otp") == 0) {
    // Server gửi mã OTP xuống cho STM32 lưu (không bắt buộc)
    const char *code = doc["code"] | "";
    String cmd = "OTP_SET:" + String(code) + "\n";
    STM32_SERIAL.print(cmd);
    Serial.print("[UART→STM32] ");
    Serial.print(cmd);

  } else if (strcmp(action, "enroll_finger") == 0) {
    // Đăng ký vân tay mới — gửi ID xuống STM32
    int fpId = doc["finger_id"] | 0;
    String cmd = "ENROLL_START:" + String(fpId) + "\n";
    STM32_SERIAL.print(cmd);
    Serial.print("[UART→STM32] ");
    Serial.println(cmd);
    publishStatus("enroll_started", user);
  }
}

// ══════════════════════════════════════
//  XỬ LÝ MESSAGE TỪ STM32
// ══════════════════════════════════════
void processSTM32Message(String msg) {
  msg.trim();
  Serial.print("[STM32→ESP32] ");
  Serial.println(msg);

  if (msg.startsWith("FINGER_OK:")) {
    // Vân tay khớp → két đã tự mở
    String fpId = msg.substring(10);
    Serial.print("[Gateway] Vân tay OK, ID: ");
    Serial.println(fpId);
    String trigger = "Fingerprint_" + fpId;
    publishStatus("unlocked", trigger.c_str());

  } else if (msg == "FINGER_FAIL") {
    Serial.println("[Gateway] Vân tay thất bại 3 lần");
    publishStatus("finger_fail", "stm32");

  } else if (msg == "F") {
    // PIN đúng → yêu cầu quét mặt (mở từ xa)
    Serial.println("[Gateway] PIN OK → request_face");
    publishStatus("request_face", "stm32");

  } else if (msg == "LOCKED_TEMP") {
    // STM32 tạm khóa do nhập sai 3 lần
    Serial.println("[Gateway] STM32 tạm khóa 5s");
    publishStatus("temp_lock_pin", "stm32");

  } else if (msg == "UNLOCK_BYPASS") {
    // STM32 bypass Lớp 2 và đã tự mở khóa
    Serial.println("[Gateway] STM32 Bypass Lớp 2 -> Đã mở khóa");
    publishStatus("unlocked", "PIN_Bypass_L2");

  } else if (msg.indexOf("ACK:") >= 0) {
    int ackIdx = msg.indexOf("ACK:");
    String ackType = msg.substring(ackIdx + 4);
    Serial.print("[Gateway] STM32 ACK: ");
    Serial.println(ackType);

    // STM32 vừa thức dậy → gửi lại lệnh cuối cùng
    if (ackType == "WAKEUP" && lastCommand.length() > 0) {
      Serial.print("[Gateway] Gửi lại lệnh sau wakeup: ");
      Serial.print(lastCommand);
      delay(200);  // Chờ STM32 khởi tạo xong UART và bắt đầu chờ lệnh
      STM32_SERIAL.print(lastCommand);
      lastCommand = "";  // Xóa sau khi gửi
    }

  } else if (msg.startsWith("OTP:")) {
    // Người dùng nhập OTP trên két → gửi lên server xác minh
    String code = msg.substring(4);
    Serial.print("[Gateway] OTP verify: ");
    Serial.println(code);

    StaticJsonDocument<256> otpDoc;
    otpDoc["event"] = "verify_otp";
    otpDoc["code"] = code;
    otpDoc["time"] = millis();
    char otpBuf[256];
    serializeJson(otpDoc, otpBuf);
    mqttClient.publish(TOPIC_STATUS.c_str(), otpBuf);

  } else if (msg.startsWith("ENROLL_OK:")) {
    // Đăng ký vân tay thành công
    String fpId = msg.substring(10);
    Serial.print("[Gateway] Đăng ký vân tay OK, ID: ");
    Serial.println(fpId);
    publishStatus("enroll_ok", fpId.c_str());

  } else if (msg.startsWith("ENROLL_FAIL:")) {
    // Đăng ký vân tay thất bại
    String reason = msg.substring(12);
    Serial.print("[Gateway] Đăng ký vân tay FAIL: ");
    Serial.println(reason);
    publishStatus("enroll_fail", reason.c_str());
  }
}

// ══════════════════════════════════════
//  PUBLISH HELPERS
// ══════════════════════════════════════
void publishStatus(const char *event, const char *trigger) {
  StaticJsonDocument<256> doc;
  doc["event"] = event;
  doc["trigger"] = trigger;
  doc["time"] = millis();

  char buf[256];
  serializeJson(doc, buf);

  if (mqttClient.publish(TOPIC_STATUS.c_str(), buf)) {
    Serial.print("[MQTT] Published status: ");
    Serial.println(buf);
  } else {
    Serial.println("[MQTT] Publish FAILED!");
  }
}

void publishHeartbeat() {
  StaticJsonDocument<128> doc;
  doc["status"] = "alive";
  doc["uptime"] = millis() / 1000;
  doc["wifi_rssi"] = WiFi.RSSI();

  char buf[128];
  serializeJson(doc, buf);
  mqttClient.publish(TOPIC_HEARTBEAT.c_str(), buf);
}
