// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef CHROME_BROWSER_UI_VIEWS_TOOLBAR_RSP_TOOLBAR_BUTTON_H_
#define CHROME_BROWSER_UI_VIEWS_TOOLBAR_RSP_TOOLBAR_BUTTON_H_

#include <memory>
#include <string>
#include <vector>

#include "base/memory/raw_ptr.h"
#include "base/memory/weak_ptr.h"
#include "chrome/browser/ui/views/toolbar/toolbar_button.h"
#include "ui/base/models/simple_combobox_model.h"
#include "ui/gfx/scoped_animation_duration_scale_mode.h"
#include "ui/views/bubble/bubble_dialog_delegate_view.h"

class Browser;
class Profile;

namespace content {
class WebContents;
}

namespace views {
class Combobox;
class Label;
class Textfield;
class View;
}

// Config bubble shown when the user clicks the RSP toolbar button.
// Declared at global scope so it can be friended by BubbleDialogDelegateView.
class RspConfigBubble : public views::BubbleDialogDelegateView {
 public:
  static void Show(views::View* anchor, Profile* profile);

  explicit RspConfigBubble(views::View* anchor, Profile* profile);
  RspConfigBubble(const RspConfigBubble&) = delete;
  RspConfigBubble& operator=(const RspConfigBubble&) = delete;
  ~RspConfigBubble() override;

  // views::BubbleDialogDelegateView:
  std::u16string GetWindowTitle() const override;
  bool Accept() override;
  void Init() override;

 private:
  void OnRefreshClicked();
  void OnNodeListReady(std::string rm_addr, std::vector<std::string> nodes);
  void UpdateCombobox(const std::vector<std::string>& nodes,
                      const std::string& selected);
  void UpdateStatusText();

  raw_ptr<Profile> profile_;
  raw_ptr<views::Label> status_label_ = nullptr;
  raw_ptr<views::Textfield> rm_field_ = nullptr;
  raw_ptr<views::Combobox> node_combobox_ = nullptr;
  raw_ptr<views::Label> refresh_hint_ = nullptr;

  std::unique_ptr<ui::SimpleComboboxModel> combobox_model_;
  std::vector<std::string> node_ids_;
  std::string pending_rm_addr_;

  // Disable layer animations for the bubble's entire lifetime. Any focus
  // change inside the bubble (button click, combobox, textfield) or between
  // the bubble and browser window during activation can trigger a focus-ring
  // layer animation from inside a cc::LayerTreeHost commit, which hits
  // DCHECK(IsPropertyChangeAllowed()). ZERO_DURATION makes these animations
  // complete synchronously so the compositor never sees them mid-commit.
  gfx::ScopedAnimationDurationScaleMode zero_animation_duration_{
      gfx::ScopedAnimationDurationScaleMode::ZERO_DURATION};

  base::WeakPtrFactory<RspConfigBubble> weak_factory_{this};
};

// Toolbar entry point shown only when the active tab is an RSP tab. The RSP
// proxy choice is stored against the active RSP OTR profile, so each RSP tab
// can use a different Resource Manager and bsd_sockets Resource Service.
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
