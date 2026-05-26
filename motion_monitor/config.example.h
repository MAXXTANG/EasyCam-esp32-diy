/*
 * EasyCam Configuration Template
 * EasyCam 設定檔範本
 *
 * How to use / 使用方式：
 * 1. Copy this file as config.h / 複製此檔案並命名為 config.h
 * 2. Fill in your WiFi, MQTT, and Worker settings / 填入你自己的設定
 * 3. config.h is gitignored and won't be uploaded / config.h 已加入 .gitignore
 */

#ifndef CONFIG_H
#define CONFIG_H

// ============================
// WiFi Settings / WiFi 設定
// ============================
// Default WiFi (used on first boot or after NVS reset)
// 預設 WiFi（首次啟動或 NVS 清除後使用）
#define WIFI_SSID          "your-wifi-ssid"
#define WIFI_PASSWORD      "your-wifi-password"

// AP mode for remote WiFi reset (hotspot name & password)
// WiFi 設定 AP 模式（遠端重設 WiFi 時會啟動此熱點）
#define AP_SSID_NAME       "EasyCam-Setup"
#define AP_PASSWORD_STR    "easycam123"

// ============================
// Cloudflare Worker URL
// ============================
// Get this URL after deploying the Worker
// 部署 Worker 後取得的網址
#define WORKER_UPLOAD_URL  "https://your-worker.your-subdomain.workers.dev/upload"
#define WORKER_POLL_URL    "https://your-worker.your-subdomain.workers.dev/poll"
#define WORKER_STATUS_URL  "https://your-worker.your-subdomain.workers.dev/status-report"

// ============================
// MQTT Settings (HiveMQ Cloud)
// MQTT 設定（HiveMQ Cloud）
// ============================
// Free signup: https://www.hivemq.com/cloud/
// 免費申請：https://www.hivemq.com/cloud/
#define MQTT_BROKER_HOST   "your-cluster-id.s1.eu.hivemq.cloud"
#define MQTT_BROKER_PORT   8883
#define MQTT_USERNAME      "your-mqtt-username"
#define MQTT_PASS          "your-mqtt-password"
#define MQTT_TOPIC         "easycam/cmd"
#define MQTT_CLIENT        "easycam-001"

#endif // CONFIG_H
