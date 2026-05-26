/*
 * ESP32-CAM EasyCam Motion Monitor
 * ESP32-CAM EasyCam 感應監視器
 *
 * Features / 功能：
 * 1. Frame difference detection → burst 3 shots, pick best → upload → LINE + Telegram
 *    畫面差異偵測 → 拍3張選最清楚1張 → 上傳雲端 → LINE + Telegram 通知
 * 2. Remote commands via MQTT/HTTP polling → instant capture
 *    MQTT/HTTP 遠端指令 → 即時拍照回傳
 * 3. IO33 LED status indicator / IO33 LED 狀態指示
 *
 * Board: ESP32 Wrover Module (with USB) / 開發板：ESP32 Wrover Module (帶USB)
 * Partition Scheme: Huge APP (3MB No OTA/1MB SPIFFS)
 */

#include "esp_camera.h"
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <PubSubClient.h>
#include <Preferences.h>
#include <WebServer.h>
#include <DNSServer.h>
#include "soc/soc.h"
#include "soc/rtc_cntl_reg.h"
#include "config.h"             // Sensitive config (copy config.example.h → config.h and fill in your values)
                                // 敏感設定（複製 config.example.h → config.h 並填入你的資料）

// ============================
// Load settings from config.h / 從 config.h 載入設定
// ============================
const char* DEFAULT_WIFI_SSID     = WIFI_SSID;
const char* DEFAULT_WIFI_PASSWORD = WIFI_PASSWORD;
const char* AP_SSID     = AP_SSID_NAME;
const char* AP_PASSWORD = AP_PASSWORD_STR;
const char* WORKER_URL  = WORKER_UPLOAD_URL;
const char* WORKER_POLL = WORKER_POLL_URL;
const char* WORKER_STATUS = WORKER_STATUS_URL;
const char* MQTT_BROKER   = MQTT_BROKER_HOST;
const int   MQTT_PORT     = MQTT_BROKER_PORT;
const char* MQTT_USER     = MQTT_USERNAME;
const char* MQTT_PASSWORD = MQTT_PASS;
const char* MQTT_TOPIC_CMD = MQTT_TOPIC;
const char* MQTT_CLIENT_ID = MQTT_CLIENT;

// ============================
// Hardware config (EasyCam breakout board) / 硬體設定（EasyCam 擴充版）
// ============================
#define LED_PIN        33    // NMKCAM LED (not the original IO4) / NMKCAM LED（不是原版的IO4）
#define BUTTON_PIN     3     // Capture button / 拍照按鈕
#define STATUS_LED_CHANNEL 4 // Core 2.x LEDC needs fixed channel, avoid camera XCLK channel 0

// ============================
// Detection parameters (tunable) / 偵測參數（可調整）
// ============================
#define MOTION_THRESHOLD    8     // Pixel diff threshold (0-255) / 像素差異門檻
#define MOTION_PERCENT      3     // Changed block percentage threshold (%) / 變化區塊百分比門檻
#define MOTION_STRONG_DIFF  22    // Strong change threshold, catches small human movement / 強變化門檻
#define MOTION_STRONG_BLOCKS 8    // Trigger if this many strong blocks reached / 強變化區塊數達標即觸發
#define PHOTO_SCORE_SIZE_WEIGHT 0.7     // JPEG size weight (KB) / JPEG 大小權重
#define PHOTO_SCORE_MOTION_WEIGHT 3.0   // Change area ratio weight / 變化區域比例權重
#define PHOTO_SCORE_STRONG_WEIGHT 0.35  // Strong change block weight / 強變化區塊權重
#define COOLDOWN_MS         15000 // Cooldown after alarm event (ms) = 15s / 警報後冷卻
#define ALARM_SEQUENCE_SHOTS 2    // Doorbell alarm: photos per event / 門口警報：一次事件幾張
#define ALARM_SEQUENCE_INTERVAL_MS 5000 // Interval between alarm shots (ms) / 警報補拍間隔
#define DETECT_WIDTH        320   // Detection resolution width / 偵測用解析度寬
#define DETECT_HEIGHT       240   // Detection resolution height / 偵測用解析度高
#define BLOCK_SIZE          8     // Comparison block size (8x8) / 比對區塊大小
#define MOTION_CONFIRM_FRAMES 1   // Low FPS on ESP32-CAM; one frame is enough / 一幀達標就觸發
#define MOTION_CONFIRM_DELAY_MS 800 // Delay before second-stage confirm / 延遲再確認，降低誤觸
#define GLOBAL_CHANGE_IGNORE_PERCENT 85 // Ignore full-frame changes (likely exposure shift) / 忽略全畫面變化
#define MOTION_STARTUP_GRACE_MS 10000 // Grace period after boot for exposure to stabilize / 開機曝光穩定期
#define ENABLE_MQTT_COMMANDS 0    // Currently using Worker /poll + KV as main command channel
                                  // 目前以 Worker /poll + KV 為主要指令通道

// ============================
// NMKCAM (ESP32-CAM) camera pin definitions / 相機腳位
// ============================
#define PWDN_GPIO_NUM     32
#define RESET_GPIO_NUM    -1
#define XCLK_GPIO_NUM      0
#define SIOD_GPIO_NUM     26
#define SIOC_GPIO_NUM     27
#define Y9_GPIO_NUM       35
#define Y8_GPIO_NUM       34
#define Y7_GPIO_NUM       39
#define Y6_GPIO_NUM       36
#define Y5_GPIO_NUM       21
#define Y4_GPIO_NUM       19
#define Y3_GPIO_NUM       18
#define Y2_GPIO_NUM        5
#define VSYNC_GPIO_NUM    25
#define HREF_GPIO_NUM     23
#define PCLK_GPIO_NUM     22

// ============================
// Global variables / 全域變數
// ============================
WiFiClientSecure espClient;
PubSubClient mqttClient(espClient);
Preferences preferences;
WebServer webServer(80);
DNSServer dnsServer;
bool apModeActive = false;           // Whether in AP setup mode / 是否在 AP 設定模式

