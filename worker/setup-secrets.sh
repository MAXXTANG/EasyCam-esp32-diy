#!/bin/bash
# EasyCam Worker — 設定 Cloudflare Secrets
# 用法：cd worker && bash setup-secrets.sh
#
# 這個腳本會用 wrangler secret put 設定所有必要的環境變數
# 每個指令會提示你輸入值（不會顯示在螢幕上）

echo "=== EasyCam Worker Secret 設定工具 ==="
echo ""
echo "請依序輸入各項 Secret（輸入時不會顯示在螢幕上）："
echo ""

echo "--- HiveMQ MQTT ---"
npx wrangler secret put HIVEMQ_URL
npx wrangler secret put HIVEMQ_USER
npx wrangler secret put HIVEMQ_PASSWORD

echo ""
echo "--- LINE ---"
npx wrangler secret put LINE_CHANNEL_ACCESS_TOKEN
npx wrangler secret put LINE_USER_ID

echo ""
echo "--- Telegram ---"
npx wrangler secret put TELEGRAM_BOT_TOKEN
npx wrangler secret put TELEGRAM_CHAT_ID

echo ""
echo "=== 全部完成！==="
echo "現在可以重新部署：npx wrangler deploy"
