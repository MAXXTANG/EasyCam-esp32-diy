# EasyCam — $15 ESP32-CAM 門口監控攝影機

用不到 $15 美元的硬體，打造一台會自動偵測動態、拍照推送到 LINE 和 Telegram 的門口監控攝影機。雲端完全免費（Cloudflare Workers 免費方案）。

## 為什麼做這個？

市面上的智慧門鈴攝影機（Ring、Google Nest）動輒 $100–200 美元，還要月付雲端儲存。這個專案用一塊 ESP32-CAM 開發板 + 免費雲端服務，實現核心功能：**有人經過門口 → 自動拍照 → 手機收到通知**。

## 功能

- **智慧動態偵測** — 區塊式灰階差異比對，不是簡單的 JPEG 比較（那會 70% 誤判）
- **連拍選圖** — 動態觸發後連拍 3 張，用混合分數選最清晰的上傳
- **雙通道通知** — LINE + Telegram 同時推送
- **Telegram 遠端控制** — 拍照、暫停/恢復偵測、翻轉鏡像、排程靜音、查狀態
- **遠端 WiFi 重設** — 透過 AP 模式 Captive Portal，不用重新燒錄
- **暗處防誤判** — 自動偵測全黑環境，不會半夜狂推通知

## 系統架構

```
ESP32-CAM ──(HTTPS)──> Cloudflare Worker ──> LINE Messaging API
                              │               Telegram Bot API
                              │
Telegram Bot ──(Webhook)──────┘
                              │
                    Cloudflare KV（暫存指令）
                              │
ESP32-CAM <──(HTTP 輪詢)──────┘
```

## 硬體成本

| 零件 | 價格 | 備註 |
|------|------|------|
| ESP32-CAM 模組 | ~$5 | AI Thinker 或相容模組 |
| 開發板/擴充板 | ~$5 | USB 直接燒錄，不需額外 FTDI |
| USB 線 + 外殼 | ~$5 | 隨便找個防水盒 |
| **總計** | **~$15** | 雲端服務全部免費 |

## Telegram Bot 指令

| 指令 | 說明 |
|------|------|
| `/capture` | 遠端拍照 |
| `/pause` / `/resume` | 暫停/恢復動態偵測 |
| `/flip` / `/mirror` | 翻轉/鏡像畫面 |
| `/schedule 23:00-07:00` | 設定靜音時段 |
| `/schedule off` | 關閉排程靜音 |
| `/info` | 裝置詳細狀態 |
| `/status` | 快速檢查 Worker |
| `/reset_wifi` | 重設 WiFi 設定 |
| `/help` | 顯示指令列表 |

## 快速開始

### 1. 韌體設定

```bash
cd motion_monitor
cp config.example.h config.h
# 編輯 config.h，填入你的 WiFi、MQTT、Worker URL
```

用 Arduino IDE 上傳：

- 開發板：**AI Thinker ESP32-CAM**
- Partition Scheme：**Huge APP (3MB No OTA/1MB SPIFFS)**
- 需安裝 `PubSubClient` 函式庫

或用 PlatformIO：

```bash
pio run -t upload
```

### 2. 部署 Cloudflare Worker

```bash
cd worker
npm install

# 建立 KV 命名空間
npx wrangler kv namespace create IMAGES
# 把回傳的 id 填入 wrangler.toml

# 設定敏感環境變數
npx wrangler secret put LINE_CHANNEL_ACCESS_TOKEN
npx wrangler secret put LINE_USER_ID
npx wrangler secret put TELEGRAM_BOT_TOKEN
npx wrangler secret put TELEGRAM_CHAT_ID
npx wrangler secret put HIVEMQ_URL
npx wrangler secret put HIVEMQ_USER
npx wrangler secret put HIVEMQ_PASSWORD

# 部署
npx wrangler deploy
```

### 3. 設定 Telegram Webhook

```
https://api.telegram.org/bot<YOUR_BOT_TOKEN>/setWebhook?url=https://<YOUR_WORKER>/webhook/telegram
```

### 4. 需要申請的免費服務

| 服務 | 用途 | 連結 |
|------|------|------|
| Cloudflare Workers | 雲端中繼 + KV 儲存 | https://workers.cloudflare.com |
| LINE Messaging API | LINE 推送通知 | https://developers.line.biz |
| Telegram BotFather | Telegram Bot | https://t.me/BotFather |
| HiveMQ Cloud | MQTT Broker | https://www.hivemq.com/cloud |

## 專案結構

```
EasyCam-esp32-diy/
├── motion_monitor/
│   ├── motion_monitor.ino     # ESP32 主程式（動態偵測 + 拍照上傳）
│   ├── config.h               # 你的設定（⚠️ gitignored）
│   └── config.example.h       # 設定範本
├── worker/
│   ├── src/worker.js          # Cloudflare Worker（路由 + 通知 + 指令）
│   ├── wrangler.toml          # Worker 部署設定（⚠️ gitignored）
│   ├── wrangler.toml.example  # 部署設定範本
│   ├── .dev.vars.example      # 本地開發環境變數範本
│   ├── setup-secrets.sh       # 一鍵設定 Cloudflare Secrets
│   └── package.json
├── platformio.ini             # PlatformIO 編譯設定
├── .gitignore
└── README.md
```

## 技術筆記

### 為什麼不直接比較 JPEG？

ESP32-CAM 每次拍照的 JPEG 壓縮結果不同（即使畫面沒變），直接比較 raw bytes 會有 ~70% 誤判率。正確做法是解碼成 RGB → 轉灰階 → 分區塊計算平均值差異。

### 為什麼用 KV 輪詢而不是 MQTT？

HiveMQ Cloud 免費版不提供 REST API（port 8443 會 timeout），Cloudflare Worker 無法直接發 MQTT。解法是 Worker 把指令存入 KV，ESP32 每 3 秒 HTTP 輪詢取回。

### Cloudflare 免費版限制

KV 免費版每日 1,000 次寫入。Worker 已實作 fallback：KV 存圖失敗時仍直接發 Telegram（二進制直傳不需 KV），LINE 需要公開 URL 所以會跳過。

## 疑難排解

- **WiFi 連不上** — ESP32 只支援 2.4GHz WiFi
- **Telegram 指令沒反應** — 確認 ESP32 序列監控有 `[POLL]` 訊息，確認 Webhook 設定正確
- **LINE 沒收到** — 確認有加官方帳號為好友，LINE_USER_ID 是 U 開頭 33 字元
- **偵測太靈敏/遲鈍** — 調整 `config.h` 中的 `MOTION_THRESHOLD`（差異門檻）和 `MOTION_PERCENT`（變化百分比）
- **照片太暗** — 增加拍照前的曝光等待時間

## 授權

MIT License
