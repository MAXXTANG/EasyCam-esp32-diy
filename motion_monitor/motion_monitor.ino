/*
 * ESP32-CAM EasyCam 感應監視器
 * NMKing 小霸王實驗室 EasyCam 擴充版
 *
 * 功能：
 * 1. 畫面差異偵測 → 拍3張選最清楚1張 → 上傳雲端 → LINE + Telegram 通知
 * 2. MQTT 遠端指令 → 即時拍照回傳
 * 3. IO33 LED 狀態指示
 *
 * 開發板選擇：ESP32 Wrover Module (帶USB)
 * Partition Scheme：Huge APP (3MB No OTA/1MB SPIFFS)
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
#include "config.h"             // 敏感設定（複製 config.example.h → config.h 並填入你的資料）

// ============================
// 從 config.h 載入設定
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
// 硬體設定（EasyCam 擴充版）
// ============================
#define LED_PIN        33    // NMKCAM LED（不是原版的IO4）
#define BUTTON_PIN     3     // 拍照按鈕
#define STATUS_LED_CHANNEL 4 // Core 2.x LEDC 需要固定通道，避開相機 XCLK 的 channel 0

// ============================
// 偵測參數（可調整）
// ============================
#define MOTION_THRESHOLD    8     // 像素差異門檻（0-255）
#define MOTION_PERCENT      3     // 變化區塊百分比門檻（%）
#define MOTION_STRONG_DIFF  22    // 強變化區塊門檻，捕捉小範圍人體移動
#define MOTION_STRONG_BLOCKS 8    // 強變化區塊數達標即觸發
#define PHOTO_SCORE_SIZE_WEIGHT 0.7     // JPEG 大小權重（KB）
#define PHOTO_SCORE_MOTION_WEIGHT 3.0   // 變化區域比例權重
#define PHOTO_SCORE_STRONG_WEIGHT 0.35  // 強變化區塊權重
#define COOLDOWN_MS         15000 // 警報事件結束後短冷卻（毫秒）= 15 秒
#define ALARM_SEQUENCE_SHOTS 2    // 門口警報：一次事件保留幾張記錄
#define ALARM_SEQUENCE_INTERVAL_MS 5000 // 警報事件補拍間隔（毫秒）
#define DETECT_WIDTH        320   // 偵測用解析度寬
#define DETECT_HEIGHT       240   // 偵測用解析度高
#define BLOCK_SIZE          8     // 比對區塊大小（8x8）
#define MOTION_CONFIRM_FRAMES 1   // ESP32-CAM 偵測幀率低，人體走過時一幀達標就觸發
#define MOTION_CONFIRM_DELAY_MS 800 // 第一幀可疑後，延遲再確認一次，降低光線/雜訊誤觸
#define GLOBAL_CHANGE_IGNORE_PERCENT 85 // 全畫面大幅變動多半是曝光/光線變化
#define MOTION_STARTUP_GRACE_MS 10000 // 開機後先讓曝光/白平衡穩定
#define ENABLE_MQTT_COMMANDS 0    // 目前以 Worker /poll + KV 為主要指令通道

// ============================
// NMKCAM (ESP32-CAM) 相機腳位
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
// 全域變數
// ============================
WiFiClientSecure espClient;
PubSubClient mqttClient(espClient);
Preferences preferences;
WebServer webServer(80);
DNSServer dnsServer;
bool apModeActive = false;           // 是否在 AP 設定模式

// NVS 儲存的 WiFi 設定
char storedSSID[64] = "";
char storedPassword[64] = "";

uint8_t* prevFrame = NULL;          // 前一幀（縮小後灰階）
int blockCols, blockRows;           // 區塊網格大小
unsigned long lastMotionTime = 0;   // 上次偵測時間
bool motionDetectionEnabled = true; // 偵測開關
unsigned long bootTime = 0;         // 開機時間（millis）

// 排程靜音時段
bool scheduleEnabled = false;
int quietStartHour = -1;
int quietStartMin = 0;
int quietEndHour = -1;
int quietEndMin = 0;

// ============================
// 前向宣告（函式定義在後面）
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
    Serial.printf("[WARN] %s 取幀失敗，重試 %d/%d\n", context, i + 1, attempts);
    delay(delayMs);
  }
  return NULL;
}

float scorePhotoCandidate(camera_fb_t* fb, float* changePercent, int* strongBlocks) {
  *changePercent = 0.0;
  *strongBlocks = 0;

  // 檔案大小仍是清晰度的低成本近似；沒有偵測基準時（例如手動拍照）就以它為主。
  float sizeScore = (float)fb->len / 1024.0;
  if (!prevFrame) {
    return sizeScore * PHOTO_SCORE_SIZE_WEIGHT;
  }

  int totalBlocks = blockCols * blockRows;
  uint8_t* candidateFrame = (uint8_t*)ps_malloc(totalBlocks);
  if (!candidateFrame) {
    Serial.println("[WARN] 照片評分記憶體不足，退回檔案大小評分");
    return sizeScore * PHOTO_SCORE_SIZE_WEIGHT;
  }

  if (!decodeJpegToGrayBlocks(fb, candidateFrame)) {
    free(candidateFrame);
    Serial.println("[WARN] 照片評分解碼失敗，退回檔案大小評分");
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
// 相機初始化
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
  config.xclk_freq_hz = 10000000;  // 10MHz（降低以減少 DMA 壓力）
  config.pixel_format = PIXFORMAT_JPEG;
  config.grab_mode    = CAMERA_GRAB_WHEN_EMPTY;

  // 使用 VGA（640x480），在 ESP32 Core 3.x 下最穩定
  config.frame_size   = FRAMESIZE_VGA;   // 640x480
  config.jpeg_quality = 15;              // 較高壓縮，加速上傳
  config.fb_count     = 2;
  config.fb_location  = CAMERA_FB_IN_PSRAM;

  esp_err_t err = esp_camera_init(&config);
  if (err != ESP_OK) {
    Serial.printf("[ERROR] 相機初始化失敗: 0x%x\n", err);
    return false;
  }

  // NMKCAM 特有設定：關閉自動白平衡抖動（減少誤觸發）
  sensor_t* s = esp_camera_sensor_get();
  s->set_whitebal(s, 1);
  s->set_awb_gain(s, 1);
  s->set_wb_mode(s, 0);

  Serial.println("[OK] 相機初始化完成");
  return true;
}

// ============================
// 切換相機解析度
// ============================
void setCameraFrameSize(framesize_t size) {
  sensor_t* s = esp_camera_sensor_get();
  s->set_framesize(s, size);
  // 丟棄前幾幀讓感測器穩定
  for (int i = 0; i < 2; i++) {
    camera_fb_t* fb = esp_camera_fb_get();
    if (fb) esp_camera_fb_return(fb);
    delay(200);
  }
}

// ============================
// 從 NVS 載入 WiFi 設定
// ============================
void loadWiFiConfig() {
  preferences.begin("easycam", true);  // 唯讀
  String ssid = preferences.getString("ssid", "");
  String pass = preferences.getString("password", "");
  preferences.end();

  if (ssid.length() > 0) {
    ssid.toCharArray(storedSSID, sizeof(storedSSID));
    pass.toCharArray(storedPassword, sizeof(storedPassword));
    Serial.printf("[WiFi] 從 NVS 載入: SSID=%s\n", storedSSID);
  } else {
    // NVS 沒有存過，使用預設值
    strncpy(storedSSID, DEFAULT_WIFI_SSID, sizeof(storedSSID));
    strncpy(storedPassword, DEFAULT_WIFI_PASSWORD, sizeof(storedPassword));
    Serial.println("[WiFi] NVS 無設定，使用預設值");
  }
}

// ============================
// 儲存 WiFi 設定到 NVS
// ============================
void saveWiFiConfig(const char* ssid, const char* password) {
  preferences.begin("easycam", false);  // 讀寫
  preferences.putString("ssid", ssid);
  preferences.putString("password", password);
  preferences.end();
  Serial.printf("[WiFi] 已儲存新設定: SSID=%s\n", ssid);
}

// ============================
// 清除 NVS 中的 WiFi 設定（進入 AP 模式）
// ============================
void clearWiFiConfig() {
  preferences.begin("easycam", false);
  preferences.remove("ssid");
  preferences.remove("password");
  preferences.end();
  Serial.println("[WiFi] 已清除 NVS 設定");
}

// ============================
// AP 設定模式 — WiFi 設定網頁
// ============================
const char AP_HTML[] PROGMEM = R"rawliteral(
<!DOCTYPE html><html><head>
<meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1">
<title>EasyCam WiFi 設定</title>
<style>
body{font-family:sans-serif;max-width:400px;margin:40px auto;padding:0 20px;background:#1a1a2e;color:#eee}
h2{color:#e94560;text-align:center}
input,select{width:100%;padding:12px;margin:8px 0;border:1px solid #555;border-radius:6px;background:#16213e;color:#eee;font-size:16px;box-sizing:border-box}
button{width:100%;padding:14px;background:#e94560;color:#fff;border:none;border-radius:6px;font-size:18px;cursor:pointer;margin-top:16px}
button:hover{background:#c23152}
.info{font-size:13px;color:#aaa;text-align:center;margin-top:20px}
#nets{margin:8px 0}
</style></head><body>
<h2>📷 EasyCam WiFi 設定</h2>
<form action="/save" method="POST">
<label>WiFi 網路名稱 (SSID)</label>
<div id="nets"></div>
<input type="text" name="ssid" id="ssid" placeholder="輸入或從上方選擇" required>
<label>WiFi 密碼</label>
<input type="password" name="password" placeholder="輸入 WiFi 密碼" required>
<button type="submit">💾 儲存並重新連線</button>
</form>
<p class="info">設定完成後 EasyCam 將自動重啟並連線到新的 WiFi。<br>如果連線失敗，會再次開啟此設定頁面。</p>
<script>
fetch('/scan').then(r=>r.json()).then(nets=>{
  let h='<select onchange="document.getElementById(\'ssid\').value=this.value"><option value="">-- 選擇偵測到的網路 --</option>';
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
  Serial.println("  📡 WiFi 設定模式");
  Serial.printf("  AP 名稱: %s\n", AP_SSID);
  Serial.printf("  AP 密碼: %s\n", AP_PASSWORD);
  Serial.printf("  設定網址: http://%s\n", apIP.toString().c_str());
  Serial.println("=============================\n");

  // DNS 攔截 — 所有網址都導到設定頁面
  dnsServer.start(53, "*", apIP);

  // 設定頁面
  webServer.on("/", HTTP_GET, []() {
    webServer.send(200, "text/html", AP_HTML);
  });

  // WiFi 掃描 API
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

  // 儲存設定
  webServer.on("/save", HTTP_POST, []() {
    String newSSID = webServer.arg("ssid");
    String newPass = webServer.arg("password");

    if (newSSID.length() > 0) {
      saveWiFiConfig(newSSID.c_str(), newPass.c_str());
      webServer.send(200, "text/html",
        "<html><head><meta charset='utf-8'></head><body style='font-family:sans-serif;text-align:center;padding:60px;background:#1a1a2e;color:#eee'>"
        "<h2>✅ 設定已儲存！</h2><p>EasyCam 將在 3 秒後重新啟動...</p></body></html>");
      delay(3000);
      ESP.restart();
    } else {
      webServer.send(400, "text/html", "<h2>SSID 不能為空</h2>");
    }
  });

  // 所有未知路徑也導到設定頁（Captive Portal）
  webServer.onNotFound([]() {
    webServer.sendHeader("Location", "/", true);
    webServer.send(302, "text/plain", "");
  });

  webServer.begin();

  // LED 快閃表示 AP 模式
  Serial.println("[AP] 等待使用者設定 WiFi...");
  while (apModeActive) {
    dnsServer.processNextRequest();
    webServer.handleClient();

    // LED 雙閃表示 AP 模式
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
// WiFi 連線（帶 NVS + AP 備援）
// ============================
void connectWiFi() {
  loadWiFiConfig();

  WiFi.mode(WIFI_STA);
  Serial.printf("[WiFi] 連線到 %s", storedSSID);
  WiFi.begin(storedSSID, storedPassword);
  WiFi.setSleep(false);

  int retry = 0;
  while (WiFi.status() != WL_CONNECTED && retry < 20) {
    delay(500);
    Serial.print(".");
    retry++;
  }

  if (WiFi.status() == WL_CONNECTED) {
    Serial.printf("\n[OK] WiFi 已連線，IP: %s\n", WiFi.localIP().toString().c_str());
    return;
  }

  // 連線失敗 → 進入 AP 設定模式
  Serial.println("\n[WARN] WiFi 連線失敗，進入設定模式...");
  startAPMode();
}

// ============================
// MQTT 回呼函式
// ============================
void mqttCallback(char* topic, byte* payload, unsigned int length) {
  String message = "";
  for (unsigned int i = 0; i < length; i++) {
    message += (char)payload[i];
  }
  Serial.printf("[MQTT] 收到指令: %s\n", message.c_str());

  if (message == "capture") {
    Serial.println("[MQTT] 執行遠端拍照...");
    captureAndUpload("remote");
    lastMotionTime = millis();
  } else if (message == "pause") {
    motionDetectionEnabled = false;
    resetMotionBaseline("pause");
    Serial.println("[MQTT] ⏸ 動態偵測已暫停");
    sendStatusReport();
  } else if (message == "resume") {
    motionDetectionEnabled = true;
    resetMotionBaseline("resume");
    Serial.println("[MQTT] ▶ 動態偵測已恢復");
    sendStatusReport();
  } else if (message == "reset_wifi") {
    Serial.println("[MQTT] 收到重置 WiFi 指令，進入設定模式...");
    clearWiFiConfig();
    delay(1000);
    ESP.restart();
  } else if (message == "flip") {
    sensor_t* s = esp_camera_sensor_get();
    int current = s->status.vflip;
    s->set_vflip(s, !current);
    resetMotionBaseline("flip");
    Serial.printf("[MQTT] 🔄 畫面翻轉: %s\n", !current ? "ON" : "OFF");
    sendStatusReport();
  } else if (message == "mirror") {
    sensor_t* s = esp_camera_sensor_get();
    int current = s->status.hmirror;
    s->set_hmirror(s, !current);
    resetMotionBaseline("mirror");
    Serial.printf("[MQTT] 🪞 畫面鏡像: %s\n", !current ? "ON" : "OFF");
    sendStatusReport();
  } else if (message == "info") {
    Serial.println("[MQTT] 回報系統狀態...");
    sendStatusReport();
  } else if (message.startsWith("schedule ")) {
    String param = message.substring(9);
    if (param == "off") {
      scheduleEnabled = false;
      Serial.println("[MQTT] ⏰ 排程靜音已關閉");
      sendStatusReport();
    } else {
      int h1, m1, h2, m2;
      if (sscanf(param.c_str(), "%d:%d-%d:%d", &h1, &m1, &h2, &m2) == 4) {
        quietStartHour = h1;
        quietStartMin = m1;
        quietEndHour = h2;
        quietEndMin = m2;
        scheduleEnabled = true;
        Serial.printf("[MQTT] ⏰ 排程靜音: %02d:%02d ~ %02d:%02d\n", h1, m1, h2, m2);
        sendStatusReport();
      }
    }
  }
}

// ============================
// MQTT 連線
// ============================
void connectMQTT() {
  espClient.setInsecure();  // HiveMQ Cloud 用 TLS，這裡跳過憑證驗證
  mqttClient.setKeepAlive(120);  // 延長 keepalive 到 120 秒（預設 15 秒太短，上傳照片期間會斷線）
  mqttClient.setServer(MQTT_BROKER, MQTT_PORT);
  mqttClient.setCallback(mqttCallback);
  mqttClient.setBufferSize(512);

  int retry = 0;
  while (!mqttClient.connected() && retry < 1) {
    Serial.printf("[MQTT] 連線到 %s...\n", MQTT_BROKER);
    if (mqttClient.connect(MQTT_CLIENT_ID, MQTT_USER, MQTT_PASSWORD)) {
      Serial.println("[OK] MQTT 已連線");
      mqttClient.subscribe(MQTT_TOPIC_CMD);
      Serial.printf("[OK] 已訂閱 %s\n", MQTT_TOPIC_CMD);
    } else {
      Serial.printf("[ERROR] MQTT 連線失敗，rc=%d\n", mqttClient.state());
      retry++;
    }
  }
}

// ============================
// JPEG 解碼 → 灰階 → 區塊平均值（正確的動態偵測）
// ============================
// 參考：alanesq/CameraWifiMotion、s60sc/ESP32-CAM_MJPEG2SD
// 原理：用 fmt2rgb888() 將 JPEG 解碼為 RGB 像素，轉灰階後做區塊比較
// 舊方法（直接比較 JPEG raw bytes）會因 JPEG 壓縮每次不同而產生 60-70% 假變化

bool decodeJpegToGrayBlocks(camera_fb_t* fb, uint8_t* blockBuffer) {
  // --- 第一步：JPEG → RGB888（使用 esp32-camera 內建函式）---
  // VGA(640x480) 需要 640*480*3 = 921,600 bytes，放在 PSRAM
  size_t rgbLen = fb->width * fb->height * 3;
  uint8_t* rgbBuf = (uint8_t*)ps_malloc(rgbLen);
  if (!rgbBuf) {
    Serial.println("[ERROR] PSRAM 不足，無法解碼 JPEG");
    return false;
  }

  // fmt2rgb888：esp32-camera 提供的 JPEG → RGB888 解碼器
  bool ok = fmt2rgb888(fb->buf, fb->len, PIXFORMAT_JPEG, rgbBuf);
  if (!ok) {
    Serial.println("[ERROR] JPEG 解碼失敗");
    free(rgbBuf);
    return false;
  }

  // --- 第二步：RGB → 灰階，降採樣到 DETECT_WIDTH x DETECT_HEIGHT，計算區塊平均值 ---
  int srcW = fb->width;   // 實際影像寬（如 640）
  int srcH = fb->height;  // 實際影像高（如 480）

  int totalBlocks = blockCols * blockRows;
  // 每個區塊對應原始影像的像素範圍
  float scaleX = (float)srcW / DETECT_WIDTH;
  float scaleY = (float)srcH / DETECT_HEIGHT;

  for (int by = 0; by < blockRows; by++) {
    for (int bx = 0; bx < blockCols; bx++) {
      // 這個區塊在原始影像中對應的像素範圍
      int startX = (int)(bx * BLOCK_SIZE * scaleX);
      int endX   = (int)((bx + 1) * BLOCK_SIZE * scaleX);
      int startY = (int)(by * BLOCK_SIZE * scaleY);
      int endY   = (int)((by + 1) * BLOCK_SIZE * scaleY);

      // 限制範圍
      if (endX > srcW) endX = srcW;
      if (endY > srcH) endY = srcH;

      // 計算區塊內所有像素的灰階平均值
      long sum = 0;
      int count = 0;
      for (int y = startY; y < endY; y++) {
        for (int x = startX; x < endX; x++) {
          int idx = (y * srcW + x) * 3;  // RGB888: 每像素 3 bytes
          uint8_t r = rgbBuf[idx];
          uint8_t g = rgbBuf[idx + 1];
          uint8_t b = rgbBuf[idx + 2];
          // ITU-R BT.601 灰階轉換（人眼對綠色最敏感）
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
  Serial.printf("[MOTION] 重建偵測基準: %s\n", reason);
}

// ============================
// 偵測畫面差異
// ============================
bool detectMotion(camera_fb_t* fb) {
  // 暗處防誤判：JPEG 檔太小表示畫面全黑（雜訊會造成假動態）
  if (fb->len < 3000) {
    static bool darkWarned = false;
    if (!darkWarned) {
      Serial.printf("[INFO] 畫面太暗（%d bytes），暫停偵測\n", fb->len);
      darkWarned = true;
    }
    return false;
  }

  int totalBlocks = blockCols * blockRows;

  // 分配當前幀的區塊 buffer（放在 PSRAM）
  uint8_t* currentFrame = (uint8_t*)ps_malloc(totalBlocks);
  if (!currentFrame) {
    Serial.println("[ERROR] 記憶體不足");
    return false;
  }

  // 解碼 JPEG 並計算區塊灰階值
  if (!decodeJpegToGrayBlocks(fb, currentFrame)) {
    free(currentFrame);
    return false;
  }

  // 第一次執行，沒有前一幀可比對
  if (prevFrame == NULL) {
    prevFrame = currentFrame;
    Serial.println("[INFO] 第一幀已儲存，下一幀開始比對");
    return false;
  }

  // 比對區塊差異
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

  // 計算變化百分比
  float changePercent = (float)changedBlocks / totalBlocks * 100.0;
  float avgChangedDiff = changedBlocks > 0 ? (float)changedDiffSum / changedBlocks : 0.0;

  // 除錯用：每 10 次輸出一次靜態變化率，方便校準門檻
  static int frameCount = 0;
  frameCount++;
  if (frameCount % 10 == 0) {
    Serial.printf("[DEBUG] 變化: %.1f%%（%d/%d），強變化:%d，平均差:%.1f，最大差:%d\n",
                  changePercent, changedBlocks, totalBlocks, strongBlocks, avgChangedDiff, maxDiff);
  }

  if (changePercent >= GLOBAL_CHANGE_IGNORE_PERCENT) {
    Serial.printf("[MOTION] 忽略全畫面亮度變化 %.1f%%（疑似曝光/光線變化）\n", changePercent);
    free(prevFrame);
    prevFrame = currentFrame;
    return false;
  }

  if (changePercent > MOTION_PERCENT || strongBlocks >= MOTION_STRONG_BLOCKS) {
    Serial.printf("[MOTION] 可疑動態，等待確認：變化 %.1f%%（%d/%d），強變化:%d，平均差:%.1f，最大差:%d\n",
                  changePercent, changedBlocks, totalBlocks, strongBlocks, avgChangedDiff, maxDiff);
    free(currentFrame);
    return true;
  }

  // 只有在未觸發時才更新基準。可疑幀不覆蓋基準，讓第二階段確認仍能和原本背景比對。
  free(prevFrame);
  prevFrame = currentFrame;
  return false;
}

// ============================
// 拍照並上傳（核心函式）
// ============================
void captureAndUpload(const char* trigger) {
  Serial.printf("[CAPTURE] 觸發來源: %s\n", trigger);

  // LED 快閃表示正在拍照
  for (int i = 0; i < 3; i++) {
    setStatusLed(255);
    delay(50);
    setStatusLed(0);
    delay(50);
  }

  // 已經在 SVGA 解析度，不需要切換
  // 丟棄一幀讓感測器穩定
  delay(400);
  camera_fb_t* discard = getCameraFrameWithRetry(5, 250, "拍照前丟棄幀");
  if (discard) esp_camera_fb_return(discard);
  delay(300);

  // 連拍 3 張，用混合分數選最佳照片：
  // 變化區域比例 + 強變化區塊數 + JPEG 檔案大小（清晰度近似）
  camera_fb_t* bestPhoto = NULL;
  float bestScore = -1.0;

  for (int i = 0; i < 3; i++) {
    camera_fb_t* fb = getCameraFrameWithRetry(5, 250, "連拍");
    if (!fb) {
      Serial.printf("[ERROR] 第 %d 張拍攝失敗\n", i + 1);
      continue;
    }
    float changePercent = 0.0;
    int strongBlocks = 0;
    float score = scorePhotoCandidate(fb, &changePercent, &strongBlocks);

    Serial.printf("[CAPTURE] 第 %d 張: %d bytes, 變化 %.1f%%, 強變化 %d, 分數 %.1f\n",
                  i + 1, fb->len, changePercent, strongBlocks, score);

    if (score > bestScore) {
      // 釋放之前的最佳照片
      if (bestPhoto) esp_camera_fb_return(bestPhoto);
      bestPhoto = fb;
      bestScore = score;
    } else {
      esp_camera_fb_return(fb);
    }
    delay(200);  // 短暫間隔讓感測器穩定
  }

  if (!bestPhoto) {
    Serial.println("[ERROR] 所有拍攝都失敗");
    return;
  }

  Serial.printf("[CAPTURE] 選擇最佳照片: %d bytes, 分數 %.1f\n", bestPhoto->len, bestScore);

  // 上傳到 Cloudflare Worker
  uploadPhoto(bestPhoto, trigger);

  // 釋放照片
  esp_camera_fb_return(bestPhoto);

  // LED 長亮 1 秒表示完成
  setStatusLed(255);
  delay(1000);
  setStatusLed(0);

  resetMotionBaseline(trigger);
}

// ============================
// HTTP 上傳照片到 Worker
// ============================
void uploadPhoto(camera_fb_t* fb, const char* trigger) {
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("[ERROR] WiFi 未連線，無法上傳");
    return;
  }

  Serial.printf("[UPLOAD] Free heap: %d, PSRAM: %d\n", ESP.getFreeHeap(), ESP.getFreePsram());

  WiFiClientSecure client;
  client.setInsecure();  // Cloudflare Workers 用公開 TLS
  client.setTimeout(30);  // 30 秒 TLS 逾時

  HTTPClient http;
  http.begin(client, WORKER_URL);  // HTTPS 需要 WiFiClientSecure
  http.addHeader("Content-Type", "image/jpeg");
  http.addHeader("X-Trigger", trigger);  // 告訴 Worker 觸發來源
  http.setTimeout(30000);  // 30 秒逾時（ESP32 TLS 握手慢）

  Serial.printf("[UPLOAD] 正在上傳 %d bytes 到 Worker...\n", fb->len);
  int httpCode = http.POST(fb->buf, fb->len);

  if (httpCode > 0) {
    String response = http.getString();
    Serial.printf("[UPLOAD] 回應碼: %d, 內容: %s\n", httpCode, response.c_str());
  } else {
    Serial.printf("[ERROR] 上傳失敗: %s\n", http.errorToString(httpCode).c_str());
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
  Serial.printf("[ALARM] 門口警報事件開始，將保留 %d 張記錄\n", ALARM_SEQUENCE_SHOTS);

  for (int i = 0; i < ALARM_SEQUENCE_SHOTS; i++) {
    Serial.printf("[ALARM] 事件記錄 %d/%d\n", i + 1, ALARM_SEQUENCE_SHOTS);
    captureAndUpload("motion");

    if (i < ALARM_SEQUENCE_SHOTS - 1) {
      waitWithCommandPolling(ALARM_SEQUENCE_INTERVAL_MS);
    }
  }

  lastMotionTime = millis();
  Serial.printf("[ALARM] 事件記錄完成，短冷卻 %d 秒\n", COOLDOWN_MS / 1000);
}

// ============================
// LED 呼吸燈（待機狀態）
// ============================
void ledBreath() {
  static unsigned long lastUpdate = 0;
  static int brightness = 0;
  static int direction = 5;

  if (millis() - lastUpdate > 30) {
    lastUpdate = millis();
    brightness += direction;
    if (brightness >= 255 || brightness <= 0) direction = -direction;
    setStatusLed(brightness);  // ESP32 用 LEDC 取代 analogWrite
  }
}

// ============================
// 檢查是否在排程靜音時段
// ============================
bool isInQuietHours() {
  if (!scheduleEnabled) return false;

  struct tm timeinfo;
  if (!getLocalTime(&timeinfo, 1000)) return false;

  int nowMin = timeinfo.tm_hour * 60 + timeinfo.tm_min;
  int startMin = quietStartHour * 60 + quietStartMin;
  int endMin = quietEndHour * 60 + quietEndMin;

  if (startMin <= endMin) {
    // 同一天內（如 08:00-18:00）
    return nowMin >= startMin && nowMin < endMin;
  } else {
    // 跨午夜（如 23:00-07:00）
    return nowMin >= startMin || nowMin < endMin;
  }
}

// ============================
// 發送系統狀態報告到 Worker → Telegram
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
  Serial.printf("[STATUS] 回報結果: HTTP %d\n", code);
  http.end();
}

// ============================
// setup()
// ============================
void setup() {
  WRITE_PERI_REG(RTC_CNTL_BROWN_OUT_REG, 0);  // 停用欠壓偵測

  Serial.begin(115200);
  Serial.println("\n=============================");
  Serial.println("  EasyCam 感應監視器 v1.0");
  Serial.println("  NMKing 小霸王實驗室");
  Serial.println("=============================\n");

  // LED 設定（ESP32 用 LEDC PWM）
  initStatusLed();

  // 拍照按鈕 IO3 與 Serial RX 共用，監控模式下不用 Serial 輸入所以可以用
  // 注意：如果 Serial Monitor 送資料進來可能會誤觸發
  pinMode(BUTTON_PIN, INPUT_PULLUP);

  // 初始化相機
  if (!initCamera()) {
    Serial.println("[FATAL] 相機初始化失敗，系統停止");
    while (1) {
      setStatusLed(255);
      delay(200);
      setStatusLed(0);
      delay(200);
    }
  }

  // 計算區塊網格
  blockCols = DETECT_WIDTH / BLOCK_SIZE;   // 40
  blockRows = DETECT_HEIGHT / BLOCK_SIZE;  // 30
  Serial.printf("[INFO] 偵測網格: %dx%d = %d 區塊\n", blockCols, blockRows, blockCols * blockRows);

  // 連線 WiFi
  connectWiFi();

  // 連線 MQTT（目前預設關閉，主要指令通道是 Worker /poll）
#if ENABLE_MQTT_COMMANDS
  connectMQTT();
#else
  Serial.println("[INFO] MQTT 指令通道已停用，使用 Worker /poll 輪詢");
#endif

  // 設定 NTP 時間（排程功能需要）
  configTime(8 * 3600, 0, "pool.ntp.org", "time.nist.gov");
  bootTime = millis();

  // LED 亮一下表示就緒
  setStatusLed(255);
  delay(2000);
  setStatusLed(0);

  Serial.println("\n[READY] 系統就緒，開始監控...\n");
}

// ============================
// loop()
// ============================
void loop() {
  // 維持 MQTT 連線（選用；避免 HiveMQ 不通時阻塞主要輪詢通道）
#if ENABLE_MQTT_COMMANDS
  if (!mqttClient.connected()) {
    connectMQTT();
  }
  mqttClient.loop();
#endif

  // WiFi 斷線重連
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("[WARN] WiFi 斷線，重新連線...");
    connectWiFi();
  }

  // 輪詢 Worker 指令：每 3 秒檢查一次（主要指令通道）
  // 放在動態偵測前，確保冷卻期間也能收到 Telegram 指令。
  static unsigned long lastPoll = 0;
  if (WiFi.status() == WL_CONNECTED && millis() - lastPoll > 3000) {
    lastPoll = millis();
    pollWorkerCommand();
  }

  // 檢查實體拍照按鈕（IO3，低電位觸發）
  if (digitalRead(BUTTON_PIN) == LOW) {
    delay(50);  // 去彈跳
    if (digitalRead(BUTTON_PIN) == LOW) {
      Serial.println("[BUTTON] 按鈕觸發拍照");
      captureAndUpload("button");
      // 等待按鈕放開
      while (digitalRead(BUTTON_PIN) == LOW) delay(10);
      lastMotionTime = millis();  // 重置冷卻
    }
  }

  // 畫面差異偵測（排程靜音時段自動跳過）
  if (motionDetectionEnabled && !isInQuietHours()) {
    if (millis() - bootTime < MOTION_STARTUP_GRACE_MS) {
      ledBreath();  // 開機後先讓曝光穩定
    }
    // 冷卻中就跳過
    else if (millis() - lastMotionTime < COOLDOWN_MS) {
      ledBreath();  // 冷卻中顯示呼吸燈
    } else {
      // 拍一幀做偵測
      camera_fb_t* fb = getCameraFrameWithRetry(3, 150, "偵測");
      if (fb) {
        bool suspiciousMotion = detectMotion(fb);
        esp_camera_fb_return(fb);

        if (suspiciousMotion) {
          waitWithCommandPolling(MOTION_CONFIRM_DELAY_MS);

          camera_fb_t* confirmFb = getCameraFrameWithRetry(3, 150, "二階段確認");
          if (confirmFb) {
            bool confirmedMotion = detectMotion(confirmFb);
            esp_camera_fb_return(confirmFb);

            if (confirmedMotion) {
              Serial.println("[MOTION] 二階段確認通過，啟動門口警報事件");
              captureAlarmSequence();
            } else {
              Serial.println("[MOTION] 二階段確認未通過，忽略本次可疑變化");
            }
          }
        }
      }
    }
  }

  // 待機 LED 微閃（每 3 秒閃一下表示正常運作）
  static unsigned long lastBlink = 0;
  if (millis() - lastBlink > 3000) {
    lastBlink = millis();
    setStatusLed(255);
    delay(30);
    setStatusLed(0);
  }

  delay(200);  // 偵測間隔（每秒約 5 幀偵測）
}

// ============================
// 輪詢 Worker 指令（MQTT 備用方案）
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
    // 解析 command 欄位，統一交給 mqttCallback 處理
    int cmdStart = response.indexOf("\"command\":\"");
    if (cmdStart >= 0) {
      cmdStart += 11;
      int cmdEnd = response.indexOf("\"", cmdStart);
      if (cmdEnd > cmdStart) {
        String cmd = response.substring(cmdStart, cmdEnd);
        Serial.printf("[POLL] 收到指令: %s\n", cmd.c_str());
        mqttCallback((char*)"easycam/cmd", (byte*)cmd.c_str(), cmd.length());
      }
    }
  } else if (httpCode < 0) {
    Serial.printf("[POLL] 輪詢失敗: %s\n", http.errorToString(httpCode).c_str());
  } else {
    Serial.printf("[POLL] Worker 回應 HTTP %d\n", httpCode);
  }
  http.end();
}
