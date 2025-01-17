/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* vim: set ts=8 sts=2 et sw=2 tw=80: */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#define INITGUID

#include "mozilla/a11y/HandlerProvider.h"

#include "Accessible2_3.h"
#include "AccessibleDocument.h"
#include "AccessibleTable.h"
#include "AccessibleTable2.h"
#include "AccessibleTableCell.h"
#include "HandlerData.h"
#include "HandlerData_i.c"
#include "mozilla/Assertions.h"
#include "mozilla/a11y/AccessibleWrap.h"
#include "mozilla/dom/ContentChild.h"
#include "mozilla/Move.h"
#include "mozilla/mscom/AgileReference.h"
#include "mozilla/mscom/FastMarshaler.h"
#include "mozilla/mscom/Interceptor.h"
#include "mozilla/mscom/MainThreadInvoker.h"
#include "mozilla/mscom/Ptr.h"
#include "mozilla/mscom/StructStream.h"
#include "mozilla/mscom/Utils.h"
#include "nsThreadUtils.h"

#include <memory.h>

namespace mozilla {
namespace a11y {

HandlerProvider::HandlerProvider(REFIID aIid,
                               mscom::InterceptorTargetPtr<IUnknown> aTarget)
  : mRefCnt(0)
  , mMutex("mozilla::a11y::HandlerProvider::mMutex")
  , mTargetUnkIid(aIid)
  , mTargetUnk(Move(aTarget))
{
}

HRESULT
HandlerProvider::QueryInterface(REFIID riid, void** ppv)
{
  if (!ppv) {
    return E_INVALIDARG;
  }

  if (riid == IID_IUnknown || riid == IID_IGeckoBackChannel) {
    RefPtr<IUnknown> punk(static_cast<IGeckoBackChannel*>(this));
    punk.forget(ppv);
    return S_OK;
  }

  if (riid == IID_IMarshal) {
    if (!mFastMarshalUnk) {
      HRESULT hr = mscom::FastMarshaler::Create(
        static_cast<IGeckoBackChannel*>(this), getter_AddRefs(mFastMarshalUnk));
      if (FAILED(hr)) {
        return hr;
      }
    }

    return mFastMarshalUnk->QueryInterface(riid, ppv);
  }

  return E_NOINTERFACE;
}

ULONG
HandlerProvider::AddRef()
{
  return ++mRefCnt;
}

ULONG
HandlerProvider::Release()
{
  ULONG result = --mRefCnt;
  if (!result) {
    delete this;
  }
  return result;
}

HRESULT
HandlerProvider::GetHandler(NotNull<CLSID*> aHandlerClsid)
{
  if (!IsTargetInterfaceCacheable()) {
    return E_NOINTERFACE;
  }

  *aHandlerClsid = CLSID_AccessibleHandler;
  return S_OK;
}

void
HandlerProvider::GetAndSerializePayload(const MutexAutoLock&,
    NotNull<mscom::IInterceptor*> aInterceptor)
{
  MOZ_ASSERT(mscom::IsCurrentThreadMTA());

  if (mSerializer) {
    return;
  }

  IA2Payload payload{};

  if (!mscom::InvokeOnMainThread("HandlerProvider::BuildInitialIA2Data",
                                 this, &HandlerProvider::BuildInitialIA2Data,
                                 aInterceptor,
                                 &payload.mStaticData, &payload.mDynamicData) ||
      !payload.mDynamicData.mUniqueId) {
    return;
  }

  // But we set mGeckoBackChannel on the current thread which resides in the
  // MTA. This is important to ensure that COM always invokes
  // IGeckoBackChannel methods in an MTA background thread.

  RefPtr<IGeckoBackChannel> payloadRef(this);
  // AddRef/Release pair for this reference is handled by payloadRef
  payload.mGeckoBackChannel = this;

  mSerializer = MakeUnique<mscom::StructToStream>(payload, &IA2Payload_Encode);

  // Now that we have serialized payload, we should clean up any
  // BSTRs, interfaces, etc. fetched in BuildInitialIA2Data.
  CleanupStaticIA2Data(payload.mStaticData);
  CleanupDynamicIA2Data(payload.mDynamicData);
}

HRESULT
HandlerProvider::GetHandlerPayloadSize(NotNull<mscom::IInterceptor*> aInterceptor,
                                       NotNull<DWORD*> aOutPayloadSize)
{
  MOZ_ASSERT(mscom::IsCurrentThreadMTA());

  if (!IsTargetInterfaceCacheable()) {
    *aOutPayloadSize = mscom::StructToStream::GetEmptySize();
    return S_OK;
  }

  MutexAutoLock lock(mMutex);

  GetAndSerializePayload(lock, aInterceptor);

  if (!mSerializer || !(*mSerializer)) {
    // Failed payload serialization is non-fatal
    *aOutPayloadSize = mscom::StructToStream::GetEmptySize();
    return S_OK;
  }

  *aOutPayloadSize = mSerializer->GetSize();
  return S_OK;
}

template <typename CondFnT, typename ExeFnT>
class MOZ_RAII ExecuteWhen final
{
public:
  ExecuteWhen(CondFnT& aCondFn, ExeFnT& aExeFn)
    : mCondFn(aCondFn)
    , mExeFn(aExeFn)
  {
  }

