// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "chrome/browser/ui/views/toolbar/rsp_toolbar_button.h"

#include <memory>
#include <string>
#include <vector>

#include "base/functional/bind.h"
#include "base/memory/weak_ptr.h"
#include "base/strings/stringprintf.h"
#include "base/strings/utf_string_conversions.h"
#include "base/task/sequenced_task_runner.h"
#include "ui/gfx/scoped_animation_duration_scale_mode.h"
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
#include "ui/base/metadata/metadata_impl_macros.h"
#include "ui/base/models/simple_combobox_model.h"
#include "ui/base/mojom/dialog_button.mojom.h"
#include "ui/views/accessibility/view_accessibility.h"
#include "ui/views/bubble/bubble_dialog_delegate_view.h"
#include "ui/views/controls/button/md_text_button.h"
#include "ui/views/controls/combobox/combobox.h"
#include "ui/views/controls/label.h"
#include "ui/views/controls/textfield/textfield.h"
#include "ui/views/layout/box_layout.h"
#include "ui/views/layout/table_layout.h"
#include "ui/views/view_class_properties.h"
#include "ui/views/widget/widget.h"

namespace {

// Returns true for OTR profiles created for RSP tabs (regardless of whether
// a bsd_sockets node has been configured yet). The button must be visible
// so the user can configure the node via the bubble.
bool IsRspProfile(Profile* profile) {
  return profile && profile->IsOffTheRecord() &&
         profile->GetOTRProfileID().IsRspTab();
}

}  // namespace

// ---------------------------------------------------------------------------
// RspConfigBubble — config bubble for an RSP tab's proxy settings.
// ---------------------------------------------------------------------------
RspConfigBubble::RspConfigBubble(views::View* anchor, Profile* profile)
    : views::BubbleDialogDelegateView(anchor, views::BubbleBorder::TOP_RIGHT),
      profile_(profile) {
  SetShowTitle(true);
  SetShowCloseButton(true);
  SetButtons(static_cast<int>(ui::mojom::DialogButton::kOk) |
             static_cast<int>(ui::mojom::DialogButton::kCancel));
  SetButtonLabel(ui::mojom::DialogButton::kOk, u"Apply");
  set_fixed_width(480);
  // Do not close on deactivation. The combobox dropdown creates a child widget
  // that briefly takes focus, which would otherwise trigger OnDeactivate →
  // animated close → layer DCHECK (IsPropertyChangeAllowed). Config dialogs
  // also benefit from staying open while the user reads another window.
  set_close_on_deactivate(false);
  // Non-activatable: prevents ShowWindow() from sending WM_ACTIVATE, which
  // Chrome handles synchronously and can trigger focus-ring layer updates
  // re-entrantly during the compositor's PaintLayerContents phase → DCHECK.
  SetCanActivate(false);
}

RspConfigBubble::~RspConfigBubble() = default;

// static
void RspConfigBubble::Show(views::View* anchor, Profile* profile) {
  DCHECK(profile);
  auto* bubble = new RspConfigBubble(anchor, profile);
  views::BubbleDialogDelegate::CreateBubble(bubble)->Show();
}

std::u16string RspConfigBubble::GetWindowTitle() const {
  return u"RSP Proxy Settings";
}

bool RspConfigBubble::Accept() {
  RspTabConfig config;
  config.rm_addr = base::UTF16ToUTF8(std::u16string(rm_field_->GetText()));
  if (config.rm_addr.empty()) {
    config.rm_addr = "127.0.0.1:3939";
  }
  auto idx = node_combobox_->GetSelectedIndex();
  if (idx.has_value() && idx.value() < node_ids_.size()) {
    config.rs_node_id = node_ids_[idx.value()];
  }
  RspConnectionManager::GetInstance()->SetConfigForProfile(profile_, config);
  return true;
}