// WiFi config stored in NVS / NVS 儲存的 WiFi 設定
char storedSSID[64] = "";
char storedPassword[64] = "";

uint8_t* prevFrame = NULL;          // Previous frame (downscaled grayscale) / 前一幀（縮小後灰階）
int blockCols, blockRows;           // Block grid dimensions / 區塊網格大小
unsigned long lastMotionTime = 0;   // Last detection time / 上次偵測時間
bool motionDetectionEnabled = true; // Detection toggle / 偵測開關
unsigned long bootTime = 0;         // Boot time (millis) / 開機時間

// Scheduled quiet hours / 排程靜音時段
bool scheduleEnabled = false;
int quietStartHour = -1;
int quietStartMin = 0;
int quietEndHour = -1;
int quietEndMin = 0;

// ============================
// Forward declarations / 前向宣告
// ============================
void captureAndUpload(const char* trigger);
void captureAlarmSequence();
void waitWithCommandPolling(unsigned long durationMs);
void sendStatusReport();
void uploadPhoto(camera_fb_t* fb, const char* trigger);
bool isInQuietHours();
void pollWorkerCommand();
void resetMotionBaseline(const char* reason);
void initStatusLed();
void setStatusLed(uint8_t brightness);
camera_fb_t* getCameraFrameWithRetry(int attempts, int delayMs, const char* context);
bool decodeJpegToGrayBlocks(camera_fb_t* fb, uint8_t* blockBuffer);
float scorePhotoCandidate(camera_fb_t* fb, float* changePercent, int* strongBlocks);

void initStatusLed() {
#if ESP_ARDUINO_VERSION_MAJOR >= 3
  ledcAttach(LED_PIN, 5000, 8);
#else
  ledcSetup(STATUS_LED_CHANNEL, 5000, 8);
  ledcAttachPin(LED_PIN, STATUS_LED_CHANNEL);
#endif
  setStatusLed(0);
}

void setStatusLed(uint8_t brightness) {
#if ESP_ARDUINO_VERSION_MAJOR >= 3
  ledcWrite(LED_PIN, brightness);
#else
  ledcWrite(STATUS_LED_CHANNEL, brightness);
#endif
}

camera_fb_t* getCameraFrameWithRetry(int attempts, int delayMs, const char* context) {
  for (int i = 0; i < attempts; i++) {
    camera_fb_t* fb = esp_camera_fb_get();
    if (fb) return fb;
    Serial.printf("[WARN] %s frame capture failed, retry %d/%d\n", context, i + 1, attempts);
    delay(delayMs);
  }
  return NULL;
}

float scorePhotoCandidate(camera_fb_t* fb, float* changePercent, int* strongBlocks) {
  *changePercent = 0.0;
  *strongBlocks = 0;

  // File size is a low-cost proxy for sharpness; used as primary score when no baseline exists
  // 檔案大小是清晰度的低成本近似；沒有偵測基準時以它為主
  float sizeScore = (float)fb->len / 1024.0;
  if (!prevFrame) {
    return sizeScore * PHOTO_SCORE_SIZE_WEIGHT;
  }

  int totalBlocks = blockCols * blockRows;
  uint8_t* candidateFrame = (uint8_t*)ps_malloc(totalBlocks);
  if (!candidateFrame) {
    Serial.println("[WARN] Score: out of memory, falling back to size score");
    return sizeScore * PHOTO_SCORE_SIZE_WEIGHT;
  }

  if (!decodeJpegToGrayBlocks(fb, candidateFrame)) {
    free(candidateFrame);
    Serial.println("[WARN] Score: decode failed, falling back to size score");
    return sizeScore * PHOTO_SCORE_SIZE_WEIGHT;
  }

  int changedBlocks = 0;
  for (int i = 0; i < totalBlocks; i++) {
    int diff = abs((int)candidateFrame[i] - (int)prevFrame[i]);
    if (diff > MOTION_THRESHOLD) {
      changedBlocks++;
      if (diff > MOTION_STRONG_DIFF) {
        (*strongBlocks)++;
      }
    }
  }
  free(candidateFrame);

  *changePercent = (float)changedBlocks / totalBlocks * 100.0;
  return (sizeScore * PHOTO_SCORE_SIZE_WEIGHT)
       + (*changePercent * PHOTO_SCORE_MOTION_WEIGHT)
       + (*strongBlocks * PHOTO_SCORE_STRONG_WEIGHT);
}

// ============================
// Camera initialization / 相機初始化
// ============================
bool initCamera() {
  camera_config_t config;
  config.ledc_channel = LEDC_CHANNEL_0;
  config.ledc_timer   = LEDC_TIMER_0;
  config.pin_d0       = Y2_GPIO_NUM;
  config.pin_d1       = Y3_GPIO_NUM;
  config.pin_d2       = Y4_GPIO_NUM;
  config.pin_d3       = Y5_GPIO_NUM;
  config.pin_d4       = Y6_GPIO_NUM;
  config.pin_d5       = Y7_GPIO_NUM;
  config.pin_d6       = Y8_GPIO_NUM;
  config.pin_d7       = Y9_GPIO_NUM;
  config.pin_xclk     = XCLK_GPIO_NUM;
  config.pin_pclk     = PCLK_GPIO_NUM;
  config.pin_vsync    = VSYNC_GPIO_NUM;
  config.pin_href     = HREF_GPIO_NUM;
  config.pin_sccb_sda = SIOD_GPIO_NUM;
  config.pin_sccb_scl = SIOC_GPIO_NUM;
  config.pin_pwdn     = PWDN_GPIO_NUM;
  config.pin_reset    = RESET_GPIO_NUM;
  config.xclk_freq_hz = 10000000;  // 10MHz (reduced to lower DMA pressure)
  config.pixel_format = PIXFORMAT_JPEG;
  config.grab_mode    = CAMERA_GRAB_WHEN_EMPTY;

  // Use VGA (640x480), most stable on ESP32 Core 3.x
  config.frame_size   = FRAMESIZE_VGA;   // 640x480
  config.jpeg_quality = 15;              // Higher compression for faster upload
  config.fb_count     = 2;
  config.fb_location  = CAMERA_FB_IN_PSRAM;

  esp_err_t err = esp_camera_init(&config);
  if (err != ESP_OK) {
    Serial.printf("[ERROR] Camera init failed: 0x%x\n", err);
    return false;
  }

  // NMKCAM specific: disable auto white balance dithering (reduces false triggers)
  // NMKCAM 特有設定：關閉自動白平衡抖動（減少誤觸發）
  sensor_t* s = esp_camera_sensor_get();
  s->set_whitebal(s, 1);
  s->set_awb_gain(s, 1);
  s->set_wb_mode(s, 0);

  Serial.println("[OK] Camera initialized");
  return true;
}

