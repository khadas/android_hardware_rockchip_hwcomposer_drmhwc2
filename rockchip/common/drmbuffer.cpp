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
#define LOG_TAG "drm-buffer"

#include <drmbuffer.h>
#include <drm_fourcc.h>
#include <rockchip/utils/drmdebug.h>

#include <sync/sync.h>
#include <libsync/sw_sync.h>
#include <cutils/atomic.h>

namespace android{

static uint64_t getUniqueId() {
    static volatile int32_t nextId = 0;
    uint64_t id = static_cast<uint32_t>(android_atomic_inc(&nextId));
    return id;
}
// MALI_GRALLOC_USAGE_NO_AFBC 是 arm_gralloc 扩展的 私有的 usage_bit_flag,
// MALI_GRALLOC_USAGE_NO_AFBC = GRALLOC_USAGE_PRIVATE_1 : 1U << 29
// RK_GRALLOC_USAGE_WITHIN_4G = GRALLOC_USAGE_PRIVATE_11: 1ULL << 56
// RK_GRALLOC_USAGE_STRIDE_ALIGN_64 = GRALLOC_USAGE_PRIVATE_7 : 1ULL << 60
// 定义在 hardware/rockchip/libgralloc/bifrost/src/mali_gralloc_usages.h 中

#ifndef RK_GRALLOC_USAGE_WITHIN_4G
#define RK_GRALLOC_USAGE_WITHIN_4G (1ULL << 56)
#endif

#ifndef RK_GRALLOC_USAGE_STRIDE_ALIGN_16
#define RK_GRALLOC_USAGE_STRIDE_ALIGN_16 (1ULL << 57)
#endif

DrmBuffer::DrmBuffer(int w, int h, int format, uint64_t usage, std::string name, int parent_id):
  uId(getUniqueId()),
  iParentId_(parent_id),
  iFd_(-1),
  iWidth_(w),
  iHeight_(h),
  iFormat_(format),
  iStride_(-1),
  iByteStride_(-1),
  iUsage_(GRALLOC_USAGE_HW_COMPOSER        |
          GRALLOC_USAGE_PRIVATE_1          |
          RK_GRALLOC_USAGE_WITHIN_4G       |
          usage),
  uFourccFormat_(0),
  uModifier_(0),
  iFinishFence_(-1),
  iReleaseFence_(-1),
  bInit_(false),
  sName_(name),
  buffer_(NULL),
  ptrBuffer_(NULL),
  ptrDrmGralloc_(DrmGralloc::getInstance()){
}

DrmBuffer::~DrmBuffer(){
  WaitFinishFence();
  WaitReleaseFence();

  if(ptrBuffer_ != NULL){
    ptrBuffer_ = NULL;
  }

  if (uFbId_ > 0)
    if (drmModeRmFB(ptrDrmGralloc_->get_drm_device(), uFbId_)){
      ALOGE("Failed to rm fb %d", uFbId_);
    }

  int ret = ptrDrmGralloc_->hwc_free_gemhandle(uBufferId_);
  if(ret){
    HWC2_ALOGE("%s hwc_free_gemhandle fail, buffer_id =%" PRIx64, sName_.c_str(), uBufferId_);
  }
}

int DrmBuffer::Init(){
  if(bInit_){
    HWC2_ALOGI("DrmBuffer has init, w=%d h=%d format=%d",iWidth_,iHeight_,iFormat_);
    return 0;
  }

  if(iWidth_ <= 0 || iHeight_ <= 0 || iFormat_ <= 0){
    HWC2_ALOGE("DrmBuffer init fail, w=%d h=%d format=%d",iWidth_,iHeight_,iFormat_);
    return -1;
  }

  ptrBuffer_ = new GraphicBuffer(iWidth_, iHeight_, iFormat_, 0, iUsage_, sName_);
  if(ptrBuffer_->initCheck()) {
    HWC2_ALOGE("new GraphicBuffer fail, w=%d h=%d format=%d",iWidth_,iHeight_,iFormat_);
    return -1;
  }
  buffer_ = ptrBuffer_->handle;
  iFd_     = ptrDrmGralloc_->hwc_get_handle_primefd(buffer_);
  iWidth_  = ptrDrmGralloc_->hwc_get_handle_attibute(buffer_,ATT_WIDTH);
  iHeight_ = ptrDrmGralloc_->hwc_get_handle_attibute(buffer_,ATT_HEIGHT);
  iStride_ = ptrDrmGralloc_->hwc_get_handle_attibute(buffer_,ATT_STRIDE);
  iHeightStride_ = ptrDrmGralloc_->hwc_get_handle_attibute(buffer_,ATT_HEIGHT_STRIDE);
  iByteStride_ = ptrDrmGralloc_->hwc_get_handle_attibute(buffer_,ATT_BYTE_STRIDE_WORKROUND);
  iSize_   = ptrDrmGralloc_->hwc_get_handle_attibute(buffer_,ATT_SIZE);
  iFormat_ = ptrDrmGralloc_->hwc_get_handle_attibute(buffer_,ATT_FORMAT);
  uFourccFormat_ = ptrDrmGralloc_->hwc_get_handle_fourcc_format(buffer_);
  uModifier_ = ptrDrmGralloc_->hwc_get_handle_format_modifier(buffer_);
  ptrDrmGralloc_->hwc_get_handle_buffer_id(buffer_, &uBufferId_);
  int ret = ptrDrmGralloc_->hwc_get_gemhandle_from_fd(iFd_, uBufferId_, &uGemHandle_);
  if(ret){
    HWC2_ALOGE("%s hwc_get_gemhandle_from_fd fail, buffer_id =%" PRIx64, sName_.c_str(), uBufferId_);
    return -1;
  }
  uFbId_ = 0;
  bInit_ = true;

  return 0;
}

bool DrmBuffer::initCheck(){
  return bInit_;
}
buffer_handle_t DrmBuffer::GetHandle(){
  return ptrBuffer_->handle;
}
std::string DrmBuffer::GetName(){
  return sName_;
}
uint64_t DrmBuffer::GetId(){
  return uId;
}
int DrmBuffer::GetParentId(){
  return iParentId_;
}
int DrmBuffer::SetParentId(int parent_id){
  iParentId_ = parent_id;
  return 0;
}
int DrmBuffer::GetFd(){
  return iFd_;
}
int DrmBuffer::GetWidth(){
  return iWidth_;
}
int DrmBuffer::GetHeight(){
  return iHeight_;
}
int DrmBuffer::GetHeightStride(){
  return iHeightStride_;
}
int DrmBuffer::GetFormat(){
  return iFormat_;
}
int DrmBuffer::GetStride(){
  return iStride_;
}
int DrmBuffer::GetByteStride(){
  return iByteStride_;
}
int DrmBuffer::GetSize(){
  return iSize_;
}
uint64_t DrmBuffer::GetUsage(){
  return iUsage_;
}
int DrmBuffer::SetCrop(int left, int top, int right, int bottom){
  iLeft_  = left;
  iTop_   = top;
  iRight_ = right;
  iBottom_  = bottom;
  return 0;
}

int DrmBuffer::GetCrop(int *left, int *top, int *right, int *bottom){
  *left   = iLeft_;
  *top    = iTop_;
  *right  = iRight_;
  *bottom = iBottom_;
  return 0;
}
uint32_t DrmBuffer::GetFourccFormat(){
  return uFourccFormat_;
}
uint64_t DrmBuffer::GetModifier(){
  return uModifier_;
}
uint64_t DrmBuffer::GetBufferId(){
  return uBufferId_;
}
uint32_t DrmBuffer::GetGemHandle(){
  return uGemHandle_;
}

uint32_t DrmBuffer::DrmFormatToPlaneNum(uint32_t drm_format) {
  switch (drm_format) {
    case DRM_FORMAT_NV12:
    case DRM_FORMAT_NV21:
    case DRM_FORMAT_NV24:
    case DRM_FORMAT_NV42:
    case DRM_FORMAT_NV16:
    case DRM_FORMAT_NV61:
    case DRM_FORMAT_NV12_10:
    case DRM_FORMAT_NV15:
      return 2;
    default:
      return 1;
  }
}

uint32_t DrmBuffer::GetFbId(){
  if(uFbId_ > 0)
    return uFbId_;
  uint32_t pitches[4] = {0};
  uint32_t offsets[4] = {0};
  uint32_t gem_handles[4] = {0};
  uint64_t modifier[4] = {0};

  pitches[0] = iByteStride_;
  gem_handles[0] = uGemHandle_;
  offsets[0] = 0;

  if(DrmFormatToPlaneNum(uFourccFormat_) == 2){
    if(uFourccFormat_ == DRM_FORMAT_NV24 ||
       uFourccFormat_ == DRM_FORMAT_NV42){
      pitches[1] = pitches[0]*2;
      gem_handles[1] = uGemHandle_;
      offsets[1] = pitches[0] * iHeight_;
    }else{
      pitches[1] = pitches[0];
      gem_handles[1] = uGemHandle_;
      offsets[1] = pitches[1] * iHeight_;
    }
  }

  modifier[0] = uModifier_;
  if(DrmFormatToPlaneNum(uFourccFormat_) == 2)
    modifier[1] = uModifier_;

  int ret = drmModeAddFB2WithModifiers(ptrDrmGralloc_->get_drm_device(),
                                       iWidth_,
                                       iHeight_,
                                       uFourccFormat_,
                                       gem_handles,
                                       pitches,
                                       offsets,
                                       modifier,
		                                   &uFbId_,
                                       DRM_MODE_FB_MODIFIERS);

  HWC2_ALOGD_IF_DEBUG("ImportBuffer fd=%d,w=%d,h=%d,bo->format=%c%c%c%c,"
                      "gem_handle=%d,bo->pitches[0]=%d,fb_id=%d,modifier = %" PRIx64 ,
                       ptrDrmGralloc_->get_drm_device(),iWidth_, iHeight_,
                       uFourccFormat_, uFourccFormat_ >> 8, uFourccFormat_ >> 16, uFourccFormat_ >> 24,
                       gem_handles[0], pitches[0], uFbId_, modifier[0]);

  if (ret) {
    ALOGE("could not create drm fb %d", ret);
    HWC2_ALOGE("ImportBuffer fd=%d,w=%d,h=%d,bo->format=%c%c%c%c,"
               "gem_handle=%d,bo->pitches[0]=%d,fb_id=%d,modifier = %" PRIx64 ,
                ptrDrmGralloc_->get_drm_device(),iWidth_, iHeight_,
                uFourccFormat_, uFourccFormat_ >> 8, uFourccFormat_ >> 16, uFourccFormat_ >> 24,
                gem_handles[0], pitches[0], uFbId_, modifier[0]);
    return ret;
  }

  return uFbId_;
}
void* DrmBuffer::Lock(){
  if(!buffer_){
    HWC2_ALOGI("LayerId=%" PRIu64 " Buffer is null.",uId);
    return NULL;
  }

  void* cpu_addr = NULL;
  static int frame_cnt =0;
  int ret = 0;
  cpu_addr = ptrDrmGralloc_->hwc_get_handle_lock(buffer_,iWidth_,iHeight_);
  if(ret){
    HWC2_ALOGE("buffer-id=%" PRIu64 " lock fail ret = %d ",uId,ret);
    return NULL;
  }

  return cpu_addr;
}

int DrmBuffer::Unlock(){
  if(!buffer_){
    HWC2_ALOGI("LayerId=%" PRIu64 " Buffer is null.",uId);
    return -1;
  }

  int ret = ptrDrmGralloc_->hwc_get_handle_unlock(buffer_);
  if(ret){
    HWC2_ALOGE("buffer-id=%" PRIu64 " unlock fail ret = %d ",uId,ret);
    return ret;
  }
  return ret;
}

int DrmBuffer::GetFinishFence(){
  return iFinishFence_.Dup();
}

int DrmBuffer::SetFinishFence(int fence){
  if(WaitFinishFence()){
    return -1;
  }
  iFinishFence_.Set(fence);
  return 0;
}

int DrmBuffer::WaitFinishFence(){
  int ret = 0;
  if(iFinishFence_.get() > 0){
    int ret = sync_wait(iFinishFence_.get(), 1500);
    if (ret) {
      HWC2_ALOGE("Failed to wait for RGA finish fence %d/%d 1500ms", iFinishFence_.get(), ret);
    }
    iFinishFence_.Close();
  }
  return ret;
}

int DrmBuffer::GetReleaseFence(){
  return iReleaseFence_.Dup();
}

int DrmBuffer::SetReleaseFence(int fence){
  iReleaseFence_.Set(fence);
  return 0;
}

int DrmBuffer::WaitReleaseFence(){
  int ret = 0;
  if(iReleaseFence_.get() > 0){
    int ret = sync_wait(iReleaseFence_.get(), 1500);
    if (ret) {
      HWC2_ALOGE("Failed to wait for RGA finish fence %d/%d 1500ms", iReleaseFence_.get(), ret);
    }
    iReleaseFence_.Close();
  }
  return ret;
}

int DrmBuffer::DumpData(){
  if(!buffer_)
    HWC2_ALOGI("LayerId=%" PRIu64 " Buffer is null.",uId);

  WaitFinishFence();

  void* cpu_addr = NULL;
  static int frame_cnt =0;
  int ret = 0;
  cpu_addr = ptrDrmGralloc_->hwc_get_handle_lock(buffer_,iWidth_,iHeight_);
  if(ret){
    HWC2_ALOGE("buffer-id=%" PRIu64 " lock fail ret = %d ",uId,ret);
    return ret;
  }
  FILE * pfile = NULL;
  char data_name[100] ;
  system("mkdir /data/dump/ && chmod /data/dump/ 777 ");
  sprintf(data_name,"/data/dump/%d_%5.5s_id-%" PRIu64 "_%dx%d.bin",
          frame_cnt++,sName_.size() < 5 ? "unset" : sName_.c_str(),
          uId,iStride_,iHeight_);

  pfile = fopen(data_name,"wb");
  if(pfile)
  {
      fwrite((const void *)cpu_addr,(size_t)(iSize_),1,pfile);
      fflush(pfile);
      fclose(pfile);
      HWC2_ALOGI("dump surface layer_id=%" PRIu64 " ,data_name %s,w:%d,h:%d,stride :%d,size=%d,cpu_addr=%p",
          uId,data_name,iWidth_,iStride_,iByteStride_,iSize_,cpu_addr);
  }
  else
  {
      HWC2_ALOGE("Open %s fail", data_name);
      HWC2_ALOGI("dump surface layer_id=%" PRIu64 " ,data_name %s,w:%d,h:%d,stride :%d,size=%d,cpu_addr=%p",
          uId,data_name,iWidth_,iStride_,iByteStride_,iSize_,cpu_addr);
  }


  ret = ptrDrmGralloc_->hwc_get_handle_unlock(buffer_);
  if(ret){
    HWC2_ALOGE("buffer-id=%" PRIu64 " unlock fail ret = %d ",uId,ret);
    return ret;
  }

  return ret;
}

} // namespace android

