/**
 * EasyCam Notify — Cloudflare Worker
 *
 * 路由：
 *   POST /upload              ESP32 上傳照片 → 轉發 LINE + Telegram
 *   GET  /image/:id           LINE 讀取暫存照片
 *   POST /webhook/telegram    Telegram Bot webhook → 發 MQTT 拍照指令
 *   GET  /poll                ESP32 輪詢待執行指令（MQTT 備用方案）
 *   POST /status-report       ESP32 回報系統狀態 → 轉發 Telegram
 *   GET  /health              健康檢查
 */

export default {
  async fetch(request, env) {
    const url = new URL(request.url);
    const path = url.pathname;

    try {
      // ── 路由（必須 await，否則 try-catch 無法捕獲 async 錯誤）──
      if (request.method === "POST" && path === "/upload") {
        return await handleUpload(request, env);
      }
      if (request.method === "GET" && path.startsWith("/image/")) {
        return await handleImageGet(path, env);
      }
      if (request.method === "POST" && path === "/webhook/telegram") {
        return await handleTelegramWebhook(request, env);
      }
      if (request.method === "GET" && path === "/poll") {
        return await handlePoll(env);
      }
      if (request.method === "POST" && path === "/status-report") {
        return await handleStatusReport(request, env);
      }
      if (request.method === "GET" && path === "/health") {
        return json({
          status: "ok",
          service: "easycam-notify",
          env_check: {
            TELEGRAM_BOT_TOKEN: !!env.TELEGRAM_BOT_TOKEN,
            TELEGRAM_CHAT_ID: !!env.TELEGRAM_CHAT_ID,
            LINE_CHANNEL_ACCESS_TOKEN: !!env.LINE_CHANNEL_ACCESS_TOKEN,
            LINE_USER_ID: !!env.LINE_USER_ID,
            HIVEMQ_URL: !!env.HIVEMQ_URL,
            HIVEMQ_USER: !!env.HIVEMQ_USER,
            HIVEMQ_PASSWORD: !!env.HIVEMQ_PASSWORD,
            IMAGES_KV: !!env.IMAGES,
          },
        });
      }

      return json({ error: "Not found" }, 404);
    } catch (err) {
      console.error("Worker error:", err);
      return json({ error: err.message }, 500);
    }
  },
};

// ============================
// POST /upload — ESP32 上傳照片
// ============================
async function handleUpload(request, env) {
  try {
    const imageData = await request.arrayBuffer();
    const trigger = request.headers.get("X-Trigger") || "unknown";
    const imageSize = imageData.byteLength;

    console.log(`[UPLOAD] 收到照片: ${imageSize} bytes, 觸發: ${trigger}`);

    if (imageSize < 1000) {
      return json({ error: "Image too small" }, 400);
    }

    // 觸發來源文字
    const triggerText = {
      motion: "🚨 偵測到移動！",
      remote: "📸 遠端拍照完成",
      button: "📷 手動按鈕拍照",
    }[trigger] || `📷 拍照 (${trigger})`;

    const timestamp = new Date().toLocaleString("zh-TW", { timeZone: "Asia/Taipei" });
    const caption = `${triggerText}\n時間：${timestamp}`;

    // 嘗試存入 KV（供 LINE 讀取），失敗不影響 Telegram
    let imageId = null;
    let imageUrl = null;
    try {
      imageId = crypto.randomUUID();
      await env.IMAGES.put(imageId, imageData, {
        expirationTtl: 3600,
        metadata: { trigger, size: imageSize, time: new Date().toISOString() },
      });
      const workerBase = new URL(request.url).origin;
      imageUrl = `${workerBase}/image/${imageId}`;
      console.log(`[KV] 照片已存入: ${imageId}`);
    } catch (kvErr) {
      console.warn(`[KV] 存入失敗（可能達到免費額度上限）: ${kvErr.message}`);
      imageId = null;
      imageUrl = null;
    }

    // 發送通知（Telegram 直傳二進制，不需要 KV）
    const notifyTasks = [sendToTelegram(env, imageData, caption)];
    if (imageUrl) {
      notifyTasks.push(sendToLINE(env, imageUrl, caption));
    } else {
      console.log("[LINE] 跳過（KV 無法存圖，LINE 需要公開 URL）");
    }

    const results = await Promise.allSettled(notifyTasks);

    const telegramResult = results[0].status === "fulfilled"
      ? results[0].value
      : { error: String(results[0].reason) };
    const lineResult = results[1]
      ? (results[1].status === "fulfilled" ? results[1].value : { error: String(results[1].reason) })
      : { skipped: true, reason: "KV unavailable" };

    console.log("[Telegram]", JSON.stringify(telegramResult));
    console.log("[LINE]", JSON.stringify(lineResult));

    return json({
      ok: true,
      imageId,
      trigger,
      telegram: results[0].status,
      line: results[1] ? results[1].status : "skipped",
    });
  } catch (err) {
    console.error("[UPLOAD] handleUpload 內部錯誤:", err.stack || err.message || err);
    return json({ error: "Upload processing failed", detail: err.message }, 500);
  }
}

