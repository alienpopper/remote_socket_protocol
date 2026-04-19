# RSP Chromium Integration Plan

## Problem Statement

Integrate RSP (Remote Socket Protocol) into Chromium as two distinct workflows:

1. **RSP Tab** — A new tab type (and incognito variant) that routes all its HTTP/HTTPS traffic through a user-selected RSP bsd_sockets resource service. Tabs sharing the same bsd_socket RS share a single RM connection.
2. **`rsp://` URI Scheme** — Typing `rsp://<uuid>` or `rsp://<name>` uses a globally-configured default RM connection to browse a website exposed via an RSP http_server RS.

---

## Architecture Overview

### Workflow 1: RSP Tabs

```
User clicks "New RSP Tab"
  → RSP Config Dialog (RM address, endorsements, bsd_socket RS selection)
  → Creates an OTR Profile with OTRProfileID "RSP::Tab::<uuid>"
  → Profile stores RSP config (RM addr, RS node ID, endorsement key)
  → RspConnectionManager returns/creates a shared RSPClient for (RM, RS)
  → New WebContents opens in that profile
  → All network requests go through RspURLLoaderFactory
       → RspURLLoaderFactory forwards TCP through the bsd_socket RS
       → HTTP response returned to renderer
  → Tab shows RSP badge in tab strip
  → Right-click link → "Open in New RSP Tab" → inherits same RSP profile
  → Last RSP tab using that (RM, RS) closes → RSPClient disconnects
```

### Workflow 2: rsp:// URI Scheme

```
User types rsp://<uuid-or-name> in address bar
  → URL parsed with registered "rsp" scheme
  → Navigation intercepted by RspSchemeURLLoaderFactory
  → If <name>: resolves via NameService using default RM connection
  → Connects to http_server RS at resolved node ID
  → HTTP request forwarded over RSP TCP connection
  → Response returned to renderer as normal page load
```

---

## Key Chromium Extension Points (from source exploration)

| Concern | Extension Point | Key File |
|---|---|---|
| RSP tab profile isolation | `Profile::OTRProfileID::CreateUnique("RSP::Tab")` | `chrome/browser/profiles/profile.cc` |
| Per-tab network routing | `ContentBrowserClient::WillCreateURLLoaderFactory()` | `chrome/browser/chrome_content_browser_client.cc:6502` |
| Network context config | `ProfileNetworkContextService::ConfigureNetworkContextParams()` | `chrome/browser/net/profile_network_context_service.cc:597` |
| rsp:// scheme registration | `url::AddStandardScheme("rsp", ...)` | `url/url_util.cc` + `chrome/common/url_constants.cc` |
| rsp:// navigation handling | `RegisterNonNetworkNavigationURLLoaderFactories()` | `chrome/browser/chrome_content_browser_client.cc` |
| New Tab button menu | `NewTabButtonMenuModel::ExecuteCommand()` | `chrome/browser/ui/views/tabs/new_tab_button.cc:91` |
| Context menu "open in tab" | `IDC_CONTENT_CONTEXT_OPENLINKNEWTAB` handler | `chrome/browser/renderer_context_menu/render_view_context_menu.cc:3309` |
| Tab visual badge | Tab rendering | `chrome/browser/ui/views/tabs/tab.cc` |
| Command IDs | `IDC_*` constants | `chrome/app/chrome_command_ids.h` |

---

## New Files to Create

### Core RSP browser service layer
```
chrome/browser/rsp/
  BUILD.gn
  rsp_profile_manager.h/.cc        # Shared RSPClient registry keyed by (RM addr, RS node ID)
  rsp_url_loader_factory.h/.cc     # URLLoaderFactory routing HTTP through bsd_socket RS
  rsp_scheme_url_loader_factory.h/.cc  # Handles rsp:// scheme navigations
  rsp_keyed_service.h/.cc          # Per-RSP-Profile keyed service holding config
  rsp_config.h                     # Struct: RspTabConfig { rm_addr, rs_node_id, ns_node_id, keys }
  rsp_connection_manager.h/.cc     # Singleton: maps (RM,RS) -> shared RSPClient, ref-counted
```

