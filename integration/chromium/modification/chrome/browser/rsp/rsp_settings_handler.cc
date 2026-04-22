// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "chrome/browser/rsp/rsp_settings_handler.h"

#include "base/values.h"
#include "chrome/browser/profiles/profile.h"
#include "chrome/browser/rsp/rsp_prefs.h"
#include "components/prefs/pref_service.h"
#include "content/public/browser/web_ui.h"

RspSettingsHandler::RspSettingsHandler() = default;
RspSettingsHandler::~RspSettingsHandler() = default;

void RspSettingsHandler::RegisterMessages() {
  web_ui()->RegisterMessageCallback(
      "getRspDefaultRm",
      base::BindRepeating(&RspSettingsHandler::HandleGetRspDefaultRm,
                          base::Unretained(this)));
  web_ui()->RegisterMessageCallback(
      "setRspDefaultRm",
      base::BindRepeating(&RspSettingsHandler::HandleSetRspDefaultRm,
                          base::Unretained(this)));
}

void RspSettingsHandler::HandleGetRspDefaultRm(const base::ListValue& args) {
  AllowJavascript();
  CHECK_EQ(1u, args.size());
  const std::string& callback_id = args[0].GetString();

  Profile* profile = Profile::FromWebUI(web_ui());
  const std::string& rm_addr =
      profile->GetPrefs()->GetString(rsp::prefs::kRspDefaultRmAddr);
  ResolveJavascriptCallback(base::Value(callback_id), base::Value(rm_addr));
}

void RspSettingsHandler::HandleSetRspDefaultRm(const base::ListValue& args) {
  CHECK_EQ(1u, args.size());
  const std::string& rm_addr = args[0].GetString();

  Profile* profile = Profile::FromWebUI(web_ui());
  profile->GetPrefs()->SetString(rsp::prefs::kRspDefaultRmAddr, rm_addr);
}