// ============================
// GET /image/:id — LINE 讀取暫存照片
// ============================
async function handleImageGet(path, env) {
  const imageId = path.replace("/image/", "");
  const imageData = await env.IMAGES.get(imageId, { type: "arrayBuffer" });

  if (!imageData) {
    return json({ error: "Image not found or expired" }, 404);
  }

  return new Response(imageData, {
    headers: {
      "Content-Type": "image/jpeg",
      "Cache-Control": "public, max-age=3600",
    },
  });
}

// ============================
// POST /webhook/telegram — Telegram 指令處理
// ============================
async function handleTelegramWebhook(request, env) {
  const update = await request.json();
  const message = update.message;

  if (!message || !message.text) {
    return json({ ok: true });
  }

  const chatId = message.chat.id.toString();
  const fromId = message.from?.id?.toString() || "";
  const text = normalizeTelegramText(message.text);

  console.log(`[Telegram] 收到訊息: "${text}" chat=${chatId} from=${fromId}`);

  // 私訊時 chat.id 會等於使用者 ID；群組裡 chat.id 是群組 ID，因此也允許授權使用者 from.id。
  if (chatId !== env.TELEGRAM_CHAT_ID && fromId !== env.TELEGRAM_CHAT_ID) {
    console.log(`[Telegram] 拒絕未授權訊息 chat=${chatId} from=${fromId}`);
    return json({ ok: true });
  }

  if (text === "/pause" || text === "暫停") {
    const mqttOk = await publishMQTT(env, "easycam/cmd", "pause");
    const replyText = mqttOk
      ? "⏸ 已暫停動態偵測。輸入 /resume 恢復。"
      : "❌ 發送指令失敗，請確認設備是否在線";
    await sendTelegramMessage(env, chatId, replyText);
    return json({ ok: true });
  }

  if (text === "/resume" || text === "恢復" || text === "復原") {
    const mqttOk = await publishMQTT(env, "easycam/cmd", "resume");
    const replyText = mqttOk
      ? "▶ 動態偵測已恢復！"
      : "❌ 發送指令失敗，請確認設備是否在線";
    await sendTelegramMessage(env, chatId, replyText);
    return json({ ok: true });
  }

  if (text === "/capture" || text === "/拍照" || text === "拍照") {
    // 發送 MQTT 指令到 ESP32
    const mqttOk = await publishMQTT(env, "easycam/cmd", "capture");

    const replyText = mqttOk
      ? "📸 已發送拍照指令，照片稍後送達..."
      : "❌ 發送指令失敗，請確認設備是否在線";

    await sendTelegramMessage(env, chatId, replyText);
    return json({ ok: true });
  }

  if (text === "/reset_wifi" || text === "重設wifi") {
    const mqttOk = await publishMQTT(env, "easycam/cmd", "reset_wifi");
    const replyText = mqttOk
      ? "📡 已發送 WiFi 重設指令。\nEasyCam 將重啟並開啟設定熱點「EasyCam-Setup」\n密碼：easycam123\n連上後開啟 http://192.168.4.1 設定新 WiFi"
      : "❌ 發送指令失敗，請確認設備是否在線";
    await sendTelegramMessage(env, chatId, replyText);
    return json({ ok: true });
  }

  if (text === "/flip" || text === "翻轉") {
    const mqttOk = await publishMQTT(env, "easycam/cmd", "flip");
    const replyText = mqttOk
      ? "🔄 已發送翻轉指令（上下翻轉切換）"
      : "❌ 發送指令失敗，請確認設備是否在線";
    await sendTelegramMessage(env, chatId, replyText);
    return json({ ok: true });
  }

  if (text === "/mirror" || text === "鏡像") {
    const mqttOk = await publishMQTT(env, "easycam/cmd", "mirror");
    const replyText = mqttOk
      ? "🪞 已發送鏡像指令（左右鏡像切換）"
      : "❌ 發送指令失敗，請確認設備是否在線";
    await sendTelegramMessage(env, chatId, replyText);
    return json({ ok: true });
  }

  if (text === "/info" || text === "資訊") {
    const mqttOk = await publishMQTT(env, "easycam/cmd", "info");
    const replyText = mqttOk
      ? "📊 正在取得系統狀態，稍候..."
      : "❌ 發送指令失敗，請確認設備是否在線";
    await sendTelegramMessage(env, chatId, replyText);
    return json({ ok: true });
  }

  if (text.startsWith("/schedule") || text.startsWith("排程")) {
    const parts = text.split(" ");
    if (parts.length < 2 || parts[1] === "off" || parts[1] === "關閉") {
      const mqttOk = await publishMQTT(env, "easycam/cmd", "schedule off");
      const replyText = mqttOk
        ? "⏰ 排程靜音已關閉"
        : "❌ 發送指令失敗";
      await sendTelegramMessage(env, chatId, replyText);
    } else {
      const timeRange = parts[1];
      const match = timeRange.match(/^(\d{1,2}):(\d{2})-(\d{1,2}):(\d{2})$/);
      if (match) {
        const mqttOk = await publishMQTT(env, "easycam/cmd", `schedule ${timeRange}`);
        const replyText = mqttOk
          ? `⏰ 排程靜音已設定：${timeRange}\n在此時段內將自動暫停動態偵測`
          : "❌ 發送指令失敗";
        await sendTelegramMessage(env, chatId, replyText);
      } else {
        await sendTelegramMessage(env, chatId, "⚠️ 格式錯誤\n正確格式：/schedule 23:00-07:00\n關閉：/schedule off");
      }
    }
    return json({ ok: true });
  }

  if (text === "/status" || text === "狀態") {
    await sendTelegramMessage(env, chatId, "🟢 Worker 正常運作中\n輸入 /info 取得裝置詳細狀態");
    return json({ ok: true });
  }

  if (text === "/help" || text === "/start") {
    const helpText = [
      "🤖 EasyCam 監控機器人",
      "",
      "📸 拍照控制：",
      "/capture 或「拍照」— 遠端拍一張照",
      "/pause 或「暫停」— 暫停動態偵測",
      "/resume 或「恢復」— 恢復動態偵測",
      "",
      "🎥 畫面調整：",
      "/flip 或「翻轉」— 上下翻轉畫面",
      "/mirror 或「鏡像」— 左右鏡像畫面",
      "",
      "⏰ 排程與狀態：",
      "/schedule 23:00-07:00 — 設定靜音時段",
      "/schedule off — 關閉排程靜音",
      "/info 或「資訊」— 裝置詳細狀態",
      "/status 或「狀態」— 快速檢查",
      "",
      "⚙️ 系統：",
      "/reset_wifi — 重設 WiFi 設定",
      "/help — 顯示此說明",
      "",
      "偵測到移動時會自動推送照片。",
    ].join("\n");

    await sendTelegramMessage(env, chatId, helpText);
    return json({ ok: true });
  }

  // 未知指令
  await sendTelegramMessage(env, chatId, "🤔 不認識這個指令，輸入 /help 看看有什麼功能");
  return json({ ok: true });
}