  ~ExecuteWhen()
  {
    if (mCondFn()) {
      mExeFn();
    }
  }

  ExecuteWhen(const ExecuteWhen&) = delete;
  ExecuteWhen(ExecuteWhen&&) = delete;
  ExecuteWhen& operator=(const ExecuteWhen&) = delete;
  ExecuteWhen& operator=(ExecuteWhen&&) = delete;

private:
  CondFnT&  mCondFn;
  ExeFnT&   mExeFn;
};

void
HandlerProvider::BuildStaticIA2Data(
  NotNull<mscom::IInterceptor*> aInterceptor,
  StaticIA2Data* aOutData)
{
  MOZ_ASSERT(aOutData);
  MOZ_ASSERT(NS_IsMainThread());
  MOZ_ASSERT(mTargetUnk);
  MOZ_ASSERT(IsTargetInterfaceCacheable());

  // Include interfaces the client is likely to request.
  // This is cheap here and saves multiple cross-process calls later.
  // These interfaces must be released in CleanupStaticIA2Data!

  // If the target is already an IAccessible2, this pointer is redundant.
  // However, the target might be an IAccessibleHyperlink, etc., in which
  // case the client will almost certainly QI for IAccessible2.
  HRESULT hr = aInterceptor->GetInterceptorForIID(NEWEST_IA2_IID,
                                          (void**)&aOutData->mIA2);
  if (FAILED(hr)) {
    // IA2 should always be present, so something has
    // gone very wrong if this fails.
    aOutData->mIA2 = nullptr;
    return;
  }

  // Some of these interfaces aren't present on all accessibles,
  // so it's not a failure if these interfaces can't be fetched.
  hr = aInterceptor->GetInterceptorForIID(IID_IEnumVARIANT,
                                          (void**)&aOutData->mIEnumVARIANT);
  if (FAILED(hr)) {
    aOutData->mIEnumVARIANT = nullptr;
  }

  hr = aInterceptor->GetInterceptorForIID(IID_IAccessibleHypertext2,
                                          (void**)&aOutData->mIAHypertext);
  if (FAILED(hr)) {
    aOutData->mIAHypertext = nullptr;
  }

  hr = aInterceptor->GetInterceptorForIID(IID_IAccessibleHyperlink,
                                          (void**)&aOutData->mIAHyperlink);
  if (FAILED(hr)) {
    aOutData->mIAHyperlink = nullptr;
  }

  hr = aInterceptor->GetInterceptorForIID(IID_IAccessibleTable,
                                          (void**)&aOutData->mIATable);
  if (FAILED(hr)) {
    aOutData->mIATable = nullptr;
  }

  hr = aInterceptor->GetInterceptorForIID(IID_IAccessibleTable2,
                                          (void**)&aOutData->mIATable2);
  if (FAILED(hr)) {
    aOutData->mIATable2 = nullptr;
  }

  hr = aInterceptor->GetInterceptorForIID(IID_IAccessibleTableCell,
                                          (void**)&aOutData->mIATableCell);
  if (FAILED(hr)) {
    aOutData->mIATableCell = nullptr;
  }
}

void
HandlerProvider::BuildDynamicIA2Data(DynamicIA2Data* aOutIA2Data)
{
  MOZ_ASSERT(aOutIA2Data);
  MOZ_ASSERT(NS_IsMainThread());
  MOZ_ASSERT(mTargetUnk);
  MOZ_ASSERT(IsTargetInterfaceCacheable());

  RefPtr<NEWEST_IA2_INTERFACE> target;
  HRESULT hr = mTargetUnk.get()->QueryInterface(NEWEST_IA2_IID,
    getter_AddRefs(target));
  if (FAILED(hr)) {
    return;
  }

  hr = E_UNEXPECTED;

  auto hasFailed = [&hr]() -> bool {
    return FAILED(hr);
  };

  auto cleanup = [this, aOutIA2Data]() -> void {
    CleanupDynamicIA2Data(*aOutIA2Data);
  };

  ExecuteWhen<decltype(hasFailed), decltype(cleanup)> onFail(hasFailed, cleanup);

  const VARIANT kChildIdSelf = {VT_I4};
  VARIANT varVal;

  hr = target->accLocation(&aOutIA2Data->mLeft, &aOutIA2Data->mTop,
                           &aOutIA2Data->mWidth, &aOutIA2Data->mHeight,
                           kChildIdSelf);
  if (FAILED(hr)) {
    return;
  }

  hr = target->get_accRole(kChildIdSelf, &aOutIA2Data->mRole);
  if (FAILED(hr)) {
    return;
  }

  hr = target->get_accState(kChildIdSelf, &varVal);
  if (FAILED(hr)) {
    return;
  }

  aOutIA2Data->mState = varVal.lVal;

  hr = target->get_accKeyboardShortcut(kChildIdSelf,
                                       &aOutIA2Data->mKeyboardShortcut);
  if (FAILED(hr)) {
    return;
  }

  hr = target->get_accName(kChildIdSelf, &aOutIA2Data->mName);
  if (FAILED(hr)) {
    return;
  }

  hr = target->get_accDescription(kChildIdSelf, &aOutIA2Data->mDescription);
  if (FAILED(hr)) {
    return;
  }

  hr = target->get_accDefaultAction(kChildIdSelf, &aOutIA2Data->mDefaultAction);
  if (FAILED(hr)) {
    return;
  }

  hr = target->get_accChildCount(&aOutIA2Data->mChildCount);
  if (FAILED(hr)) {
    return;
  }

  hr = target->get_accValue(kChildIdSelf, &aOutIA2Data->mValue);
  if (FAILED(hr)) {
    return;
  }

  hr = target->get_states(&aOutIA2Data->mIA2States);
  if (FAILED(hr)) {
    return;
  }

  hr = target->get_attributes(&aOutIA2Data->mAttributes);
  if (FAILED(hr)) {
    return;
  }

  HWND hwnd;
  hr = target->get_windowHandle(&hwnd);
  if (FAILED(hr)) {
    return;
  }

  aOutIA2Data->mHwnd = PtrToLong(hwnd);

  hr = target->get_locale(&aOutIA2Data->mIA2Locale);
  if (FAILED(hr)) {
    return;
  }

  hr = target->role(&aOutIA2Data->mIA2Role);
  if (FAILED(hr)) {
    return;
  }

  RefPtr<IAccessibleAction> action;
  // It is not an error if this fails.
  hr = mTargetUnk.get()->QueryInterface(IID_IAccessibleAction,
    getter_AddRefs(action));
  if (SUCCEEDED(hr)) {
    hr = action->nActions(&aOutIA2Data->mNActions);
    if (FAILED(hr)) {
      return;
    }
  }

  RefPtr<IAccessibleTableCell> cell;
  // It is not an error if this fails.
  hr = mTargetUnk.get()->QueryInterface(IID_IAccessibleTableCell,
    getter_AddRefs(cell));
  if (SUCCEEDED(hr)) {
    hr = cell->get_rowColumnExtents(&aOutIA2Data->mRowIndex,
                                    &aOutIA2Data->mColumnIndex,
                                    &aOutIA2Data->mRowExtent,
                                    &aOutIA2Data->mColumnExtent,
                                    &aOutIA2Data->mCellIsSelected);
    if (FAILED(hr)) {
      return;
    }
  }

  // NB: get_uniqueID should be the final property retrieved in this method,
  // as its presence is used to determine whether the rest of this data
  // retrieval was successful.
  hr = target->get_uniqueID(&aOutIA2Data->mUniqueId);
}

void
HandlerProvider::CleanupStaticIA2Data(StaticIA2Data& aData)
{
  // When CoMarshalInterface writes interfaces out to a stream, it AddRefs.
  // Therefore, we must release our references after this.
  if (aData.mIA2) {
    aData.mIA2->Release();
  }
  if (aData.mIEnumVARIANT) {
    aData.mIEnumVARIANT->Release();
  }
  if (aData.mIAHypertext) {
    aData.mIAHypertext->Release();
  }
  if (aData.mIAHyperlink) {
    aData.mIAHyperlink->Release();
  }
  if (aData.mIATable) {
    aData.mIATable->Release();
  }
  if (aData.mIATable2) {
    aData.mIATable2->Release();
  }
  if (aData.mIATableCell) {
    aData.mIATableCell->Release();
  }
  ZeroMemory(&aData, sizeof(StaticIA2Data));
}

void
HandlerProvider::CleanupDynamicIA2Data(DynamicIA2Data& aData)
{
  ::VariantClear(&aData.mRole);
  ZeroMemory(&aData, sizeof(DynamicIA2Data));
}

void
HandlerProvider::BuildInitialIA2Data(
  NotNull<mscom::IInterceptor*> aInterceptor,
  StaticIA2Data* aOutStaticData,
  DynamicIA2Data* aOutDynamicData)
{
  BuildStaticIA2Data(aInterceptor, aOutStaticData);
  if (!aOutStaticData->mIA2) {
    return;
  }
  BuildDynamicIA2Data(aOutDynamicData);
  if (!aOutDynamicData->mUniqueId) {
    // Building dynamic data failed, which means building the payload failed.
    // However, we've already built static data, so we must clean this up.
    CleanupStaticIA2Data(*aOutStaticData);
  }
}

bool
HandlerProvider::IsTargetInterfaceCacheable()
{
  return MarshalAs(mTargetUnkIid) == NEWEST_IA2_IID ||
         mTargetUnkIid == IID_IAccessibleHyperlink;
}

HRESULT
HandlerProvider::WriteHandlerPayload(NotNull<mscom::IInterceptor*> aInterceptor,
                                     NotNull<IStream*> aStream)
{
  MutexAutoLock lock(mMutex);

  if (!mSerializer || !(*mSerializer)) {
    // Failed payload serialization is non-fatal
    mscom::StructToStream emptyStruct;
    return emptyStruct.Write(aStream);
  }

  HRESULT hr = mSerializer->Write(aStream);

  mSerializer.reset();

  return hr;
}

REFIID
HandlerProvider::MarshalAs(REFIID aIid)
{
  static_assert(&NEWEST_IA2_IID == &IID_IAccessible2_3,
                "You have modified NEWEST_IA2_IID. This code needs updating.");
  if (aIid == IID_IDispatch || aIid == IID_IAccessible ||
      aIid == IID_IAccessible2 || aIid == IID_IAccessible2_2 ||
      aIid == IID_IAccessible2_3) {
    // This should always be the newest IA2 interface ID
    return NEWEST_IA2_IID;
  }
  // Otherwise we juse return the identity.
  return aIid;
}

REFIID
HandlerProvider::GetEffectiveOutParamIid(REFIID aCallIid,
                                         ULONG aCallMethod)
{
  if (aCallIid == IID_IAccessibleTable ||
      aCallIid == IID_IAccessibleTable2 ||
      aCallIid == IID_IAccessibleDocument ||
      aCallIid == IID_IAccessibleTableCell ||
      aCallIid == IID_IAccessibleRelation) {
    return NEWEST_IA2_IID;
  }

  // IAccessible2_2::accessibleWithCaret
  static_assert(&NEWEST_IA2_IID == &IID_IAccessible2_3,
                "You have modified NEWEST_IA2_IID. This code needs updating.");
  if ((aCallIid == IID_IAccessible2_2 || aCallIid == IID_IAccessible2_3) &&
      aCallMethod == 47) {
    return NEWEST_IA2_IID;
  }

  MOZ_ASSERT(false);
  return IID_IUnknown;
}

HRESULT
HandlerProvider::NewInstance(REFIID aIid,
                             mscom::InterceptorTargetPtr<IUnknown> aTarget,
                             NotNull<mscom::IHandlerProvider**> aOutNewPayload)
{
  RefPtr<IHandlerProvider> newPayload(new HandlerProvider(aIid, Move(aTarget)));
  newPayload.forget(aOutNewPayload.get());
  return S_OK;
}

void
HandlerProvider::SetHandlerControlOnMainThread(DWORD aPid,
                                              mscom::ProxyUniquePtr<IHandlerControl> aCtrl)
{
  MOZ_ASSERT(NS_IsMainThread());

  auto content = dom::ContentChild::GetSingleton();
  MOZ_ASSERT(content);

  IHandlerControlHolder holder(CreateHolderFromHandlerControl(Move(aCtrl)));
  Unused << content->SendA11yHandlerControl(aPid, holder);
}

HRESULT
HandlerProvider::put_HandlerControl(long aPid, IHandlerControl* aCtrl)
{
  MOZ_ASSERT(mscom::IsCurrentThreadMTA());

  if (!aCtrl) {
    return E_INVALIDARG;
  }

  auto ptrProxy = mscom::ToProxyUniquePtr(aCtrl);

  if (!mscom::InvokeOnMainThread("HandlerProvider::SetHandlerControlOnMainThread",
                                 this,
                                 &HandlerProvider::SetHandlerControlOnMainThread,
                                 static_cast<DWORD>(aPid), Move(ptrProxy))) {
    return E_FAIL;
  }

  return S_OK;
}

HRESULT
HandlerProvider::Refresh(DynamicIA2Data* aOutData)
{
  MOZ_ASSERT(mscom::IsCurrentThreadMTA());

  if (!mscom::InvokeOnMainThread("HandlerProvider::BuildDynamicIA2Data",
                                 this, &HandlerProvider::BuildDynamicIA2Data,
                                 aOutData)) {
    return E_FAIL;
  }

  return S_OK;
}

} // namespace a11y
} // namespace mozilla