void RspConfigBubble::Init() {
  RspTabConfig config =
      RspConnectionManager::GetInstance()->GetConfigForProfile(profile_);
  if (config.rm_addr.empty()) {
    config.rm_addr = "127.0.0.1:3939";
  }

  SetLayoutManager(std::make_unique<views::BoxLayout>(
      views::BoxLayout::Orientation::kVertical, gfx::Insets(16), 8));

  // Status line.
  status_label_ = AddChildView(std::make_unique<views::Label>());
  status_label_->SetHorizontalAlignment(gfx::ALIGN_LEFT);
  status_label_->SetMultiLine(true);
  UpdateStatusText();

  // Grid: [label | field | button]
  auto* grid = AddChildView(std::make_unique<views::View>());
  auto* table = grid->SetLayoutManager(std::make_unique<views::TableLayout>());
  table->AddColumn(views::LayoutAlignment::kStart,
                   views::LayoutAlignment::kCenter,
                   views::TableLayout::kFixedSize,
                   views::TableLayout::ColumnSize::kUsePreferred, 0, 0);
  table->AddPaddingColumn(views::TableLayout::kFixedSize, 8);
  table->AddColumn(views::LayoutAlignment::kStretch,
                   views::LayoutAlignment::kCenter, 1.0f,
                   views::TableLayout::ColumnSize::kUsePreferred, 0, 0);
  table->AddPaddingColumn(views::TableLayout::kFixedSize, 4);
  table->AddColumn(views::LayoutAlignment::kStart,
                   views::LayoutAlignment::kCenter,
                   views::TableLayout::kFixedSize,
                   views::TableLayout::ColumnSize::kUsePreferred, 0, 0);
  table->AddRows(2, views::TableLayout::kFixedSize, 0);

  // Row 1: RM server + Refresh button.
  grid->AddChildView(std::make_unique<views::Label>(u"RM server"));
  rm_field_ = grid->AddChildView(std::make_unique<views::Textfield>());
  rm_field_->SetText(base::UTF8ToUTF16(config.rm_addr));
  rm_field_->SetPlaceholderText(u"127.0.0.1:3939");
  auto* refresh_button = grid->AddChildView(std::make_unique<views::MdTextButton>(
      base::BindRepeating(&RspConfigBubble::OnRefreshClicked,
                          base::Unretained(this)),
      u"Refresh"));
  // NEVER focus: RequestFocusFromEvent() during NotifyClick would blur the
  // textfield and trigger a focus-ring layer animation that can DCHECK
  // (IsPropertyChangeAllowed()) during a compositor BeginFrame commit.
  refresh_button->SetFocusBehavior(views::View::FocusBehavior::NEVER);

  // Row 2: bsd_sockets node dropdown.
  grid->AddChildView(std::make_unique<views::Label>(u"bsd_sockets node"));
  combobox_model_ = std::make_unique<ui::SimpleComboboxModel>(
      std::vector<ui::SimpleComboboxModel::Item>{});
  node_combobox_ = grid->AddChildView(
      std::make_unique<views::Combobox>(combobox_model_.get()));
  grid->AddChildView(std::make_unique<views::Label>(u""));  // spacer

  // Hint below the grid.
  refresh_hint_ = AddChildView(std::make_unique<views::Label>());
  refresh_hint_->SetHorizontalAlignment(gfx::ALIGN_LEFT);
  refresh_hint_->SetEnabledColor(SK_ColorGRAY);
  refresh_hint_->SetText(
      u"Click Refresh to discover available bsd_sockets nodes.");

  // Pre-populate with the current node ID if one is already configured.
  if (!config.rs_node_id.empty()) {
    UpdateCombobox({config.rs_node_id}, config.rs_node_id);
  }

  // Kick off auto-discovery. Init() runs before the first render (inside
  // Widget::Init, before Show()), so there is no active compositor commit
  // and no risk of the SetHideLayerAndSubtree DCHECK.
  OnRefreshClicked();
}

void RspConfigBubble::OnRefreshClicked() {
  std::string rm_addr =
      base::UTF16ToUTF8(std::u16string(rm_field_->GetText()));
  if (rm_addr.empty()) {
    rm_addr = "127.0.0.1:3939";
  }
  pending_rm_addr_ = rm_addr;
  refresh_hint_->SetText(u"Discovering nodes\u2026");

  RspConnectionManager::GetInstance()->ListBsdSocketsNodes(
      rm_addr, base::BindOnce(&RspConfigBubble::OnNodeListReady,
                              weak_factory_.GetWeakPtr(), rm_addr));
}