// ============================
// LINE Messaging API — 發送圖片
// ============================
async function sendToLINE(env, imageUrl, altText) {
  if (!env.LINE_CHANNEL_ACCESS_TOKEN || !env.LINE_USER_ID) {
    console.warn("[LINE] 缺少 LINE_CHANNEL_ACCESS_TOKEN 或 LINE_USER_ID，跳過");
    return { skipped: true, reason: "missing env vars" };
  }
  const body = {
    to: env.LINE_USER_ID,
    messages: [
      {
        type: "image",
        originalContentUrl: imageUrl,
        previewImageUrl: imageUrl,
      },
      {
        type: "text",
        text: altText,
      },
    ],
  };

  const res = await fetch("https://api.line.me/v2/bot/message/push", {
    method: "POST",
    headers: {
      "Content-Type": "application/json",
      Authorization: `Bearer ${env.LINE_CHANNEL_ACCESS_TOKEN}`,
    },
    body: JSON.stringify(body),
  });

  const result = await res.json();
  if (!res.ok) throw new Error(`LINE API error: ${JSON.stringify(result)}`);
  return result;
}

// ============================
// Telegram Bot API — 發送照片
// ============================
async function sendToTelegram(env, imageData, caption) {
  if (!env.TELEGRAM_BOT_TOKEN || !env.TELEGRAM_CHAT_ID) {
    console.warn("[Telegram] 缺少 TELEGRAM_BOT_TOKEN 或 TELEGRAM_CHAT_ID，跳過");
    return { skipped: true, reason: "missing env vars" };
  }
  const formData = new FormData();
  formData.append("chat_id", env.TELEGRAM_CHAT_ID);
  formData.append("caption", caption);
  formData.append("photo", new Blob([imageData], { type: "image/jpeg" }), "capture.jpg");

  const res = await fetch(
    `https://api.telegram.org/bot${env.TELEGRAM_BOT_TOKEN}/sendPhoto`,
    { method: "POST", body: formData }
  );

  const result = await res.json();
  if (!result.ok) throw new Error(`Telegram API error: ${JSON.stringify(result)}`);
  return result;
}

