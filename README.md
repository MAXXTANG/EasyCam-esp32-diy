# EasyCam — A $15 DIY Door Security Camera with ESP32-CAM

> **[中文說明 / Chinese version below](#中文說明)**

Build a motion-detecting door camera for under $15 in hardware. It automatically captures photos when movement is detected and pushes alerts to **LINE** and **Telegram** — all powered by free cloud services (Cloudflare Workers).

## Why This Project?

Smart doorbells like Ring or Google Nest cost $100–200+, plus monthly cloud storage fees. This project uses a single ESP32-CAM board with free cloud services to deliver the core experience: **someone walks by your door → auto snapshot → instant notification on your phone**.

## Features

- **Smart motion detection** — Block-based grayscale difference comparison (not naive JPEG comparison, which gives ~70% false positives)
- **Burst mode with best-shot selection** — Captures 3 photos per event, picks the sharpest one using a composite score
- **Dual-channel notifications** — Push to both LINE and Telegram simultaneously
- **Telegram remote control** — Capture, pause/resume detection, flip/mirror, schedule quiet hours, check status
- **Remote WiFi reset** — Reconfigure WiFi via AP mode captive portal, no reflashing needed
- **Dark scene filtering** — Auto-detects pitch-black frames to avoid false triggers at night

## Architecture

```
ESP32-CAM ──(HTTPS)──> Cloudflare Worker ──> LINE Messaging API
                              │               Telegram Bot API
                              │
Telegram Bot ──(Webhook)──────┘
                              │
                    Cloudflare KV (command queue)
                              │
ESP32-CAM <──(HTTP polling)───┘
```

## Hardware Cost

| Part | Price | Notes |
|------|-------|-------|
| ESP32-CAM module | ~$5 | AI Thinker or compatible |
| Dev board / breakout | ~$5 | USB upload, no FTDI needed |
| USB cable + enclosure | ~$5 | Any waterproof box works |
| **Total** | **~$15** | All cloud services are free |

## Telegram Bot Commands

| Command | Description |
|---------|-------------|
| `/capture` | Take a photo remotely |
| `/pause` / `/resume` | Pause / resume motion detection |
| `/flip` / `/mirror` | Flip / mirror the camera view |
| `/schedule 23:00-07:00` | Set quiet hours (no alerts) |
| `/schedule off` | Disable quiet hours |
| `/info` | Device status details |
| `/status` | Quick health check |
| `/reset_wifi` | Reset WiFi config (opens setup AP) |
| `/help` | Show command list |

## Quick Start

### 1. Firmware Setup

```bash
cd motion_monitor
cp config.example.h config.h
# Edit config.h with your WiFi, MQTT, and Worker URL
```

Upload with Arduino IDE:

- Board: **AI Thinker ESP32-CAM**
- Partition Scheme: **Huge APP (3MB No OTA / 1MB SPIFFS)**
- Install the `PubSubClient` library

Or use PlatformIO:

```bash
pio run -t upload
```

### 2. Deploy Cloudflare Worker

```bash
cd worker
npm install

# Create KV namespace
npx wrangler kv namespace create IMAGES
# Copy the returned ID into wrangler.toml

# Set secrets (sensitive env vars)
npx wrangler secret put LINE_CHANNEL_ACCESS_TOKEN
npx wrangler secret put LINE_USER_ID
npx wrangler secret put TELEGRAM_BOT_TOKEN
npx wrangler secret put TELEGRAM_CHAT_ID
npx wrangler secret put HIVEMQ_URL
npx wrangler secret put HIVEMQ_USER
npx wrangler secret put HIVEMQ_PASSWORD

# Deploy
npx wrangler deploy
```

### 3. Set Up Telegram Webhook

```
https://api.telegram.org/bot<YOUR_BOT_TOKEN>/setWebhook?url=https://<YOUR_WORKER>/webhook/telegram
```

### 4. Required Free Services

| Service | Purpose | Link |
|---------|---------|------|
| Cloudflare Workers | Cloud relay + KV storage | https://workers.cloudflare.com |
| LINE Messaging API | LINE push notifications | https://developers.line.biz |
| Telegram BotFather | Telegram Bot | https://t.me/BotFather |
| HiveMQ Cloud | MQTT Broker | https://www.hivemq.com/cloud |

## Project Structure

```
EasyCam-esp32-diy/
├── motion_monitor/
│   ├── motion_monitor.ino     # ESP32 main firmware (motion detect + upload)
│   ├── config.h               # Your config (⚠️ gitignored)
│   └── config.example.h       # Config template
├── worker/
│   ├── src/worker.js          # Cloudflare Worker (routing + notifications + commands)
│   ├── wrangler.toml          # Worker deploy config (⚠️ gitignored)
│   ├── wrangler.toml.example  # Deploy config template
│   ├── .dev.vars.example      # Local dev env vars template
│   ├── setup-secrets.sh       # One-click Cloudflare Secrets setup
│   └── package.json
├── platformio.ini             # PlatformIO build config
├── .gitignore
└── README.md
```

## Technical Notes

### Why Not Compare JPEGs Directly?

The ESP32-CAM produces different JPEG output each time even with an identical scene (compression artifacts vary). Comparing raw bytes yields ~70% false positive rate. The correct approach: decode to RGB → convert to grayscale → compare block-averaged values.

### Why HTTP Polling Instead of Pure MQTT?

HiveMQ Cloud's free tier doesn't expose a REST API (port 8443 times out), and Cloudflare Workers can't send MQTT. Solution: Worker stores commands in KV, ESP32 polls `/poll` every 3 seconds via HTTP.

### Cloudflare Free Tier Limits

KV free tier allows 1,000 writes/day. The Worker implements fallback: if KV image storage fails, it still sends directly to Telegram (binary upload, no KV needed). LINE requires a public URL, so it's skipped when KV is unavailable.

## Troubleshooting

- **WiFi won't connect** — ESP32 only supports 2.4GHz WiFi
- **Telegram commands not working** — Check serial monitor for `[POLL]` messages; verify webhook is set correctly
- **LINE not receiving** — Make sure you've added the official account as a friend; LINE_USER_ID must be a 33-char string starting with `U`
- **Too sensitive / not sensitive enough** — Adjust `MOTION_THRESHOLD` (pixel diff threshold) and `MOTION_PERCENT` (change percentage) in `config.h`
- **Photos too dark** — Increase the exposure stabilization delay before capture

## License

MIT License

---

# 中文說明

## EasyCam — $15 美元 ESP32-CAM DIY 門口監控攝影機

用不到 $15 美元的硬體，打造一台會自動偵測動態、拍照推送到 LINE 和 Telegram 的門口監控攝影機。雲端完全免費（Cloudflare Workers 免費方案）。

### 為什麼做這個？

市面上的智慧門鈴（Ring、Google Nest）動輒 $100–200 美元，還要月付雲端儲存費。這個專案用一塊 ESP32-CAM + 免費雲端服務，實現核心功能：**有人經過門口 → 自動拍照 → 手機收到通知**。

### 功能

- **智慧動態偵測** — 區塊式灰階差異比對，避免 JPEG 直接比較造成 70% 誤判
- **連拍選圖** — 每次觸發連拍 3 張，用混合分數選最清晰的一張上傳
- **雙通道通知** — LINE + Telegram 同時推送
- **Telegram 遠端控制** — 拍照、暫停/恢復偵測、翻轉鏡像、排程靜音、查狀態
- **遠端 WiFi 重設** — 透過 AP 模式 Captive Portal，不用重新燒錄
- **暗處防誤判** — 自動偵測全黑環境，不會半夜狂推通知

### 硬體成本

| 零件 | 價格 | 備註 |
|------|------|------|
| ESP32-CAM 模組 | ~$5 | AI Thinker 或相容模組 |
| 開發板/擴充板 | ~$5 | USB 直接燒錄，不需額外 FTDI |
| USB 線 + 外殼 | ~$5 | 隨便找個防水盒 |
| **總計** | **~$15** | 雲端服務全部免費 |

詳細設定步驟請參考上方英文說明，或查看各 `.example` 範本檔案中的註解。