// ============================
// Switch camera resolution / 切換相機解析度
// ============================
void setCameraFrameSize(framesize_t size) {
  sensor_t* s = esp_camera_sensor_get();
  s->set_framesize(s, size);
  // Discard first few frames to let sensor stabilize / 丟棄前幾幀讓感測器穩定
  for (int i = 0; i < 2; i++) {
    camera_fb_t* fb = esp_camera_fb_get();
    if (fb) esp_camera_fb_return(fb);
    delay(200);
  }
}

// ============================
// Load WiFi config from NVS / 從 NVS 載入 WiFi 設定
// ============================
void loadWiFiConfig() {
  preferences.begin("easycam", true);  // Read-only / 唯讀
  String ssid = preferences.getString("ssid", "");
  String pass = preferences.getString("password", "");
  preferences.end();

  if (ssid.length() > 0) {
    ssid.toCharArray(storedSSID, sizeof(storedSSID));
    pass.toCharArray(storedPassword, sizeof(storedPassword));
    Serial.printf("[WiFi] Loaded from NVS: SSID=%s\n", storedSSID);
  } else {
    // NVS empty, use defaults / NVS 沒有存過，使用預設值
    strncpy(storedSSID, DEFAULT_WIFI_SSID, sizeof(storedSSID));
    strncpy(storedPassword, DEFAULT_WIFI_PASSWORD, sizeof(storedPassword));
    Serial.println("[WiFi] No NVS config, using defaults");
  }
}

// ============================
// Save WiFi config to NVS / 儲存 WiFi 設定到 NVS
// ============================
void saveWiFiConfig(const char* ssid, const char* password) {
  preferences.begin("easycam", false);  // Read-write / 讀寫
  preferences.putString("ssid", ssid);
  preferences.putString("password", password);
  preferences.end();
  Serial.printf("[WiFi] Saved new config: SSID=%s\n", ssid);
}

// ============================
// Clear NVS WiFi config (enter AP mode) / 清除 NVS WiFi 設定
// ============================
void clearWiFiConfig() {
  preferences.begin("easycam", false);
  preferences.remove("ssid");
  preferences.remove("password");
  preferences.end();
  Serial.println("[WiFi] NVS config cleared");
}

