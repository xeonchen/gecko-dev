/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* vim: set ts=8 sts=2 et sw=2 tw=80: */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "nsSocketMarkerService.h"

namespace mozilla {
namespace net {

NS_IMPL_ISUPPORTS(nsSocketMarkerService, nsISocketMarkerService)

nsSocketMarkerService::nsSocketMarkerService()
  : mLock("nsSocketMarkerService.mLock")
{
}

nsSocketMarkerService::~nsSocketMarkerService()
{
}

NS_IMETHODIMP
nsSocketMarkerService::SetMarkEnabled(uint32_t aAppId, bool aEnabled)
{
  MutexAutoLock lock(mLock);

  if (!aEnabled) {
      mMarkEnabledAppIds.RemoveElement(aAppId);
  } else if (!mMarkEnabledAppIds.Contains(aAppId)) {
      mMarkEnabledAppIds.AppendElement(aAppId);
  }
  return NS_OK;
}

NS_IMETHODIMP
nsSocketMarkerService::GetMarkEnabled(uint32_t aAppId, bool* _retval)
{
  MutexAutoLock lock(mLock);

  // DEBUG Begin
  if (aAppId & 0x01) {
    *_retval = true;
    return NS_OK;
  }
  // DEBUG End

  *_retval = mMarkEnabledAppIds.Contains(aAppId);
  return NS_OK;
}

} // namespace net
} // namespace mozilla
