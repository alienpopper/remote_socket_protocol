// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef CHROME_BROWSER_RSP_RSP_SETTINGS_HANDLER_H_
#define CHROME_BROWSER_RSP_RSP_SETTINGS_HANDLER_H_

#include "content/public/browser/web_ui_message_handler.h"

namespace base {
class ListValue;
}

// WebUI message handler for RSP settings.
// Exposes chrome.send() messages to the Settings page:
//   getRspDefaultRm  → replies with the current default RM address string
//   setRspDefaultRm  → accepts a string and persists it in profile prefs
class RspSettingsHandler : public content::WebUIMessageHandler {
 public:
  RspSettingsHandler();
  ~RspSettingsHandler() override;

  RspSettingsHandler(const RspSettingsHandler&) = delete;
  RspSettingsHandler& operator=(const RspSettingsHandler&) = delete;

  // content::WebUIMessageHandler:
  void RegisterMessages() override;

 private:
  void HandleGetRspDefaultRm(const base::ListValue& args);
  void HandleSetRspDefaultRm(const base::ListValue& args);
};

#endif  // CHROME_BROWSER_RSP_RSP_SETTINGS_HANDLER_H_