### UI
```
chrome/browser/ui/views/rsp/
  rsp_tab_config_dialog.h/.cc      # Modal dialog: RM addr, endorsement, RS picker
  rsp_tab_indicator_view.h/.cc     # Badge shown in tab strip for RSP tabs
```

### Settings WebUI
```
chrome/browser/ui/webui/rsp/
  rsp_settings_ui.h/.cc            # chrome://settings/rsp — default RM config
  rsp_settings_handler.h/.cc       # Mojo handler for settings page
  rsp_settings.html/.css/.ts       # Settings page frontend
```

---

## Existing Files to Modify

### 1. Scheme Registration (`url/url_util.cc`, `chrome/common/url_constants.cc/h`)
- Add `constexpr char kRspScheme[] = "rsp";` to `url_constants.h`
- Call `url::AddStandardScheme(kRspScheme, url::SCHEME_WITH_HOST)` in `ChromeMainDelegate::PreSandboxStartup()` (`chrome/app/chrome_main_delegate.cc`) — must happen before sandbox lockdown
- Register in `ChildProcessSecurityPolicyImpl` as a safe, web-accessible scheme (`content/browser/child_process_security_policy_impl.cc`)
- Add to the allowed list in `chrome/browser/chrome_content_browser_client.cc` (`GetAdditionalAllowedSchemesForFileSystem` etc.)

**Note on URI syntax**: `rsp:\\<name>` uses backslash (Windows UNC style) but URI standard uses `://`. Implementation will use `rsp://<uuid-or-name>` (e.g., `rsp://linux-sshd` or `rsp://c3d4e5f6-a7b8-4c9d-8e0f-1a2b3c4d5e6f`). The address bar can accept either and normalize.

### 2. Profile OTR IDs (`chrome/browser/profiles/profile.cc/h`)
```cpp
// In profile.cc — alongside kDevToolsOTRProfileIDPrefix etc.
const char kRspTabOTRProfileIDPrefix[] = "RSP::Tab";
const char kRspIncognitoTabOTRProfileIDPrefix[] = "RSP::IncognitoTab";

// New factory methods in OTRProfileID:
static OTRProfileID CreateUniqueForRspTab();        // uses kRspTabOTRProfileIDPrefix
static OTRProfileID CreateUniqueForRspIncognito();  // uses kRspIncognitoTabOTRProfileIDPrefix

// AllowsBrowserWindows() must return true for both RSP prefix types
```

### 3. Network interception (`chrome/browser/chrome_content_browser_client.cc`)
In `WillCreateURLLoaderFactory()` (line 6502):
- Check if the `BrowserContext` has an RSP OTR profile ID
- If yes, wrap the default factory with `RspURLLoaderFactory` for that profile's RSP config
- `RspURLLoaderFactory` intercepts `http://` and `https://` requests, establishes TCP via RSP bsd_socket RS, forwards the raw HTTP stream

Also override `RegisterNonNetworkNavigationURLLoaderFactories()` to register `RspSchemeURLLoaderFactory` for the `rsp://` scheme.

### 4. New Tab button menu (`chrome/browser/ui/views/tabs/new_tab_button.cc/h`)
Add to `NewTabButtonMenuModel`:
```cpp
DEFINE_CLASS_ELEMENT_IDENTIFIER_VALUE(NewTabButtonMenuModel, kNewRspTab);
DEFINE_CLASS_ELEMENT_IDENTIFIER_VALUE(NewTabButtonMenuModel, kNewIncognitoRspTab);
```
In `ExecuteCommand()`, handle `IDC_NEW_RSP_TAB` and `IDC_NEW_INCOGNITO_RSP_TAB`:
- Open `RspTabConfigDialog` modal
- On confirm, call `chrome::NewRspTab(browser, config)` or `chrome::NewIncognitoRspTab(browser, config)`

### 5. Command IDs (`chrome/app/chrome_command_ids.h`)
```cpp
#define IDC_NEW_RSP_TAB                    34060
#define IDC_NEW_INCOGNITO_RSP_TAB          34061
#define IDC_CONTENT_CONTEXT_OPENLINKRSPTAB 34062
```

