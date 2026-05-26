#!/bin/bash
# EasyCam Worker — Set up Cloudflare Secrets
# EasyCam Worker — 設定 Cloudflare Secrets
#
# Usage / 用法：cd worker && bash setup-secrets.sh
#
# This script uses `wrangler secret put` to set all required env vars.
# Each command will prompt you to enter the value (hidden from screen).
# 這個腳本會用 wrangler secret put 設定所有必要的環境變數。
# 每個指令會提示你輸入值（不會顯示在螢幕上）。

echo "=== EasyCam Worker Secret Setup / Secret 設定工具 ==="
echo ""
echo "Enter each secret when prompted (input is hidden):"
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
echo "=== All done! Now redeploy: npx wrangler deploy ==="
echo "=== 全部完成！現在可以重新部署：npx wrangler deploy ==="
