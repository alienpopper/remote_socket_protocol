// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "chrome/browser/rsp/rsp_prefs.h"

#include "components/pref_registry/pref_registry_syncable.h"

namespace rsp {
namespace prefs {

void RegisterProfilePrefs(user_prefs::PrefRegistrySyncable* registry) {
  registry->RegisterStringPref(kRspDefaultRmAddr, /*default_value=*/"");
  registry->RegisterStringPref(kRspDefaultRsNodeId, /*default_value=*/"");
}

}  // namespace prefs
}  // namespace rsp
