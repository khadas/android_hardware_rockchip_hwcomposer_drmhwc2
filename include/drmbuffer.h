/*
 * Copyright (C) 2016 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#ifndef _DRM_BUFFER_H_
#define _DRM_BUFFER_H_

#include "rockchip/drmgralloc.h"
#include "utils/autofd.h"

#include <ui/GraphicBuffer.h>

namespace android {

class DrmBuffer{
public:
  DrmBuffer(int w, int h, int format, uint64_t usage = 0, std::string sName_ = "unset", int parent_id = 0);
  ~DrmBuffer();
  int Init();
  bool initCheck();
  buffer_handle_t GetHandle();
  uint64_t GetId();
  int GetParentId();
  int SetParentId(int parent_id);
  int GetFd();
  std::string GetName();
  int GetWidth();
  int GetHeight();
  int GetFormat();
  int GetStride();
  int GetByteStride();
  int GetSize();
  uint64_t GetUsage();
  int SetCrop(int left, int top, int right, int bottom);
  int GetCrop(int *left, int *top, int *right, int *bottom);
  uint32_t GetFourccFormat();
  uint64_t GetModifier();
  uint64_t GetBufferId();
  uint32_t GetGemHandle();
  uint32_t DrmFormatToPlaneNum(uint32_t drm_format);
  uint32_t GetFbId();
  void* Lock();
  int Unlock();
  int GetFinishFence();
  int SetFinishFence(int fence);
  int WaitFinishFence();
  int GetReleaseFence();
  int SetReleaseFence(int fence);
  int WaitReleaseFence();
  int DumpData();

private:
  uint64_t uId;
  int iParentId_;
  // BufferInfo
  int iFd_;
  int iWidth_;
  int iHeight_;
  int iFormat_;
  int iStride_;
  int iByteStride_;
  int iSize_;
  uint64_t iUsage_;
  uint32_t uFourccFormat_;
  uint64_t uModifier_;
  uint64_t uBufferId_;
  uint32_t uGemHandle_;
  uint32_t uFbId_;

  // rect crop info
  int iLeft_;
  int iTop_;
  int iRight_;
  int iBottom_;

  // Fence info
  UniqueFd iFinishFence_;
  UniqueFd iReleaseFence_;

  // Init flags
  bool bInit_;
  std::string sName_;
  buffer_handle_t buffer_;
  sp<GraphicBuffer> ptrBuffer_;
  DrmGralloc *ptrDrmGralloc_;
};

}// namespace android
#endif // #ifndef _DRM_BUFFER_QUEUE_H_
