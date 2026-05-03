// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "chrome/browser/ui/views/toolbar/rsp_toolbar_button.h"

#include <memory>
#include <string>

#include "base/functional/bind.h"
#include "base/strings/utf_string_conversions.h"
#include "chrome/browser/profiles/profile.h"
#include "chrome/browser/rsp/rsp_config.h"
#include "chrome/browser/rsp/rsp_connection_manager.h"
#include "chrome/browser/ui/browser.h"
#include "chrome/browser/ui/tabs/tab_strip_model.h"
#include "chrome/browser/ui/views/chrome_layout_provider.h"
#include "chrome/grit/generated_resources.h"
#include "components/vector_icons/vector_icons.h"
#include "content/public/browser/web_contents.h"
#include "ui/base/l10n/l10n_util.h"
#include "ui/base/models/dialog_model.h"
#include "ui/base/metadata/metadata_impl_macros.h"
#include "ui/base/mojom/dialog_button.mojom.h"
#include "ui/views/accessibility/view_accessibility.h"
#include "ui/views/bubble/bubble_dialog_delegate_view.h"
#include "ui/views/bubble/bubble_dialog_model_host.h"
#include "ui/views/controls/label.h"
#include "ui/views/controls/textfield/textfield.h"
#include "ui/views/layout/box_layout.h"
#include "ui/views/layout/table_layout.h"
#include "ui/views/view_class_properties.h"
#include "ui/views/widget/widget.h"

namespace {

bool IsRspProfile(Profile* profile) {
  return profile && profile->IsOffTheRecord() &&
         profile->GetOTRProfileID().IsRspTab();
}

std::u16string ToStatusText(const RspConnectionHealth& health) {
  std::u16string status = u"RM: ";
  status += health.rm_reachable ? u"reachable" : u"unreachable";
  status += u"\nbsd_sockets: ";
  if (!health.rs_configured) {
    status += u"not selected";
  } else {
    status += health.rs_reachable ? u"reachable" : u"unreachable";
  }
  if (!health.message.empty()) {
    status += u"\n" + base::UTF8ToUTF16(health.message);
  }
  return status;
}

bool ApplyRspConfig(Profile* profile,
                    views::Textfield* rm_field,
                    views::Textfield* rs_field,
                    views::Label* status_label) {
  RspTabConfig config;
  config.rm_addr = base::UTF16ToUTF8(std::u16string(rm_field->GetText()));
  config.rs_node_id = base::UTF16ToUTF8(std::u16string(rs_field->GetText()));
  if (config.rm_addr.empty()) {
    config.rm_addr = "127.0.0.1:3939";
  }

  const bool applied =
      RspConnectionManager::GetInstance()->SetConfigForProfile(profile, config);
  if (status_label) {
    status_label->SetText(ToStatusText(
        RspConnectionManager::GetInstance()->GetHealthForProfile(profile)));
  }
  return applied;
}

void ShowRspConfigBubble(views::View* anchor_view, Profile* profile) {
  auto* manager = RspConnectionManager::GetInstance();
  RspTabConfig config = manager->GetConfigForProfile(profile);
  if (config.rm_addr.empty()) {
    config.rm_addr = "127.0.0.1:3939";
  }

  auto content = std::make_unique<views::View>();
  auto* layout = content->SetLayoutManager(std::make_unique<views::BoxLayout>(
      views::BoxLayout::Orientation::kVertical, gfx::Insets(), 8));
  layout->set_cross_axis_alignment(
      views::BoxLayout::CrossAxisAlignment::kStretch);

  views::Label* status_label =
      content->AddChildView(std::make_unique<views::Label>());
  status_label->SetHorizontalAlignment(gfx::ALIGN_LEFT);
  status_label->SetMultiLine(true);
  status_label->SetText(ToStatusText(manager->GetHealthForProfile(profile)));

  auto* grid = content->AddChildView(std::make_unique<views::View>());
  auto* table = grid->SetLayoutManager(std::make_unique<views::TableLayout>());
  table->AddColumn(views::LayoutAlignment::kStart,
                   views::LayoutAlignment::kCenter,
                   views::TableLayout::kFixedSize,
                   views::TableLayout::ColumnSize::kUsePreferred, 0, 0);
  table->AddPaddingColumn(views::TableLayout::kFixedSize, 8);
  table->AddColumn(views::LayoutAlignment::kStretch,
                   views::LayoutAlignment::kCenter, 1.0f,
                   views::TableLayout::ColumnSize::kUsePreferred, 0, 320);
  table->AddRows(2, views::TableLayout::kFixedSize);

  grid->AddChildView(std::make_unique<views::Label>(u"RM server"));
  views::Textfield* rm_field =
      grid->AddChildView(std::make_unique<views::Textfield>());
  rm_field->SetText(base::UTF8ToUTF16(config.rm_addr));
  rm_field->SetPlaceholderText(u"127.0.0.1:3939");

  grid->AddChildView(std::make_unique<views::Label>(u"bsd_sockets Node ID"));
  views::Textfield* rs_field =
      grid->AddChildView(std::make_unique<views::Textfield>());
  rs_field->SetText(base::UTF8ToUTF16(config.rs_node_id));
  rs_field->SetPlaceholderText(u"xxxxxxxx-xxxx-xxxx-xxxx-xxxxxxxxxxxx");

  auto model = ui::DialogModel::Builder()
                   .SetTitle(u"RSP tab proxy")
                   .AddCustomField(
                       std::make_unique<views::BubbleDialogModelHost::CustomView>(
                           std::move(content),
                           views::BubbleDialogModelHost::FieldType::kControl,
                           rm_field))
                   .AddOkButton(base::BindRepeating(&ApplyRspConfig, profile,
                                                    rm_field, rs_field,
                                                    status_label),
                                ui::DialogModel::Button::Params().SetLabel(
                                    u"Apply"))
                   .AddCancelButton(base::DoNothing())
                   .Build();

  auto bubble = std::make_unique<views::BubbleDialogModelHost>(
      std::move(model), anchor_view, views::BubbleBorder::TOP_RIGHT);
  views::BubbleDialogDelegate::CreateBubble(std::move(bubble))->Show();
}

}  // namespace