// ============================
// AP Setup Mode — WiFi config web page / AP 設定模式 — WiFi 設定網頁
// ============================
const char AP_HTML[] PROGMEM = R"rawliteral(
<!DOCTYPE html><html><head>
<meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1">
<title>EasyCam WiFi Setup</title>
<style>
body{font-family:sans-serif;max-width:400px;margin:40px auto;padding:0 20px;background:#1a1a2e;color:#eee}
h2{color:#e94560;text-align:center}
input,select{width:100%;padding:12px;margin:8px 0;border:1px solid #555;border-radius:6px;background:#16213e;color:#eee;font-size:16px;box-sizing:border-box}
button{width:100%;padding:14px;background:#e94560;color:#fff;border:none;border-radius:6px;font-size:18px;cursor:pointer;margin-top:16px}
button:hover{background:#c23152}
.info{font-size:13px;color:#aaa;text-align:center;margin-top:20px}
#nets{margin:8px 0}
</style></head><body>
<h2>📷 EasyCam WiFi Setup</h2>
<form action="/save" method="POST">
<label>WiFi Network (SSID)</label>
<div id="nets"></div>
<input type="text" name="ssid" id="ssid" placeholder="Enter or select from above" required>
<label>WiFi Password</label>
<input type="password" name="password" placeholder="Enter WiFi password" required>
<button type="submit">💾 Save & Reconnect</button>
</form>
<p class="info">After saving, EasyCam will reboot and connect to the new WiFi.<br>If connection fails, this setup page will reappear.</p>
<script>
fetch('/scan').then(r=>r.json()).then(nets=>{
  let h='<select onchange="document.getElementById(\'ssid\').value=this.value"><option value="">-- Select a detected network --</option>';
  nets.forEach(n=>h+='<option value="'+n.ssid+'">'+n.ssid+' ('+n.rssi+'dBm)</option>');
  h+='</select>';
  document.getElementById('nets').innerHTML=h;
});
</script>
</body></html>
)rawliteral";

void startAPMode() {
  apModeActive = true;
  WiFi.disconnect(true);
  WiFi.mode(WIFI_AP);
  WiFi.softAP(AP_SSID, AP_PASSWORD);
  delay(500);

  IPAddress apIP = WiFi.softAPIP();
  Serial.println("\n=============================");
  Serial.println("  📡 WiFi Setup Mode");
  Serial.printf("  AP Name: %s\n", AP_SSID);
  Serial.printf("  AP Password: %s\n", AP_PASSWORD);
  Serial.printf("  Setup URL: http://%s\n", apIP.toString().c_str());
  Serial.println("=============================\n");

  // DNS intercept — redirect all URLs to setup page
  // DNS 攔截 — 所有網址都導到設定頁面
  dnsServer.start(53, "*", apIP);

  // Setup page / 設定頁面
  webServer.on("/", HTTP_GET, []() {
    webServer.send(200, "text/html", AP_HTML);
  });

  // WiFi scan API / WiFi 掃描 API
  webServer.on("/scan", HTTP_GET, []() {
    int n = WiFi.scanNetworks();
    String json = "[";
    for (int i = 0; i < n; i++) {
      if (i > 0) json += ",";
      json += "{\"ssid\":\"" + WiFi.SSID(i) + "\",\"rssi\":" + String(WiFi.RSSI(i)) + "}";
    }
    json += "]";
    webServer.send(200, "application/json", json);
  });

  // Save config / 儲存設定
  webServer.on("/save", HTTP_POST, []() {
    String newSSID = webServer.arg("ssid");
    String newPass = webServer.arg("password");

    if (newSSID.length() > 0) {
      saveWiFiConfig(newSSID.c_str(), newPass.c_str());
      webServer.send(200, "text/html",
        "<html><head><meta charset='utf-8'></head><body style='font-family:sans-serif;text-align:center;padding:60px;background:#1a1a2e;color:#eee'>"
        "<h2>✅ Settings saved!</h2><p>EasyCam will reboot in 3 seconds...</p></body></html>");
      delay(3000);
      ESP.restart();
    } else {
      webServer.send(400, "text/html", "<h2>SSID cannot be empty</h2>");
    }
  });

  // Redirect all unknown paths to setup page (Captive Portal)
  // 所有未知路徑也導到設定頁（Captive Portal）
  webServer.onNotFound([]() {
    webServer.sendHeader("Location", "/", true);
    webServer.send(302, "text/plain", "");
  });

  webServer.begin();

  // LED blink to indicate AP mode / LED 快閃表示 AP 模式
  Serial.println("[AP] Waiting for user to configure WiFi...");
  while (apModeActive) {
    dnsServer.processNextRequest();
    webServer.handleClient();

    // Double blink for AP mode / LED 雙閃表示 AP 模式
    static unsigned long lastBlink = 0;
    if (millis() - lastBlink > 500) {
      lastBlink = millis();
      static bool on = false;
      on = !on;
      setStatusLed(on ? 128 : 0);
    }
    delay(10);
  }
}

// ============================
// WiFi connection (with NVS + AP fallback) / WiFi 連線（帶 NVS + AP 備援）
// ============================
void connectWiFi() {
  loadWiFiConfig();

  WiFi.mode(WIFI_STA);
  Serial.printf("[WiFi] Connecting to %s", storedSSID);
  WiFi.begin(storedSSID, storedPassword);
  WiFi.setSleep(false);

  int retry = 0;
  while (WiFi.status() != WL_CONNECTED && retry < 20) {
    delay(500);
    Serial.print(".");
    retry++;
  }

  if (WiFi.status() == WL_CONNECTED) {
    Serial.printf("\n[OK] WiFi connected, IP: %s\n", WiFi.localIP().toString().c_str());
    return;
  }

  // Connection failed → enter AP setup mode / 連線失敗 → 進入 AP 設定模式
  Serial.println("\n[WARN] WiFi connection failed, entering setup mode...");
  startAPMode();
}

// ============================
// MQTT callback / MQTT 回呼函式
// ============================
void mqttCallback(char* topic, byte* payload, unsigned int length) {
  String message = "";
  for (unsigned int i = 0; i < length; i++) {
    message += (char)payload[i];
  }
  Serial.printf("[MQTT] Command received: %s\n", message.c_str());

  if (message == "capture") {
    Serial.println("[MQTT] Executing remote capture...");
    captureAndUpload("remote");
    lastMotionTime = millis();
  } else if (message == "pause") {
    motionDetectionEnabled = false;
    resetMotionBaseline("pause");
    Serial.println("[MQTT] ⏸ Motion detection paused");
    sendStatusReport();
  } else if (message == "resume") {
    motionDetectionEnabled = true;
    resetMotionBaseline("resume");
    Serial.println("[MQTT] ▶ Motion detection resumed");
    sendStatusReport();
  } else if (message == "reset_wifi") {
    Serial.println("[MQTT] WiFi reset command received, entering setup mode...");
    clearWiFiConfig();
    delay(1000);
    ESP.restart();
  } else if (message == "flip") {
    sensor_t* s = esp_camera_sensor_get();
    int current = s->status.vflip;
    s->set_vflip(s, !current);
    resetMotionBaseline("flip");
    Serial.printf("[MQTT] 🔄 Flip: %s\n", !current ? "ON" : "OFF");
    sendStatusReport();
  } else if (message == "mirror") {
    sensor_t* s = esp_camera_sensor_get();
    int current = s->status.hmirror;
    s->set_hmirror(s, !current);
    resetMotionBaseline("mirror");
    Serial.printf("[MQTT] 🪞 Mirror: %s\n", !current ? "ON" : "OFF");
    sendStatusReport();
  } else if (message == "info") {
    Serial.println("[MQTT] Reporting system status...");
    sendStatusReport();
  } else if (message.startsWith("schedule ")) {
    String param = message.substring(9);
    if (param == "off") {
      scheduleEnabled = false;
      Serial.println("[MQTT] ⏰ Quiet hours disabled");
      sendStatusReport();
    } else {
      int h1, m1, h2, m2;
      if (sscanf(param.c_str(), "%d:%d-%d:%d", &h1, &m1, &h2, &m2) == 4) {
        quietStartHour = h1;
        quietStartMin = m1;
        quietEndHour = h2;
        quietEndMin = m2;
        scheduleEnabled = true;
        Serial.printf("[MQTT] ⏰ Quiet hours: %02d:%02d ~ %02d:%02d\n", h1, m1, h2, m2);
        sendStatusReport();
      }
    }
  }
}

// ============================
// MQTT connection / MQTT 連線
// ============================
void connectMQTT() {
  espClient.setInsecure();  // HiveMQ Cloud uses TLS, skip cert verification here
  mqttClient.setKeepAlive(120);  // Extend keepalive to 120s (default 15s too short, disconnects during upload)
  mqttClient.setServer(MQTT_BROKER, MQTT_PORT);
  mqttClient.setCallback(mqttCallback);
  mqttClient.setBufferSize(512);

  int retry = 0;
  while (!mqttClient.connected() && retry < 1) {
    Serial.printf("[MQTT] Connecting to %s...\n", MQTT_BROKER);
    if (mqttClient.connect(MQTT_CLIENT_ID, MQTT_USER, MQTT_PASSWORD)) {
      Serial.println("[OK] MQTT connected");
      mqttClient.subscribe(MQTT_TOPIC_CMD);
      Serial.printf("[OK] Subscribed to %s\n", MQTT_TOPIC_CMD);
    } else {
      Serial.printf("[ERROR] MQTT connection failed, rc=%d\n", mqttClient.state());
      retry++;
    }
  }
}

// ============================
// JPEG decode → grayscale → block averages (proper motion detection)
// JPEG 解碼 → 灰階 → 區塊平均值（正確的動態偵測）
// ============================
// References: alanesq/CameraWifiMotion, s60sc/ESP32-CAM_MJPEG2SD
// Approach: Use fmt2rgb888() to decode JPEG → RGB pixels, convert to grayscale, do block comparison
// Old method (comparing JPEG raw bytes) produces 60-70% false positives due to compression variance

bool decodeJpegToGrayBlocks(camera_fb_t* fb, uint8_t* blockBuffer) {
  // Step 1: JPEG → RGB888 (using esp32-camera built-in function)
  // VGA(640x480) needs 640*480*3 = 921,600 bytes, stored in PSRAM
  size_t rgbLen = fb->width * fb->height * 3;
  uint8_t* rgbBuf = (uint8_t*)ps_malloc(rgbLen);
  if (!rgbBuf) {
    Serial.println("[ERROR] Insufficient PSRAM for JPEG decode");
    return false;
  }

  // fmt2rgb888: JPEG → RGB888 decoder provided by esp32-camera
  bool ok = fmt2rgb888(fb->buf, fb->len, PIXFORMAT_JPEG, rgbBuf);
  if (!ok) {
    Serial.println("[ERROR] JPEG decode failed");
    free(rgbBuf);
    return false;
  }

  // Step 2: RGB → grayscale, downsample to DETECT_WIDTH x DETECT_HEIGHT, compute block averages
  int srcW = fb->width;   // Actual image width (e.g. 640)
  int srcH = fb->height;  // Actual image height (e.g. 480)

  int totalBlocks = blockCols * blockRows;
  // Each block maps to a pixel range in the source image
  float scaleX = (float)srcW / DETECT_WIDTH;
  float scaleY = (float)srcH / DETECT_HEIGHT;

  for (int by = 0; by < blockRows; by++) {
    for (int bx = 0; bx < blockCols; bx++) {
      // Pixel range this block maps to in the source image
      int startX = (int)(bx * BLOCK_SIZE * scaleX);
      int endX   = (int)((bx + 1) * BLOCK_SIZE * scaleX);
      int startY = (int)(by * BLOCK_SIZE * scaleY);
      int endY   = (int)((by + 1) * BLOCK_SIZE * scaleY);

      // Clamp bounds / 限制範圍
      if (endX > srcW) endX = srcW;
      if (endY > srcH) endY = srcH;

      // Calculate average grayscale value for all pixels in this block
      long sum = 0;
      int count = 0;
      for (int y = startY; y < endY; y++) {
        for (int x = startX; x < endX; x++) {
          int idx = (y * srcW + x) * 3;  // RGB888: 3 bytes per pixel
          uint8_t r = rgbBuf[idx];
          uint8_t g = rgbBuf[idx + 1];
          uint8_t b = rgbBuf[idx + 2];
          // ITU-R BT.601 grayscale conversion (human eye most sensitive to green)
          sum += (r * 77 + g * 150 + b * 29) >> 8;
          count++;
        }
      }

      blockBuffer[by * blockCols + bx] = (count > 0) ? (uint8_t)(sum / count) : 0;
    }
  }

  free(rgbBuf);
  return true;
}

void resetMotionBaseline(const char* reason) {
  if (prevFrame) {
    free(prevFrame);
    prevFrame = NULL;
  }
  Serial.printf("[MOTION] Baseline reset: %s\n", reason);
}

// ============================
// Detect frame difference / 偵測畫面差異
// ============================
bool detectMotion(camera_fb_t* fb) {
  // Dark scene filter: very small JPEG = black frame (noise causes false motion)
  // 暗處防誤判：JPEG 檔太小表示畫面全黑（雜訊會造成假動態）
  if (fb->len < 3000) {
    static bool darkWarned = false;
    if (!darkWarned) {
      Serial.printf("[INFO] Scene too dark (%d bytes), skipping detection\n", fb->len);
      darkWarned = true;
    }
    return false;
  }

  int totalBlocks = blockCols * blockRows;

  // Allocate block buffer for current frame (in PSRAM)
  uint8_t* currentFrame = (uint8_t*)ps_malloc(totalBlocks);
  if (!currentFrame) {
    Serial.println("[ERROR] Out of memory");
    return false;
  }

  // Decode JPEG and compute block grayscale values
  if (!decodeJpegToGrayBlocks(fb, currentFrame)) {
    free(currentFrame);
    return false;
  }

  // First run, no previous frame to compare
  // 第一次執行，沒有前一幀可比對
  if (prevFrame == NULL) {
    prevFrame = currentFrame;
    Serial.println("[INFO] First frame stored, detection starts next frame");
    return false;
  }

  // Compare block differences / 比對區塊差異
  int changedBlocks = 0;
  int strongBlocks = 0;
  long changedDiffSum = 0;
  int maxDiff = 0;
  for (int i = 0; i < totalBlocks; i++) {
    int diff = abs((int)currentFrame[i] - (int)prevFrame[i]);
    if (diff > MOTION_THRESHOLD) {
      changedBlocks++;
      changedDiffSum += diff;
      if (diff > maxDiff) maxDiff = diff;
      if (diff > MOTION_STRONG_DIFF) {
        strongBlocks++;
      }
    }
  }

  // Calculate change percentage / 計算變化百分比
  float changePercent = (float)changedBlocks / totalBlocks * 100.0;
  float avgChangedDiff = changedBlocks > 0 ? (float)changedDiffSum / changedBlocks : 0.0;

  // Debug: print static change rate every 10 frames for threshold calibration
  static int frameCount = 0;
  frameCount++;
  if (frameCount % 10 == 0) {
    Serial.printf("[DEBUG] Change: %.1f%% (%d/%d), strong:%d, avgDiff:%.1f, maxDiff:%d\n",
                  changePercent, changedBlocks, totalBlocks, strongBlocks, avgChangedDiff, maxDiff);
  }

  if (changePercent >= GLOBAL_CHANGE_IGNORE_PERCENT) {
    Serial.printf("[MOTION] Ignoring full-frame brightness change %.1f%% (likely exposure/lighting shift)\n", changePercent);
    free(prevFrame);
    prevFrame = currentFrame;
    return false;
  }

  if (changePercent > MOTION_PERCENT || strongBlocks >= MOTION_STRONG_BLOCKS) {
    Serial.printf("[MOTION] Suspicious motion, pending confirm: change %.1f%% (%d/%d), strong:%d, avgDiff:%.1f, maxDiff:%d\n",
                  changePercent, changedBlocks, totalBlocks, strongBlocks, avgChangedDiff, maxDiff);
    free(currentFrame);
    return true;
  }

  // Only update baseline when no motion detected. Suspicious frames don't overwrite,
  // so second-stage confirm can still compare against the original background.
  // 只在未觸發時更新基準，可疑幀不覆蓋，讓二階段確認仍能和原本背景比對。
  free(prevFrame);
  prevFrame = currentFrame;
  return false;
}

// ============================
// Capture and upload (core function) / 拍照並上傳（核心函式）
// ============================
void captureAndUpload(const char* trigger) {
  Serial.printf("[CAPTURE] Trigger: %s\n", trigger);

  // LED flash to indicate capture / LED 快閃表示正在拍照
  for (int i = 0; i < 3; i++) {
    setStatusLed(255);
    delay(50);
    setStatusLed(0);
    delay(50);
  }

  // Already at SVGA resolution, no switch needed
  // Discard one frame to let sensor stabilize / 丟棄一幀讓感測器穩定
  delay(400);
  camera_fb_t* discard = getCameraFrameWithRetry(5, 250, "pre-capture discard");
  if (discard) esp_camera_fb_return(discard);
  delay(300);

  // Burst 3 shots, pick best using composite score:
  // change area ratio + strong change blocks + JPEG file size (sharpness proxy)
  // 連拍 3 張，用混合分數選最佳：變化比例 + 強變化區塊 + JPEG 大小（清晰度近似）
  camera_fb_t* bestPhoto = NULL;
  float bestScore = -1.0;

  for (int i = 0; i < 3; i++) {
    camera_fb_t* fb = getCameraFrameWithRetry(5, 250, "burst");
    if (!fb) {
      Serial.printf("[ERROR] Shot %d capture failed\n", i + 1);
      continue;
    }
    float changePercent = 0.0;
    int strongBlocks = 0;
    float score = scorePhotoCandidate(fb, &changePercent, &strongBlocks);

    Serial.printf("[CAPTURE] Shot %d: %d bytes, change %.1f%%, strong %d, score %.1f\n",
                  i + 1, fb->len, changePercent, strongBlocks, score);

    if (score > bestScore) {
      // Release previous best / 釋放之前的最佳照片
      if (bestPhoto) esp_camera_fb_return(bestPhoto);
      bestPhoto = fb;
      bestScore = score;
    } else {
      esp_camera_fb_return(fb);
    }
    delay(200);  // Short interval for sensor stabilization
  }

  if (!bestPhoto) {
    Serial.println("[ERROR] All captures failed");
    return;
  }

  Serial.printf("[CAPTURE] Best photo selected: %d bytes, score %.1f\n", bestPhoto->len, bestScore);

  // Upload to Cloudflare Worker
  uploadPhoto(bestPhoto, trigger);

  // Release photo / 釋放照片
  esp_camera_fb_return(bestPhoto);

  // LED on for 1s to indicate completion / LED 長亮 1 秒表示完成
  setStatusLed(255);
  delay(1000);
  setStatusLed(0);

  resetMotionBaseline(trigger);
}

// ============================
// HTTP upload photo to Worker / HTTP 上傳照片到 Worker
// ============================
void uploadPhoto(camera_fb_t* fb, const char* trigger) {
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("[ERROR] WiFi not connected, cannot upload");
    return;
  }

  Serial.printf("[UPLOAD] Free heap: %d, PSRAM: %d\n", ESP.getFreeHeap(), ESP.getFreePsram());

  WiFiClientSecure client;
  client.setInsecure();  // Cloudflare Workers use public TLS
  client.setTimeout(30);  // 30s TLS timeout

  HTTPClient http;
  http.begin(client, WORKER_URL);  // HTTPS requires WiFiClientSecure
  http.addHeader("Content-Type", "image/jpeg");
  http.addHeader("X-Trigger", trigger);  // Tell Worker the trigger source
  http.setTimeout(30000);  // 30s timeout (ESP32 TLS handshake is slow)

  Serial.printf("[UPLOAD] Uploading %d bytes to Worker...\n", fb->len);
  int httpCode = http.POST(fb->buf, fb->len);

  if (httpCode > 0) {
    String response = http.getString();
    Serial.printf("[UPLOAD] Response: HTTP %d, body: %s\n", httpCode, response.c_str());
  } else {
    Serial.printf("[ERROR] Upload failed: %s\n", http.errorToString(httpCode).c_str());
  }

  http.end();
}

void waitWithCommandPolling(unsigned long durationMs) {
  unsigned long start = millis();
  unsigned long lastPollDuringWait = 0;
  while (millis() - start < durationMs) {
    if (WiFi.status() == WL_CONNECTED && millis() - lastPollDuringWait > 3000) {
      lastPollDuringWait = millis();
      pollWorkerCommand();
    }
    ledBreath();
    delay(100);
  }
}

void captureAlarmSequence() {
  Serial.printf("[ALARM] Door alarm event started, capturing %d shots\n", ALARM_SEQUENCE_SHOTS);

  for (int i = 0; i < ALARM_SEQUENCE_SHOTS; i++) {
    Serial.printf("[ALARM] Event record %d/%d\n", i + 1, ALARM_SEQUENCE_SHOTS);
    captureAndUpload("motion");

    if (i < ALARM_SEQUENCE_SHOTS - 1) {
      waitWithCommandPolling(ALARM_SEQUENCE_INTERVAL_MS);
    }
  }

  lastMotionTime = millis();
  Serial.printf("[ALARM] Event complete, cooldown %d seconds\n", COOLDOWN_MS / 1000);
}

// ============================
// LED breathing effect (standby indicator) / LED 呼吸燈（待機狀態）
// ============================
void ledBreath() {
  static unsigned long lastUpdate = 0;
  static int brightness = 0;
  static int direction = 5;

  if (millis() - lastUpdate > 30) {
    lastUpdate = millis();
    brightness += direction;
    if (brightness >= 255 || brightness <= 0) direction = -direction;
    setStatusLed(brightness);
  }
}

// ============================
// Check if in quiet hours / 檢查是否在排程靜音時段
// ============================
bool isInQuietHours() {
  if (!scheduleEnabled) return false;

  struct tm timeinfo;
  if (!getLocalTime(&timeinfo, 1000)) return false;

  int nowMin = timeinfo.tm_hour * 60 + timeinfo.tm_min;
  int startMin = quietStartHour * 60 + quietStartMin;
  int endMin = quietEndHour * 60 + quietEndMin;

  if (startMin <= endMin) {
    // Same day (e.g. 08:00-18:00) / 同一天內
    return nowMin >= startMin && nowMin < endMin;
  } else {
    // Crosses midnight (e.g. 23:00-07:00) / 跨午夜
    return nowMin >= startMin || nowMin < endMin;
  }
}

// ============================
// Send status report to Worker → Telegram / 發送系統狀態報告
// ============================
void sendStatusReport() {
  struct tm timeinfo;
  bool hasTime = getLocalTime(&timeinfo, 1000);

  unsigned long uptimeSec = (millis() - bootTime) / 1000;
  int days = uptimeSec / 86400;
  int hours = (uptimeSec % 86400) / 3600;
  int mins = (uptimeSec % 3600) / 60;

  sensor_t* s = esp_camera_sensor_get();

  char timeStr[32] = "N/A";
  if (hasTime) {
    snprintf(timeStr, sizeof(timeStr), "%02d:%02d:%02d",
             timeinfo.tm_hour, timeinfo.tm_min, timeinfo.tm_sec);
  }

  char schedStr[32] = "off";
  if (scheduleEnabled) {
    snprintf(schedStr, sizeof(schedStr), "%02d:%02d-%02d:%02d",
             quietStartHour, quietStartMin, quietEndHour, quietEndMin);
  }

  char report[512];
  snprintf(report, sizeof(report),
    "{\"uptime\":\"%dd %dh %dm\","
    "\"heap\":%d,"
    "\"psram\":%d,"
    "\"wifi_rssi\":%d,"
    "\"wifi_ssid\":\"%s\","
    "\"motion\":\"%s\","
    "\"flip\":%d,"
    "\"mirror\":%d,"
    "\"schedule\":\"%s\","
    "\"time\":\"%s\"}",
    days, hours, mins,
    ESP.getFreeHeap(),
    ESP.getFreePsram(),
    WiFi.RSSI(),
    WiFi.SSID().c_str(),
    motionDetectionEnabled ? "on" : "off",
    s->status.vflip,
    s->status.hmirror,
    schedStr,
    timeStr
  );

  WiFiClientSecure client;
  client.setInsecure();
  client.setTimeout(10);
  HTTPClient http;
  http.begin(client, WORKER_STATUS);
  http.addHeader("Content-Type", "application/json");
  http.setTimeout(10000);
  int code = http.POST((uint8_t*)report, strlen(report));
  Serial.printf("[STATUS] Report result: HTTP %d\n", code);
  http.end();
}

// ============================
// setup()
// ============================
void setup() {
  WRITE_PERI_REG(RTC_CNTL_BROWN_OUT_REG, 0);  // Disable brownout detector / 停用欠壓偵測

  Serial.begin(115200);
  Serial.println("\n=============================");
  Serial.println("  EasyCam Motion Monitor v1.0");
  Serial.println("=============================\n");

  // LED setup (ESP32 uses LEDC PWM)
  initStatusLed();

  // Capture button IO3 shares with Serial RX; OK in monitor mode since we don't read Serial input
  // 拍照按鈕 IO3 與 Serial RX 共用，監控模式下不用 Serial 輸入所以可以用
  pinMode(BUTTON_PIN, INPUT_PULLUP);

  // Initialize camera / 初始化相機
  if (!initCamera()) {
    Serial.println("[FATAL] Camera init failed, system halted");
    while (1) {
      setStatusLed(255);
      delay(200);
      setStatusLed(0);
      delay(200);
    }
  }

  // Calculate block grid / 計算區塊網格
  blockCols = DETECT_WIDTH / BLOCK_SIZE;   // 40
  blockRows = DETECT_HEIGHT / BLOCK_SIZE;  // 30
  Serial.printf("[INFO] Detection grid: %dx%d = %d blocks\n", blockCols, blockRows, blockCols * blockRows);

  // Connect WiFi / 連線 WiFi
  connectWiFi();

  // Connect MQTT (disabled by default; main command channel is Worker /poll)
  // 連線 MQTT（目前預設關閉，主要指令通道是 Worker /poll）
#if ENABLE_MQTT_COMMANDS
  connectMQTT();
#else
  Serial.println("[INFO] MQTT commands disabled, using Worker /poll");
#endif

  // Set NTP time (needed for scheduling) / 設定 NTP 時間（排程功能需要）
  configTime(8 * 3600, 0, "pool.ntp.org", "time.nist.gov");
  bootTime = millis();

  // LED on briefly to indicate ready / LED 亮一下表示就緒
  setStatusLed(255);
  delay(2000);
  setStatusLed(0);

  Serial.println("\n[READY] System ready, monitoring started...\n");
}

// ============================
// loop()
// ============================
void loop() {
  // Maintain MQTT connection (optional; avoids blocking main poll channel if HiveMQ is unreachable)
#if ENABLE_MQTT_COMMANDS
  if (!mqttClient.connected()) {
    connectMQTT();
  }
  mqttClient.loop();
#endif

  // WiFi reconnect / WiFi 斷線重連
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("[WARN] WiFi disconnected, reconnecting...");
    connectWiFi();
  }

  // Poll Worker commands: check every 3s (main command channel)
  // Placed before motion detection so commands are received even during cooldown.
  // 輪詢 Worker 指令：每 3 秒，放在偵測前，冷卻期間也能收到指令。
  static unsigned long lastPoll = 0;
  if (WiFi.status() == WL_CONNECTED && millis() - lastPoll > 3000) {
    lastPoll = millis();
    pollWorkerCommand();
  }

  // Check physical capture button (IO3, active LOW)
  // 檢查實體拍照按鈕（IO3，低電位觸發）
  if (digitalRead(BUTTON_PIN) == LOW) {
    delay(50);  // Debounce / 去彈跳
    if (digitalRead(BUTTON_PIN) == LOW) {
      Serial.println("[BUTTON] Button triggered capture");
      captureAndUpload("button");
      // Wait for button release / 等待按鈕放開
      while (digitalRead(BUTTON_PIN) == LOW) delay(10);
      lastMotionTime = millis();  // Reset cooldown
    }
  }

  // Motion detection (auto-skipped during quiet hours)
  // 畫面差異偵測（排程靜音時段自動跳過）
  if (motionDetectionEnabled && !isInQuietHours()) {
    if (millis() - bootTime < MOTION_STARTUP_GRACE_MS) {
      ledBreath();  // Startup grace period for exposure stabilization / 開機曝光穩定期
    }
    // Skip during cooldown / 冷卻中跳過
    else if (millis() - lastMotionTime < COOLDOWN_MS) {
      ledBreath();  // Breathing LED during cooldown / 冷卻中呼吸燈
    } else {
      // Capture one frame for detection / 拍一幀做偵測
      camera_fb_t* fb = getCameraFrameWithRetry(3, 150, "detect");
      if (fb) {
        bool suspiciousMotion = detectMotion(fb);
        esp_camera_fb_return(fb);

        if (suspiciousMotion) {
          waitWithCommandPolling(MOTION_CONFIRM_DELAY_MS);

          camera_fb_t* confirmFb = getCameraFrameWithRetry(3, 150, "confirm");
          if (confirmFb) {
            bool confirmedMotion = detectMotion(confirmFb);
            esp_camera_fb_return(confirmFb);

            if (confirmedMotion) {
              Serial.println("[MOTION] Second-stage confirmed, triggering door alarm event");
              captureAlarmSequence();
            } else {
              Serial.println("[MOTION] Second-stage not confirmed, ignoring suspicious change");
            }
          }
        }
      }
    }
  }

  // Standby LED: brief flash every 3s to indicate normal operation
  // 待機 LED 微閃（每 3 秒閃一下表示正常運作）
  static unsigned long lastBlink = 0;
  if (millis() - lastBlink > 3000) {
    lastBlink = millis();
    setStatusLed(255);
    delay(30);
    setStatusLed(0);
  }

  delay(200);  // Detection interval (~5 fps) / 偵測間隔（每秒約 5 幀）
}

// ============================
// Poll Worker commands (MQTT fallback) / 輪詢 Worker 指令（MQTT 備用方案）
// ============================
void pollWorkerCommand() {
  WiFiClientSecure client;
  client.setInsecure();

  HTTPClient http;
  http.begin(client, WORKER_POLL);
  http.setTimeout(5000);

  int httpCode = http.GET();
  if (httpCode == 200) {
    String response = http.getString();
    // Parse command field, reuse mqttCallback for unified handling
    // 解析 command 欄位，統一交給 mqttCallback 處理
    int cmdStart = response.indexOf("\"command\":\"");
    if (cmdStart >= 0) {
      cmdStart += 11;
      int cmdEnd = response.indexOf("\"", cmdStart);
      if (cmdEnd > cmdStart) {
        String cmd = response.substring(cmdStart, cmdEnd);
        Serial.printf("[POLL] Command received: %s\n", cmd.c_str());
        mqttCallback((char*)"easycam/cmd", (byte*)cmd.c_str(), cmd.length());
      }
    }
  } else if (httpCode < 0) {
    Serial.printf("[POLL] Poll failed: %s\n", http.errorToString(httpCode).c_str());
  } else {
    Serial.printf("[POLL] Worker responded HTTP %d\n", httpCode);
  }
  http.end();
}
