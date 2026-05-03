// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef CHROME_BROWSER_UI_VIEWS_TOOLBAR_RSP_TOOLBAR_BUTTON_H_
#define CHROME_BROWSER_UI_VIEWS_TOOLBAR_RSP_TOOLBAR_BUTTON_H_

#include "base/memory/raw_ptr.h"
#include "chrome/browser/ui/views/toolbar/toolbar_button.h"

class Browser;
class Profile;

namespace content {
class WebContents;
}

namespace views {
class View;
}

// Toolbar entry point shown only when the active tab is an RSP tab. The RSP
// proxy choice is stored against the active RSP OTR profile, so each RSP tab can
// use a different Resource Manager and bsd_sockets Resource Service.
class RspToolbarButton : public ToolbarButton {
  METADATA_HEADER(RspToolbarButton, ToolbarButton)

 public:
  explicit RspToolbarButton(Browser* browser);
  RspToolbarButton(const RspToolbarButton&) = delete;
  RspToolbarButton& operator=(const RspToolbarButton&) = delete;
  ~RspToolbarButton() override;

  static void ShowBubbleForProfile(views::View* anchor_view, Profile* profile);

  void UpdateForWebContents(content::WebContents* web_contents);

 private:
  void ButtonPressed(const ui::Event& event);
  Profile* GetActiveRspProfile() const;

  const raw_ptr<Browser> browser_;
};

#endif  // CHROME_BROWSER_UI_VIEWS_TOOLBAR_RSP_TOOLBAR_BUTTON_H_