RspToolbarButton::RspToolbarButton(Browser* browser)
    : ToolbarButton(base::BindRepeating(&RspToolbarButton::ButtonPressed,
                                        base::Unretained(this))),
      browser_(browser) {
  SetVectorIcon(vector_icons::kRouterIcon);
  SetTooltipText(u"RSP proxy settings");
  GetViewAccessibility().SetName(u"RSP proxy settings");
  SetVisible(false);
}

RspToolbarButton::~RspToolbarButton() = default;

// static
void RspToolbarButton::ShowBubbleForProfile(views::View* anchor_view,
                                            Profile* profile) {
  if (!anchor_view || !IsRspProfile(profile)) {
    return;
  }

  ShowRspConfigBubble(anchor_view, profile);
}

void RspToolbarButton::UpdateForWebContents(content::WebContents* web_contents) {
  Profile* profile = web_contents
                         ? Profile::FromBrowserContext(
                               web_contents->GetBrowserContext())
                         : nullptr;
  SetVisible(IsRspProfile(profile));
}

void RspToolbarButton::ButtonPressed(const ui::Event& event) {
  Profile* profile = GetActiveRspProfile();
  if (!profile) {
    return;
  }

  ShowBubbleForProfile(this, profile);
}

Profile* RspToolbarButton::GetActiveRspProfile() const {
  content::WebContents* active_contents =
      browser_->tab_strip_model()->GetActiveWebContents();
  Profile* profile = active_contents
                         ? Profile::FromBrowserContext(
                               active_contents->GetBrowserContext())
                         : nullptr;
  return IsRspProfile(profile) ? profile : nullptr;
}

BEGIN_METADATA(RspToolbarButton)
END_METADATA
