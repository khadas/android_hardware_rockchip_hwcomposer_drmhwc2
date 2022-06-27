/*
 * Copyright (C) 2018 The Android Open Source Project
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

#define LOG_TAG "hwc-resource-manager"

#include "resourcemanager.h"
#include "drmlayer.h"

#include <cutils/properties.h>
#include <log/log.h>
#include <sstream>
#include <string>

#include <drm_fourcc.h>
#include <xf86drm.h>
#include <xf86drmMode.h>

#include <im2d.hpp>
//XML prase
#include <tinyxml2.h>

namespace android {

#define ALIGN_DOWN( value, base)	(value & (~(base-1)) )

ResourceManager::ResourceManager() :
  num_displays_(0) {
  drmGralloc_ = DrmGralloc::getInstance();
}

int ResourceManager::Init(DrmHwcTwo *hwc2) {
  hwc2_ = hwc2;
  char path_pattern[PROPERTY_VALUE_MAX];
  // Could be a valid path or it can have at the end of it the wildcard %
  // which means that it will try open all devices until an error is met.
  int path_len = property_get("vendor.hwc.drm.device", path_pattern, "/dev/dri/card0");
  int ret = 0;
  if (path_pattern[path_len - 1] != '%') {
    ret = AddDrmDevice(std::string(path_pattern));
  } else {
    path_pattern[path_len - 1] = '\0';
    for (int idx = 0; !ret; ++idx) {
      std::ostringstream path;
      path << path_pattern << idx;
      ret = AddDrmDevice(path.str());
    }
  }

  if (!num_displays_) {
    ALOGE("Failed to initialize any displays");
    return ret ? -EINVAL : ret;
  }


  fb0_fd = open("/dev/graphics/fb0", O_RDWR, 0);
  if(fb0_fd < 0){
    ALOGE("Open fb0 fail in %s",__FUNCTION__);
  }

  DrmDevice *drm = drms_.front().get();
  for(auto &crtc : drm->crtcs()){
    mapDrmDisplayCompositor_.insert(
      std::pair<int, std::shared_ptr<DrmDisplayCompositor>>(crtc->id(),std::make_shared<DrmDisplayCompositor>()));
    HWC2_ALOGI("Create DrmDisplayCompositor crtc=%d",crtc->id());
  }

  displays_ = drm->GetDisplays();
  if(displays_.size() == 0){
    ALOGE("Failed to initialize any displays");
    return ret ? -EINVAL : ret;
  }

  hwcPlatform_ = HwcPlatform::CreateInstance(drm);
  if (!hwcPlatform_) {
    ALOGE("Failed to create HwcPlatform instance");
    return -1;
  }

  return 0;
}

int ResourceManager::AddDrmDevice(std::string path) {
  std::unique_ptr<DrmDevice> drm = std::make_unique<DrmDevice>();
  int displays_added, ret;
  std::tie(ret, displays_added) = drm->Init(path.c_str(), num_displays_);
  if (ret)
    return ret;

  //Get soc id
  soc_id_ = drm->getSocId();
  //DrmVersion
  drmVersion_ = drm->getDrmVersion();
  drmGralloc_->set_drm_version(dup(drm->fd()),drmVersion_);

  std::shared_ptr<Importer> importer;
  importer.reset(Importer::CreateInstance(drm.get()));
  if (!importer) {
    ALOGE("Failed to create importer instance");
    return -ENODEV;
  }
  importers_.push_back(std::move(importer));
  drms_.push_back(std::move(drm));
  num_displays_ += displays_added;
  return ret;
}

DrmConnector *ResourceManager::AvailableWritebackConnector(int display) {
  DrmDevice *drm_device = GetDrmDevice(display);
  DrmConnector *writeback_conn = NULL;
  if (drm_device) {
    writeback_conn = drm_device->AvailableWritebackConnector(display);
    if (writeback_conn)
      return writeback_conn;
  }
  for (auto &drm : drms_) {
    if (drm.get() == drm_device)
      continue;
    writeback_conn = drm->AvailableWritebackConnector(display);
    if (writeback_conn)
      return writeback_conn;
  }
  return writeback_conn;
}

DrmDevice *ResourceManager::GetDrmDevice(int display) {
  for (auto &drm : drms_) {
    if (drm->HandlesDisplay(display & ~DRM_CONNECTOR_SPILT_MODE_MASK))
      return drm.get();
  }
  return NULL;
}

std::shared_ptr<Importer> ResourceManager::GetImporter(int display) {
  for (unsigned int i = 0; i < drms_.size(); i++) {
    if (drms_[i]->HandlesDisplay(display & ~DRM_CONNECTOR_SPILT_MODE_MASK))
      return importers_[i];
  }
  return NULL;
}

std::shared_ptr<DrmDisplayCompositor> ResourceManager::GetDrmDisplayCompositor(DrmCrtc* crtc){
  if(!crtc){
    HWC2_ALOGE("crtc is null");
    return NULL;
  }

  if(mapDrmDisplayCompositor_.size() == 0){
    HWC2_ALOGE("mapDrmDisplayCompositor_.size()=0");
    return NULL;
  }

  auto pairDrmDisplayCompositor = mapDrmDisplayCompositor_.find(crtc->id());
  return pairDrmDisplayCompositor->second;
}

int ResourceManager::assignPlaneGroup(){
  uint32_t active_display_num = getActiveDisplayCnt();
  if(active_display_num==0){
    ALOGI_IF(DBG_INFO,"%s,line=%d, active_display_num = %u not to assignPlaneGroup",
                                 __FUNCTION__,__LINE__,active_display_num);
    return -1;
  }

  int ret = hwcPlatform_->TryAssignPlane(drms_.front().get(), active_display_);
  if(ret){
    HWC2_ALOGI("TryAssignPlane fail, ret = %d",ret);
    return ret;
  }
  return 0;
}

int ResourceManager::GetWBDisplay() const {
  std::lock_guard<std::mutex> lock(mtx_);
  return iWriteBackDisplayId_;
}

bool ResourceManager::isWBMode() const {
  std::lock_guard<std::mutex> lock(mtx_);
  return bEnableWriteBack_ > 0;
}

const DrmMode& ResourceManager::GetWBMode() const {
  std::lock_guard<std::mutex> lock(mtx_);
  return mWBMode_;
}

int ResourceManager::EnableWriteBackMode(int display){
  std::lock_guard<std::mutex> lock(mtx_);

  // 1. 检查 WB 模块是否已经被绑定
  if(bEnableWriteBack_ > 0){
    if(iWriteBackDisplayId_ != display){
      HWC2_ALOGE("WriteBack has bind display %d, so display=%d WB request can't handle.",
                iWriteBackDisplayId_, display);
      return -1;
    }else{
      bEnableWriteBack_++;
      return 0;
    }
  }

  // 2. 获取待 WriteBack display状态，状态异常则直接关闭 WriteBack 模式
  DrmDevice *drmDevice = GetDrmDevice(display);
  DrmConnector *writeBackConn = drmDevice->GetConnectorForDisplay(display);
  if(!writeBackConn){
    HWC2_ALOGE("display=%d WriteBackConn is NULL", display);
    return -1;
  }

  if(writeBackConn->state() != DRM_MODE_CONNECTED){
    HWC2_ALOGE("display=%d WriteBackConn state isn't connected(%d)",
                display, writeBackConn->state());
    return -1;
  }

  // 3. 获取待 WriteBack 当前分辨率，用于申请 WriteBackBuffer
  // 4. WriteBack 硬件要求 16对齐，否则超出部分会直接丢弃
  DrmMode currentMode = writeBackConn->current_mode();
  mWBMode_ = currentMode;
  iWBWidth_  = ALIGN_DOWN(currentMode.width(),16);
  iWBHeight_ = currentMode.height();
  iWBFormat_ = HAL_PIXEL_FORMAT_YCrCb_NV12;


  // 5. 创建 WriteBackBuffer BufferQueue，并且申请 WB Buffer.
  if(mWriteBackBQ_ == NULL){
    mWriteBackBQ_ = std::make_shared<DrmBufferQueue>();

    mNextWriteBackBuffer_
      = mWriteBackBQ_->DequeueDrmBuffer(iWBWidth_,
                                        iWBHeight_,
                                        iWBFormat_,
                                        RK_GRALLOC_USAGE_STRIDE_ALIGN_16,
                                        "WriteBackBuffer");
    if(!mNextWriteBackBuffer_->initCheck()){
      HWC2_ALOGE("display=%d WBBuffer Dequeue fail, w=%d h=%d format=%d",
                                        display,
                                        iWBWidth_,
                                        iWBHeight_,
                                        iWBFormat_);
      return -1;
    }
  }

  bEnableWriteBack_++;
  iWriteBackDisplayId_ = display;
  return 0;
}


int ResourceManager::UpdateWriteBackResolution(int display){
  std::lock_guard<std::mutex> lock(mtx_);

  // 1. 检查 WB 模块是否已经被绑定
  if(bEnableWriteBack_ > 0){
    if(iWriteBackDisplayId_ != display){
      HWC2_ALOGE("WriteBack has bind display %d, so display=%d WB request can't handle.",
                iWriteBackDisplayId_, display);
      return -1;
    }
  }

  // 2. 获取待 WriteBack display状态，状态异常则直接关闭 WriteBack 模式
  DrmDevice *drmDevice = GetDrmDevice(display);
  DrmConnector *writeBackConn = drmDevice->GetConnectorForDisplay(display);
  if(!writeBackConn){
    HWC2_ALOGE("display=%d WriteBackConn is NULL", display);
    return -1;
  }

  if(writeBackConn->state() != DRM_MODE_CONNECTED){
    HWC2_ALOGE("display=%d WriteBackConn state isn't connected(%d)",
                display, writeBackConn->state());
    return -1;
  }

  // 3. 获取待 WriteBack 当前分辨率，用于申请 WriteBackBuffer
  DrmMode currentMode = writeBackConn->current_mode();
  mWBMode_ = currentMode;
  int tempWBWidth  = ALIGN_DOWN(mWBMode_.width(),16);
  int tempWBHeight = mWBMode_.height();
  if(tempWBWidth == iWBWidth_ &&
     tempWBHeight == iWBHeight_){
    return 0;
  }else{
    HWC2_ALOGI("display=%d update WriteBack resolution(%dx%d)=>(%dx%d)",
                display, iWBWidth_, iWBHeight_,
                currentMode.width(), currentMode.height());
  }

  iWBWidth_  = tempWBWidth;
  iWBHeight_ = tempWBHeight;
  iWBFormat_ = HAL_PIXEL_FORMAT_YCrCb_NV12;

  // 4. 创建 WriteBackBuffer BufferQueue，并且申请 WB Buffer.
  if(mWriteBackBQ_ == NULL){
    mWriteBackBQ_ = std::make_shared<DrmBufferQueue>();
  }

  mNextWriteBackBuffer_
    = mWriteBackBQ_->DequeueDrmBuffer(iWBWidth_,
                                      iWBHeight_,
                                      iWBFormat_,
                                      RK_GRALLOC_USAGE_STRIDE_ALIGN_16,
                                      "WriteBackBuffer");
  if(!mNextWriteBackBuffer_->initCheck()){
    HWC2_ALOGE("display=%d WBBuffer Dequeue fail, w=%d h=%d format=%d",
                display, iWBWidth_, iWBHeight_, iWBFormat_);
    return -1;
  }
  return 0;
}

int ResourceManager::DisableWriteBackMode(int display){
  std::lock_guard<std::mutex> lock(mtx_);
  if(display != iWriteBackDisplayId_)
    return 0;

  bEnableWriteBack_--;
  if(bEnableWriteBack_ <= 0){
    mNextWriteBackBuffer_ = NULL;
    mDrawingWriteBackBuffer_ = NULL;
    mFinishWriteBackBuffer_ = NULL;
    mWriteBackBQ_ = NULL;
    iWriteBackDisplayId_ = -1;
  }
  return 0;
}

std::shared_ptr<DrmBuffer> ResourceManager::GetResetWBBuffer(){
  std::lock_guard<std::mutex> lock(mtx_);
  if(mResetBackBuffer_ == NULL){
    mResetBackBuffer_ =  std::make_shared<DrmBuffer>(640,
                                                     360,
                                                     HAL_PIXEL_FORMAT_YCrCb_NV12,
                                                     RK_GRALLOC_USAGE_STRIDE_ALIGN_16,
                                                     "WBResetBuffer");
    if(mResetBackBuffer_->Init()){
      HWC2_ALOGE("DrmBuffer Init fail, w=%d h=%d format=%d name=%s",
                  640, 360, HAL_PIXEL_FORMAT_YCrCb_NV12, "WBResetBuffer");
      mResetBackBuffer_ = NULL;
      return NULL;
    }

    rga_buffer_t src;
    im_rect src_rect;

    // Set src buffer info
    src.fd      = mResetBackBuffer_->GetFd();
    src.width   = mResetBackBuffer_->GetWidth();
    src.height  = mResetBackBuffer_->GetHeight();
    src.wstride = mResetBackBuffer_->GetStride();
    src.hstride = mResetBackBuffer_->GetHeight();
    src.format  = mResetBackBuffer_->GetFormat();

    // Set src rect info
    src_rect.x = 0;
    src_rect.y = 0;
    src_rect.width  = src.width ;
    src_rect.height = src.height;

    src.color_space_mode = IM_RGB_TO_YUV_BT601_LIMIT;

    // Set Dataspace
    // if((buffer.mBufferInfo_.uDataSpace_ & HAL_DATASPACE_STANDARD_BT709) == HAL_DATASPACE_STANDARD_BT709){
    //   dst.color_space_mode = IM_YUV_TO_RGB_BT709_LIMIT;
    //   SVEP_ALOGD_IF("color_space_mode = BT709 dataspace=0x%" PRIx64,buffer.mBufferInfo_.uDataSpace_);
    // }else{
    //   SVEP_ALOGD_IF("color_space_mode = BT601 dataspace=0x%" PRIx64,buffer.mBufferInfo_.uDataSpace_);
    // }

    IM_STATUS im_state;

    // Call Im2d 格式转换
    im_state = imfill(src, src_rect, 0x0);

    if(im_state != IM_STATUS_SUCCESS){
      HWC2_ALOGE("call im2d reset Fail!");
    }
  }
  return mResetBackBuffer_;
}
std::shared_ptr<DrmBuffer> ResourceManager::GetNextWBBuffer(){
  std::lock_guard<std::mutex> lock(mtx_);
  return mNextWriteBackBuffer_;
}

std::shared_ptr<DrmBuffer> ResourceManager::GetDrawingWBBuffer(){
  std::lock_guard<std::mutex> lock(mtx_);
  return mDrawingWriteBackBuffer_;
}

std::shared_ptr<DrmBuffer> ResourceManager::GetFinishWBBuffer(){
  std::lock_guard<std::mutex> lock(mtx_);
  return mFinishWriteBackBuffer_;
}

int ResourceManager::SwapWBBuffer(){
  std::lock_guard<std::mutex> lock(mtx_);
  if(bEnableWriteBack_ <= 0){
    HWC2_ALOGE("");
    return -1;
  }

  // 1. Drawing 切换为 Finish 状态
  mFinishWriteBackBuffer_ = mDrawingWriteBackBuffer_;

  // 2. Next 切换为 Drawing 状态
  mDrawingWriteBackBuffer_ = mNextWriteBackBuffer_;
  if(mWriteBackBQ_->QueueBuffer(mDrawingWriteBackBuffer_)){
    HWC2_ALOGE("display=%d WBBuffer Queue fail, w=%d h=%d format=%d",
                                      iWriteBackDisplayId_,
                                      iWBWidth_,
                                      iWBHeight_,
                                      iWBFormat_);
    return -1;
  }

  // 3. 申请 Next Buffer
  std::shared_ptr<DrmBuffer> next
    = mWriteBackBQ_->DequeueDrmBuffer(iWBWidth_,
                                      iWBHeight_,
                                      iWBFormat_,
                                      RK_GRALLOC_USAGE_STRIDE_ALIGN_16,
                                      "WriteBackBuffer");
  if(!next->initCheck()){
    HWC2_ALOGE("display=%d WBBuffer Dequeue fail, w=%d h=%d format=%d",
                                      iWriteBackDisplayId_,
                                      iWBWidth_,
                                      iWBHeight_,
                                      iWBFormat_);
    return -1;
  }

  mNextWriteBackBuffer_ = next;
  return 0;
}


}  // namespace android
