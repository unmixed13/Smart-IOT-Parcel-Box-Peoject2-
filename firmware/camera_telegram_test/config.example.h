// Copy this file to config.h and fill in your values. config.h is git-ignored:
// it holds the Wi-Fi password and the Telegram bot token - never commit it.
#pragma once

#define WIFI_SSID          "your-wifi-name"
#define WIFI_PASSWORD      "your-wifi-password"

// 1) In Telegram open @BotFather -> /newbot -> copy the token it gives you.
// 2) Send any message to your new bot, then open
//    https://api.telegram.org/bot<TOKEN>/getUpdates  and copy  "chat":{"id": ... }
//    (for a group the id is negative).
#define TELEGRAM_BOT_TOKEN "123456789:replace-with-your-token"
#define TELEGRAM_CHAT_ID   "123456789"