### 6. Browser commands (`chrome/browser/ui/browser_commands.cc/h`)
```cpp
void NewRspTab(Browser* browser, const RspTabConfig& config);
void NewIncognitoRspTab(Browser* browser, const RspTabConfig& config);
```
These:
1. Call `RspConnectionManager::GetOrCreate(config)` to get/create the shared RSPClient
2. Create an OTR profile via `profile->GetOffTheRecordProfile(OTRProfileID::CreateUniqueForRspTab(), true)`
3. Attach `RspKeyedService` to the profile with the config
4. Open a new tab in that profile via `NavigateParams`

### 7. Context menu (`chrome/browser/renderer_context_menu/render_view_context_menu.cc`)
Around line 1791 (where `show_open_in_new_tab` is evaluated):
- Check if source profile is an RSP profile (check OTRProfileID prefix)
- If yes, add `IDC_CONTENT_CONTEXT_OPENLINKRSPTAB` item with label "Open Link in New RSP Tab"
- Handler at line 3309: open new RSP tab inheriting parent's `RspTabConfig`

### 8. Tab visual indicator (`chrome/browser/ui/views/tabs/tab.cc`)
- In `Tab::GetGroupColor()` or the tab paint path, detect RSP profiles
- Show an RSP logo badge (similar to how incognito shows the spy icon)
- RSP tab: blue shield badge; Incognito RSP tab: grey shield badge

### 9. Link propagation in `Browser::OpenURLFromTab` (`chrome/browser/ui/browser.cc` line 1934)
When a navigation opens in a new tab from an RSP tab:
- Detect source profile is RSP
- Propagate `WindowOpenDisposition` to use the same RSP OTR profile

---

## Implementation Detail: `RspURLLoaderFactory`

```
class RspURLLoaderFactory : public network::mojom::URLLoaderFactory {
  // Holds: shared RSPClient*, endorsement keypair, profile weak ptr
  // For each CreateLoaderAndStart():
  //   1. Check scheme is http or https
  //   2. Open TCP stream to target host:port via RSP bsd_socket RS
  //      (RSP ResourceRequest with host:port as parameters)
  //   3. Send raw HTTP request bytes over the stream
  //   4. Receive response bytes, parse headers, feed to URLLoaderClient
  //   5. Stream body bytes to URLLoaderClient::OnReceiveData
};
```

The bsd_socket RS exposes a `connect(host, port)` → stream interface. Each HTTP request gets one RSP stream. HTTP/1.1 keep-alive can reuse streams if the RS supports it.

## Implementation Detail: `RspSchemeURLLoaderFactory`

```
class RspSchemeURLLoaderFactory : public network::mojom::URLLoaderFactory {
  // For rsp://<identifier>/path?query:
  //   1. Extract <identifier> (UUID or name string)
  //   2. If name: use RspConnectionManager's default RSPClient
  //              call nameQuery(name) → node ID
  //   3. Connect to http_server RS at node ID
  //   4. Issue HTTP GET / with Host: <identifier>
  //   5. Return response to renderer
};
```

## Implementation Detail: `RspConnectionManager`

```
class RspConnectionManager {  // Lives on browser process, accessed via g_browser_process
  struct Key { std::string rm_addr; NodeId rs_node_id; };
  std::map<Key, std::shared_ptr<RSPClient>> connections_;
  std::map<Key, int> ref_counts_;

  std::shared_ptr<RSPClient> GetOrCreate(const RspTabConfig&);
  void Release(const Key&);  // called when RSP tab closes; disconnects if ref=0
};
```

## Implementation Detail: `RspTabConfigDialog`

Fields:
- **RM Address** (host:port text field, e.g. `localhost:8080`)
- **Endorsement** — dropdown of available endorsement keys from keychain/profile
- **Resource Service** — populated by calling `resourceList()` on the RM after address entry, filtered to `bsd_sockets` type; shows friendly names if NS is available
- **[Connect]** button — tests the connection before confirming

---

## Implementation Sequence (recommended order)

### Phase 1 — Foundation
1. Register `rsp://` scheme in `url_util`, `url_constants`, `chrome_main_delegate`
2. Add `OTRProfileID::CreateUniqueForRspTab/Incognito` + `AllowsBrowserWindows` fix
3. Implement `RspConnectionManager` and `RspKeyedService` stubs
4. Add IDC command IDs