void RspConfigBubble::OnNodeListReady(std::string rm_addr,
                                      std::vector<std::string> nodes) {
  // Discard stale results (user changed RM addr and clicked Refresh again).
  if (rm_addr != pending_rm_addr_) {
    return;
  }
  if (nodes.empty()) {
    refresh_hint_->SetText(
        u"No bsd_sockets nodes found. Check the RM server address.");
    return;
  }
  refresh_hint_->SetText(base::UTF8ToUTF16(base::StringPrintf(
      "Found %d node(s).", static_cast<int>(nodes.size()))));

  // Preserve the current selection if it's still in the new list.
  std::string current;
  auto idx = node_combobox_->GetSelectedIndex();
  if (idx.has_value() && idx.value() < node_ids_.size()) {
    current = node_ids_[idx.value()];
  }
  UpdateCombobox(nodes, current);
}

void RspConfigBubble::UpdateCombobox(const std::vector<std::string>& nodes,
                                     const std::string& selected) {
  node_ids_ = nodes;
  std::vector<ui::SimpleComboboxModel::Item> items;
  items.reserve(nodes.size());
  size_t selected_idx = 0;
  bool found = false;
  for (size_t i = 0; i < nodes.size(); ++i) {
    items.emplace_back(base::UTF8ToUTF16(nodes[i]));
    if (!found && nodes[i] == selected) {
      selected_idx = i;
      found = true;
    }
  }
  combobox_model_ =
      std::make_unique<ui::SimpleComboboxModel>(std::move(items));
  node_combobox_->SetModel(combobox_model_.get());
  if (!node_ids_.empty()) {
    node_combobox_->SetSelectedIndex(selected_idx);
  }
}

void RspConfigBubble::UpdateStatusText() {
  auto health =
      RspConnectionManager::GetInstance()->GetHealthForProfile(profile_);
  std::u16string status = u"RM: ";
  status += health.rm_reachable ? u"configured" : u"not configured";
  status += u"   bsd_sockets: ";
  if (!health.rs_configured) {
    status += u"not selected";
  } else {
    status += health.rs_reachable ? u"reachable" : u"configured";
  }
  if (!health.message.empty()) {
    status += u"\n" + base::UTF8ToUTF16(health.message);
  }
  status_label_->SetText(status);
}

// ---------------------------------------------------------------------------
// RspToolbarButton
// ---------------------------------------------------------------------------

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
  RspConfigBubble::Show(anchor_view, profile);
}

void RspToolbarButton::UpdateForWebContents(content::WebContents*) {
  // RSP tabs always live in a dedicated browser window whose profile is the
  // RSP OTR profile. Check the window's profile rather than the active tab's
  // browser context — individual web contents in the window share the window
  // profile and would give the same answer, but this avoids the null case
  // (web_contents==nullptr) hiding the button during security state updates.
  bool should_be_visible = IsRspProfile(browser_->profile());
  if (GetVisible() != should_be_visible) {
    SetVisible(should_be_visible);
    PreferredSizeChanged();
  }
}

void RspToolbarButton::ButtonPressed(const ui::Event& event) {
  Profile* profile = GetActiveRspProfile();
  if (!profile) {
    return;
  }
  // PostTask so the button-press event fully unwinds before the bubble widget
  // is created and activates.  Showing the bubble synchronously from inside an
  // event handler triggers a DCHECK(IsPropertyChangeAllowed()) in
  // cc::Layer::SetHideLayerAndSubtree via the focus-ring visibility update on
  // the browser window that is being deactivated.
  base::SequencedTaskRunner::GetCurrentDefault()->PostTask(
      FROM_HERE,
      base::BindOnce(&RspToolbarButton::ShowBubbleForProfile,
                     base::Unretained(this),
                     base::Unretained(profile)));
}

Profile* RspToolbarButton::GetActiveRspProfile() const {
  return IsRspProfile(browser_->profile()) ? browser_->profile() : nullptr;
}

BEGIN_METADATA(RspToolbarButton)
END_METADATA
