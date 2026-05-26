/**
 * EasyCam Notify — Cloudflare Worker
 *
 * Routes:
 *   POST /upload              ESP32 uploads photo → forward to LINE + Telegram
 *   GET  /image/:id           LINE fetches cached photo from KV
 *   POST /webhook/telegram    Telegram Bot webhook → queue command for ESP32
 *   GET  /poll                ESP32 polls for pending commands (KV-based)
 *   POST /status-report       ESP32 reports system status → forward to Telegram
 *   GET  /health              Health check
 */

export default {
  async fetch(request, env) {
    const url = new URL(request.url);
    const path = url.pathname;

    try {
      // ── Routes (must await, otherwise try-catch can't catch async errors) ──
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
// POST /upload — ESP32 photo upload
// ============================
async function handleUpload(request, env) {
  try {
    const imageData = await request.arrayBuffer();
    const trigger = request.headers.get("X-Trigger") || "unknown";
    const imageSize = imageData.byteLength;

    console.log(`[UPLOAD] Received photo: ${imageSize} bytes, trigger: ${trigger}`);

    if (imageSize < 1000) {
      return json({ error: "Image too small" }, 400);
    }

    // Trigger source display text (bilingual)
    const triggerText = {
      motion: "🚨 Motion detected! / 偵測到移動！",
      remote: "📸 Remote capture done / 遠端拍照完成",
      button: "📷 Button capture / 手動按鈕拍照",
    }[trigger] || `📷 Capture (${trigger})`;

    const timestamp = new Date().toLocaleString("zh-TW", { timeZone: "Asia/Taipei" });
    const caption = `${triggerText}\nTime / 時間：${timestamp}`;

    // Try storing in KV (for LINE to read); failure won't affect Telegram
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
      console.log(`[KV] Photo stored: ${imageId}`);
    } catch (kvErr) {
      console.warn(`[KV] Store failed (likely hit free tier limit): ${kvErr.message}`);
      imageId = null;
      imageUrl = null;
    }

    // Send notifications (Telegram uses binary upload, no KV needed)
    // 發送通知（Telegram 直傳二進制，不需要 KV）
    const notifyTasks = [sendToTelegram(env, imageData, caption)];
    if (imageUrl) {
      notifyTasks.push(sendToLINE(env, imageUrl, caption));
    } else {
      console.log("[LINE] Skipped (KV unavailable, LINE requires a public URL)");
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
    console.error("[UPLOAD] Internal error:", err.stack || err.message || err);
    return json({ error: "Upload processing failed", detail: err.message }, 500);
  }
}

// ============================
// GET /image/:id — Serve cached photo for LINE
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
// POST /webhook/telegram — Telegram command handler
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

  console.log(`[Telegram] Received: "${text}" chat=${chatId} from=${fromId}`);

  // In DMs, chat.id equals user ID; in groups, chat.id is the group ID,
  // so we also allow authorized user's from.id.
  // 私訊時 chat.id = 使用者 ID；群組裡 chat.id 是群組 ID，因此也允許 from.id。
  if (chatId !== env.TELEGRAM_CHAT_ID && fromId !== env.TELEGRAM_CHAT_ID) {
    console.log(`[Telegram] Unauthorized message rejected: chat=${chatId} from=${fromId}`);
    return json({ ok: true });
  }

  if (text === "/pause" || text === "暫停") {
    const mqttOk = await publishMQTT(env, "easycam/cmd", "pause");
    const replyText = mqttOk
      ? "⏸ Motion detection paused. Send /resume to resume.\n動態偵測已暫停，輸入 /resume 恢復。"
      : "❌ Command failed. Is the device online?\n發送指令失敗，請確認設備是否在線";
    await sendTelegramMessage(env, chatId, replyText);
    return json({ ok: true });
  }

  if (text === "/resume" || text === "恢復" || text === "復原") {
    const mqttOk = await publishMQTT(env, "easycam/cmd", "resume");
    const replyText = mqttOk
      ? "▶ Motion detection resumed!\n動態偵測已恢復！"
      : "❌ Command failed. Is the device online?\n發送指令失敗，請確認設備是否在線";
    await sendTelegramMessage(env, chatId, replyText);
    return json({ ok: true });
  }

  if (text === "/capture" || text === "/拍照" || text === "拍照") {
    const mqttOk = await publishMQTT(env, "easycam/cmd", "capture");
    const replyText = mqttOk
      ? "📸 Capture command sent, photo incoming...\n已發送拍照指令，照片稍後送達..."
      : "❌ Command failed. Is the device online?\n發送指令失敗，請確認設備是否在線";
    await sendTelegramMessage(env, chatId, replyText);
    return json({ ok: true });
  }

  if (text === "/reset_wifi" || text === "重設wifi") {
    const mqttOk = await publishMQTT(env, "easycam/cmd", "reset_wifi");
    const replyText = mqttOk
      ? "📡 WiFi reset command sent.\nEasyCam will reboot and open setup AP \"EasyCam-Setup\"\nPassword: easycam123\nConnect and visit http://192.168.4.1 to set new WiFi"
      : "❌ Command failed. Is the device online?\n發送指令失敗，請確認設備是否在線";
    await sendTelegramMessage(env, chatId, replyText);
    return json({ ok: true });
  }

  if (text === "/flip" || text === "翻轉") {
    const mqttOk = await publishMQTT(env, "easycam/cmd", "flip");
    const replyText = mqttOk
      ? "🔄 Flip command sent (vertical toggle)\n已發送翻轉指令（上下翻轉切換）"
      : "❌ Command failed. Is the device online?\n發送指令失敗，請確認設備是否在線";
    await sendTelegramMessage(env, chatId, replyText);
    return json({ ok: true });
  }

  if (text === "/mirror" || text === "鏡像") {
    const mqttOk = await publishMQTT(env, "easycam/cmd", "mirror");
    const replyText = mqttOk
      ? "🪞 Mirror command sent (horizontal toggle)\n已發送鏡像指令（左右鏡像切換）"
      : "❌ Command failed. Is the device online?\n發送指令失敗，請確認設備是否在線";
    await sendTelegramMessage(env, chatId, replyText);
    return json({ ok: true });
  }

  if (text === "/info" || text === "資訊") {
    const mqttOk = await publishMQTT(env, "easycam/cmd", "info");
    const replyText = mqttOk
      ? "📊 Fetching device status, please wait...\n正在取得系統狀態，稍候..."
      : "❌ Command failed. Is the device online?\n發送指令失敗，請確認設備是否在線";
    await sendTelegramMessage(env, chatId, replyText);
    return json({ ok: true });
  }

  if (text.startsWith("/schedule") || text.startsWith("排程")) {
    const parts = text.split(" ");
    if (parts.length < 2 || parts[1] === "off" || parts[1] === "關閉") {
      const mqttOk = await publishMQTT(env, "easycam/cmd", "schedule off");
      const replyText = mqttOk
        ? "⏰ Quiet hours disabled / 排程靜音已關閉"
        : "❌ Command failed / 發送指令失敗";
      await sendTelegramMessage(env, chatId, replyText);
    } else {
      const timeRange = parts[1];
      const match = timeRange.match(/^(\d{1,2}):(\d{2})-(\d{1,2}):(\d{2})$/);
      if (match) {
        const mqttOk = await publishMQTT(env, "easycam/cmd", `schedule ${timeRange}`);
        const replyText = mqttOk
          ? `⏰ Quiet hours set: ${timeRange}\nMotion detection will pause during this period\n排程靜音已設定，此時段內將暫停動態偵測`
          : "❌ Command failed / 發送指令失敗";
        await sendTelegramMessage(env, chatId, replyText);
      } else {
        await sendTelegramMessage(env, chatId, "⚠️ Invalid format / 格式錯誤\nUsage: /schedule 23:00-07:00\nDisable: /schedule off");
      }
    }
    return json({ ok: true });
  }

  if (text === "/status" || text === "狀態") {
    await sendTelegramMessage(env, chatId, "🟢 Worker is running / Worker 正常運作中\nSend /info for device details / 輸入 /info 取得裝置詳細狀態");
    return json({ ok: true });
  }

  if (text === "/help" || text === "/start") {
    const helpText = [
      "🤖 EasyCam Security Bot",
      "",
      "📸 Capture / 拍照控制：",
      "/capture — Take a photo / 遠端拍一張照",
      "/pause — Pause detection / 暫停動態偵測",
      "/resume — Resume detection / 恢復動態偵測",
      "",
      "🎥 Camera / 畫面調整：",
      "/flip — Flip vertically / 上下翻轉",
      "/mirror — Mirror horizontally / 左右鏡像",
      "",
      "⏰ Schedule & Status / 排程與狀態：",
      "/schedule 23:00-07:00 — Set quiet hours / 設定靜音時段",
      "/schedule off — Disable quiet hours / 關閉排程靜音",
      "/info — Device status / 裝置詳細狀態",
      "/status — Quick check / 快速檢查",
      "",
      "⚙️ System / 系統：",
      "/reset_wifi — Reset WiFi config / 重設 WiFi",
      "/help — Show this help / 顯示此說明",
      "",
      "Photos are pushed automatically when motion is detected.",
      "偵測到移動時會自動推送照片。",
    ].join("\n");

    await sendTelegramMessage(env, chatId, helpText);
    return json({ ok: true });
  }

  // Unknown command / 未知指令
  await sendTelegramMessage(env, chatId, "🤔 Unknown command. Send /help to see available commands.\n不認識這個指令，輸入 /help 看看有什麼功能");
  return json({ ok: true });
}

// ============================
// LINE Messaging API — Send image
// ============================
async function sendToLINE(env, imageUrl, altText) {
  if (!env.LINE_CHANNEL_ACCESS_TOKEN || !env.LINE_USER_ID) {
    console.warn("[LINE] Missing LINE_CHANNEL_ACCESS_TOKEN or LINE_USER_ID, skipping");
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
// Telegram Bot API — Send photo
// ============================
async function sendToTelegram(env, imageData, caption) {
  if (!env.TELEGRAM_BOT_TOKEN || !env.TELEGRAM_CHAT_ID) {
    console.warn("[Telegram] Missing TELEGRAM_BOT_TOKEN or TELEGRAM_CHAT_ID, skipping");
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
// Telegram — Send text message
// ============================
async function sendTelegramMessage(env, chatId, text) {
  if (!env.TELEGRAM_BOT_TOKEN) {
    console.warn("[Telegram] Missing TELEGRAM_BOT_TOKEN, cannot send message");
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
    console.error("[Telegram] sendMessage failed:", JSON.stringify(result));
  }
  return result;
}

// ============================
// KV Command Queue — Store Telegram commands for ESP32 /poll
// KV 指令佇列 — 暫存 Telegram 指令供 ESP32 輪詢
// ============================
async function publishMQTT(env, topic, message) {
  // Uses KV polling (HiveMQ Cloud free tier has no REST API)
  // 使用 KV 輪詢（HiveMQ Cloud 免費版不提供 REST API）
  // ESP32 polls /poll every 3 seconds to fetch commands
  try {
    await env.IMAGES.put("pending_command", message, { expirationTtl: 300 });
    console.log(`[CMD] Command stored in KV: ${message}`);
    return true;
  } catch (err) {
    console.error("[CMD] KV store failed:", err.message);
    return false;
  }
}

// ============================
// GET /poll — ESP32 polls for pending commands
// ============================
async function handlePoll(env) {
  const command = await env.IMAGES.get("pending_command");
  if (command) {
    // Delete after reading to ensure single execution
    // 取出後刪除，確保只執行一次
    await env.IMAGES.delete("pending_command");
    return json({ command });
  }
  return json({ command: null });
}

// ============================
// POST /status-report — ESP32 reports status → forward to Telegram
// ============================
async function handleStatusReport(request, env) {
  const data = await request.json();

  const statusText = [
    "📊 EasyCam Device Status / 裝置狀態",
    "",
    `⏱ Uptime / 運行時間：${data.uptime}`,
    `📶 WiFi：${data.wifi_ssid}（${data.wifi_rssi} dBm）`,
    `🧠 Memory / 記憶體：${(data.heap / 1024).toFixed(0)}KB / PSRAM ${(data.psram / 1024).toFixed(0)}KB`,
    `👁 Detection / 動態偵測：${data.motion === "on" ? "✅ ON" : "⏸ Paused"}`,
    `🔄 Flip：${data.flip ? "ON" : "OFF"} ｜ 🪞 Mirror：${data.mirror ? "ON" : "OFF"}`,
    `⏰ Quiet hours / 排程靜音：${data.schedule === "off" ? "Off / 未設定" : data.schedule}`,
    `🕐 Device time / 裝置時間：${data.time}`,
  ].join("\n");

  await sendTelegramMessage(env, env.TELEGRAM_CHAT_ID, statusText);
  return json({ ok: true });
}

// ============================
// Utilities / 工具函式
// ============================
function json(data, status = 200) {
  return new Response(JSON.stringify(data), {
    status,
    headers: { "Content-Type": "application/json" },
  });
}

function normalizeTelegramText(text) {
  const normalized = text.trim().toLowerCase();
  // In groups, Telegram sends /command@BotName; we only need the command itself.
  // 群組內 Telegram 常送出 /command@BotName；只需要 command 本身。
  return normalized.replace(/^\/([^\s@]+)@[^\s]+/, "/$1");
}