### Phase 2 — Network Layer
5. Implement `RspURLLoaderFactory` (http/https traffic through bsd_socket RS)
6. Wire into `WillCreateURLLoaderFactory()` in `ChromeContentBrowserClient`
7. Implement `RspSchemeURLLoaderFactory` (rsp:// scheme)
8. Wire into `RegisterNonNetworkNavigationURLLoaderFactories()`

### Phase 3 — UI
9. Implement `RspTabConfigDialog`
10. Add RSP items to `NewTabButtonMenuModel`
11. Add `IDC_NEW_RSP_TAB` / `IDC_NEW_INCOGNITO_RSP_TAB` to `browser_commands.cc`
12. Add RSP tab badge to `tab.cc`

### Phase 4 — Context menu & link propagation
13. Add `IDC_CONTENT_CONTEXT_OPENLINKRSPTAB` to context menu
14. Propagate RSP config through `OpenURLFromTab`

### Phase 5 — Settings & default RM
15. `chrome://settings/rsp` WebUI for default RM address
16. Default `RspConnectionManager` entry from prefs
17. `RspSchemeURLLoaderFactory` wired to default RM

---

## Additional Findings from Source Exploration

### Profile Subclassing Strategy (refined)
The explore agent confirmed two viable approaches:

**Option A (recommended): Use OTRProfileID + OffTheRecordProfileImpl**
- `Profile::OTRProfileID::CreateUnique("RSP::Tab")` creates an OTR profile backed by `OffTheRecordProfileImpl`
- Intercept network in `WillCreateURLLoaderFactory()` by inspecting the profile's OTRProfileID
- Less invasive — no new Profile subclass needed
- Used by DevTools, MediaRouter, CaptivePortal already

**Option B: Full Profile subclass `RSPProfile : public Profile`**
- Wraps a parent profile, overrides `GetPath()`, `GetPrefs()`, network context methods
- More control but requires implementing/delegating ~20 virtual methods
- Better long-term if RSP needs deep network context customization beyond `WillCreateURLLoaderFactory`

**Recommendation**: Start with Option A for the network layer. If `WillCreateURLLoaderFactory()` proves insufficient for bsd_socket RS TCP streaming (e.g., can't stream large bodies, WebSocket issues), upgrade to Option B.

### Browser::Type enum
There is a `Browser::Type` enum (`TYPE_NORMAL`, `TYPE_POPUP`, etc.). Adding `TYPE_RSP` is possible but likely unnecessary — RSP tabs live in a normal browser window and the isolation is at the Profile/network level, not the Browser level. **Do not add TYPE_RSP.**

### ChildProcessSecurityPolicy scheme registration
Confirmed call: `ChildProcessSecurityPolicyImpl::GetInstance()->RegisterSafeScheme("rsp")` — this allows the renderer to access `rsp://` URLs without a security error.

### Context menu confirmed location
`IDC_CONTENT_CONTEXT_OPENLINKNEWTAB` handler is at `render_view_context_menu.cc:3309`. The `GetProfile()` method is available at that point to check for RSP OTR profile ID prefix.

---

## Open Questions / Design Decisions

1. **RSP TCP stream multiplexing**: Does the bsd_socket RS support multiple concurrent TCP streams over one RSP connection, or is a new RSP connection needed per HTTP request? If one-per-request, `RspConnectionManager` may need a pool.

2. **HTTPS**: HTTPS requires TLS termination. Two options:
   - Terminate TLS inside Chromium (pass raw TLS to the RS): transparent, RS is a dumb pipe
   - Have the http_server RS handle TLS: requires RSP-level TLS cert management
   The first option (Chromium handles TLS, RS is a TCP pipe) is simpler and consistent with how bsd_sockets already works.

3. **WebSocket over RSP**: Should RSP tabs also route WebSocket connections? Likely yes — `WillCreateURLLoaderFactory` covers this if using the network service WebSocket path.

4. **RSP tab persistence**: Should RSP tab configs be saved across browser restarts? Probably not for the first version (same as incognito — ephemeral).

5. **URI syntax**: `rsp:\\name` → normalize to `rsp://name` in the address bar omnibox handler.

6. **Name resolution caching**: Should the `rsp://` scheme cache name→node ID mappings? Suggest: cache with TTL matching NS record TTL.
