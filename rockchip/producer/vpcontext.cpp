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

#define ATRACE_TAG ATRACE_TAG_GRAPHICS
#define LOG_TAG "VpContext"

#include "rockchip/utils/drmdebug.h"
#include "rockchip/producer/vpcontext.h"
#include "utils/drmfence.h"

#include <inttypes.h>

namespace android {

VpContext::VpContext(int tunnel_fd) :
  mDrmGralloc_(DrmGralloc::getInstance()),
  iTunnelId_(tunnel_fd),
  uFrameNo_(0){}

VpContext::~VpContext(){
  std::lock_guard<std::mutex> lock(mtx_);
}

int VpContext::GetTunnelId(){
  return iTunnelId_;
}

// Get Buffer cache
std::shared_ptr<DrmBuffer> VpContext::GetBufferCache(vt_buffer_t* vp_buffer){
  std::lock_guard<std::mutex> lock(mtx_);
  std::shared_ptr<DrmBuffer> out_buffer = NULL;

  // 利用BufferId作为键值获取CacheBuffer
  uint64_t buffer_id = vp_buffer->buffer_id;
  native_handle_t *handle = vp_buffer->handle;
  // Cache 命中
  if(mMapBuffer_.count(buffer_id)){
    mMapBuffer_[buffer_id]->SetVpBuffer(vp_buffer);
    out_buffer = mMapBuffer_[buffer_id]->GetDrmBuffer();
    out_buffer->SetCrop(vp_buffer->crop.left,
                        vp_buffer->crop.top,
                        vp_buffer->crop.right,
                        vp_buffer->crop.bottom);
    HWC2_ALOGD_IF_DEBUG("Get cache buffer-id=0x%" PRIx64" crop=[%d,%d,%d,%d]",
                         out_buffer->GetBufferId(),
                         vp_buffer->crop.left,
                         vp_buffer->crop.top,
                         vp_buffer->crop.right,
                         vp_buffer->crop.bottom);
  }else{ // Cache 未命中，则说明是新Buffer,需要执行Import操作
    std::shared_ptr<DrmBuffer> drm_buffer = std::make_shared<DrmBuffer>(handle);
    if(!drm_buffer->initCheck()){
      HWC2_ALOGI("DrmBuffer import fail, handle=%p",
              handle);
      return NULL;
    }
    drm_buffer->SetCrop(vp_buffer->crop.left,
                        vp_buffer->crop.top,
                        vp_buffer->crop.right,
                        vp_buffer->crop.bottom);
    // 保存外部唯一键值的ExternelId, 作为SidebandStream唯一键值
    drm_buffer->SetExternalId(vp_buffer->buffer_id);
    auto vp_buffer_info = std::make_shared<VpBufferInfo>(vp_buffer, drm_buffer);
    mMapBuffer_[buffer_id] = vp_buffer_info;
    out_buffer = vp_buffer_info->GetDrmBuffer();
    HWC2_ALOGD_IF_DEBUG("Get new cache buffer-id=0x%" PRIx64 " vp_buffer->buffer_id=0x%" PRIx64 ,
                        out_buffer->GetBufferId(), vp_buffer->buffer_id);
  }
  return out_buffer;
}


// Get Buffer cache
vt_buffer_t* VpContext::GetVpBufferInfo(uint64_t buffer_id){
  std::lock_guard<std::mutex> lock(mtx_);
  // 获取 BufferId
  vt_buffer_t* out_buffer = NULL;
  if(mMapBuffer_.count(buffer_id)){
    out_buffer = mMapBuffer_[buffer_id]->GetVpBuffer();
  }
  return out_buffer;
}

// Add ReleaseFence
int VpContext::AddReleaseFence(uint64_t buffer_id){
  std::lock_guard<std::mutex> lock(mtx_);

  if(!mTimeLine_.isValid()){
    HWC2_ALOGE("mTimeLine_ is invalid, buffer-id=%" PRIu64 " TunnelId=%d",
               buffer_id, iTunnelId_);
    return -1;
  }

  // 获取 BufferId
  if(mMapBuffer_.count(buffer_id)){
    char acBuf[32];
    uFrameNo_ = mTimeLine_.IncTimeline();
    sprintf(acBuf,"RFVP-ID%d-B%" PRIu64 "-FN%" PRIu64, iTunnelId_, buffer_id, uFrameNo_);
    sp<ReleaseFence> release_fence = sp<ReleaseFence>(new ReleaseFence(mTimeLine_, uFrameNo_, acBuf));
    auto &buffer_info = mMapBuffer_[buffer_id];
    buffer_info->SetReleaseFence(release_fence);
    HWC2_ALOGD_IF_INFO("Create ReleaseFence Name=%s uFrameNo_=%" PRIu64 ,
                    release_fence->getName().c_str(), uFrameNo_);
    return 0;
  }

  HWC2_ALOGE("add buffer-id=%" PRIu64 " releaseFence fail, TunnelId=%d",
             buffer_id, iTunnelId_);
  return -1;
}

// Get ReleaseFence
sp<ReleaseFence> VpContext::GetReleaseFence(uint64_t buffer_id){
  std::lock_guard<std::mutex> lock(mtx_);

  if(!mTimeLine_.isValid()){
    HWC2_ALOGE("mTimeLine_ is invalid, buffer-id=%" PRIu64 " TunnelId=%d",
               buffer_id, iTunnelId_);
    return NULL;
  }

  // 获取 BufferId
  if(mMapBuffer_.count(buffer_id)){
    auto &buffer_info = mMapBuffer_[buffer_id];
    sp<ReleaseFence> release_fence = buffer_info->GetReleaseFence();
    return release_fence;
  }

  return NULL;
}

// Signal ReleaseFence
int VpContext::SignalReleaseFence(uint64_t buffer_id){
  std::lock_guard<std::mutex> lock(mtx_);

  if(!mTimeLine_.isValid()){
    HWC2_ALOGE("mTimeLine_ is invalid, buffer-id=%" PRIu64 " TunnelId=%d",
               buffer_id, iTunnelId_);
    return -1;
  }

  // 获取 BufferId
  if(mMapBuffer_.count(buffer_id)){
    auto &buffer_info = mMapBuffer_[buffer_id];
    buffer_info->SignalReleaseFence();
    return 0;
  }

  HWC2_ALOGE("can't find buffer-id=%" PRIu64 " releaseFence, TunnelId=%d",
             buffer_id, iTunnelId_);
  return -1;
}

int VpContext::SetTimeStamp(int64_t queue_time){
  std::lock_guard<std::mutex> lock(mtx_);
  mQueueFrameTimestamp_ = queue_time;

  struct timespec ts;
  uint64_t timestamp = 0;
  int ret = clock_gettime(CLOCK_MONOTONIC, &ts);
  mAcquireFrameTimestamp_ = ts.tv_sec * 1000 * 1000 + ts.tv_nsec / 1000;
  return 0;
}

// Get queue timestamp
int64_t VpContext::GetQueueTime(){
  std::lock_guard<std::mutex> lock(mtx_);
  return mQueueFrameTimestamp_;
}
// Record timestamp
int64_t VpContext::GetAcquireTime(){
  std::lock_guard<std::mutex> lock(mtx_);
  return mAcquireFrameTimestamp_;
}

int VpContext::VpPrintTimestamp(){
  std::lock_guard<std::mutex> lock(mtx_);
  struct timespec ts;
  uint64_t timestamp = 0;
  int ret = clock_gettime(CLOCK_MONOTONIC, &ts);
  mCommitFrameTimestamp_ = ts.tv_sec * 1000 * 1000 + ts.tv_nsec / 1000;

  HWC2_ALOGD_IF_INFO("Queue->Acquire=%" PRIi64 "ms Queue->Commit=%" PRIi64 "ms",
              (mAcquireFrameTimestamp_ - mQueueFrameTimestamp_) / 1000,
              (mCommitFrameTimestamp_ - mQueueFrameTimestamp_) / 1000);

  return 0;
}

};