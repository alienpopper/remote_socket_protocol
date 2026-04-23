// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef CHROME_BROWSER_RSP_RSP_PREFS_H_
#define CHROME_BROWSER_RSP_RSP_PREFS_H_

namespace user_prefs {
class PrefRegistrySyncable;
}

namespace rsp {
namespace prefs {

// Address of the default Resource Manager, e.g. "localhost:8080".
// Empty string means no default RM is configured.
inline constexpr char kRspDefaultRmAddr[] = "rsp.default_rm_addr";

// Node ID of the default bsd_sockets Resource Service.
// Empty string means no default RS is configured.
inline constexpr char kRspDefaultRsNodeId[] = "rsp.default_rs_node_id";

void RegisterProfilePrefs(user_prefs::PrefRegistrySyncable* registry);

}  // namespace prefs
}  // namespace rsp

#endif  // CHROME_BROWSER_RSP_RSP_PREFS_H_