// ============================
// Telegram — 發送文字訊息
// ============================
async function sendTelegramMessage(env, chatId, text) {
  if (!env.TELEGRAM_BOT_TOKEN) {
    console.warn("[Telegram] 缺少 TELEGRAM_BOT_TOKEN，無法發送訊息");
    return { skipped: true };
  }
  const res = await fetch(
    `https://api.telegram.org/bot${env.TELEGRAM_BOT_TOKEN}/sendMessage`,
    {
      method: "POST",
      headers: { "Content-Type": "application/json" },
      body: JSON.stringify({ chat_id: chatId, text }),
    }
  );
  const result = await res.json();
  if (!result.ok) {
    console.error("[Telegram] sendMessage 失敗:", JSON.stringify(result));
  }
  return result;
}

// ============================
// KV 指令佇列 — Telegram 指令暫存給 ESP32 /poll
// ============================
async function publishMQTT(env, topic, message) {
  // 直接使用 KV 輪詢（HiveMQ Cloud 免費版不提供 REST API）
  // ESP32 每 3 秒輪詢 /poll 取指令
  try {
    await env.IMAGES.put("pending_command", message, { expirationTtl: 300 });
    console.log(`[CMD] 指令已存入 KV: ${message}`);
    return true;
  } catch (err) {
    console.error("[CMD] KV 存入失敗:", err.message);
    return false;
  }
}

// ============================
// GET /poll — ESP32 輪詢待執行指令
// ============================
async function handlePoll(env) {
  const command = await env.IMAGES.get("pending_command");
  if (command) {
    // 取出後刪除，確保只執行一次
    await env.IMAGES.delete("pending_command");
    return json({ command });
  }
  return json({ command: null });
}

// ============================
// POST /status-report — ESP32 回報狀態 → 轉發 Telegram
// ============================
async function handleStatusReport(request, env) {
  const data = await request.json();

  const statusText = [
    "📊 EasyCam 裝置狀態",
    "",
    `⏱ 運行時間：${data.uptime}`,
    `📶 WiFi：${data.wifi_ssid}（${data.wifi_rssi} dBm）`,
    `🧠 記憶體：${(data.heap / 1024).toFixed(0)}KB / PSRAM ${(data.psram / 1024).toFixed(0)}KB`,
    `👁 動態偵測：${data.motion === "on" ? "✅ 開啟" : "⏸ 暫停"}`,
    `🔄 翻轉：${data.flip ? "ON" : "OFF"} ｜ 🪞 鏡像：${data.mirror ? "ON" : "OFF"}`,
    `⏰ 排程靜音：${data.schedule === "off" ? "未設定" : data.schedule}`,
    `🕐 裝置時間：${data.time}`,
  ].join("\n");

  await sendTelegramMessage(env, env.TELEGRAM_CHAT_ID, statusText);
  return json({ ok: true });
}

// ============================
// 工具函式
// ============================
function json(data, status = 200) {
  return new Response(JSON.stringify(data), {
    status,
    headers: { "Content-Type": "application/json" },
  });
}

function normalizeTelegramText(text) {
  const normalized = text.trim().toLowerCase();
  // 群組內 Telegram 常送出 /command@BotName；本專案只需要 command 本身。
  return normalized.replace(/^\/([^\s@]+)@[^\s]+/, "/$1");
}
