/*
 * Copyright (C) 2018-2021, The Linux Foundation. All rights reserved.
 * Not a Contribution.
 *
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

#define LOG_TAG "android.hardware.usb.gadget@1.1-service-qti"

#include <android-base/file.h>
#include <android-base/properties.h>
#include <functional>
#include <map>
#include <dirent.h>
#include <fcntl.h>
#include <stdio.h>
#include <sys/types.h>
#include <unistd.h>
#include <hidl/HidlTransportSupport.h>
#include <UsbGadgetCommon.h>
#include "UsbGadget.h"

#define ESOC_DEVICE_PATH "/sys/bus/esoc/devices"
#define SOC_MACHINE_PATH "/sys/devices/soc0/machine"
#define USB_CONTROLLER_PROP "vendor.usb.controller"
#define DIAG_FUNC_NAME_PROP "vendor.usb.diag.func.name"
#define RNDIS_FUNC_NAME_PROP "vendor.usb.rndis.func.name"
#define RMNET_FUNC_NAME_PROP "vendor.usb.rmnet.func.name"
#define RMNET_INST_NAME_PROP "vendor.usb.rmnet.inst.name"
#define DPL_INST_NAME_PROP "vendor.usb.dpl.inst.name"
#define VENDOR_USB_PROP "vendor.usb.config"
#define PERSIST_VENDOR_USB_PROP "persist.vendor.usb.config"
#define QDSS_INST_NAME_PROP "vendor.usb.qdss.inst.name"

enum mdmType {
  INTERNAL,
  EXTERNAL,
  INTERNAL_EXTERNAL,
  NONE,
};

namespace android {
namespace hardware {
namespace usb {
namespace gadget {
namespace V1_1 {
namespace implementation {

using ::android::sp;
using ::android::base::GetProperty;
using ::android::base::SetProperty;
using ::android::base::WriteStringToFile;
using ::android::base::ReadFileToString;
using ::android::hardware::Return;
using ::android::hardware::Void;
using ::android::hardware::usb::gadget::V1_0::GadgetFunction;
using ::android::hardware::usb::gadget::V1_0::Status;
using ::android::hardware::usb::gadget::addAdb;
using ::android::hardware::usb::gadget::kDisconnectWaitUs;
using ::android::hardware::usb::gadget::linkFunction;
using ::android::hardware::usb::gadget::resetGadget;
using ::android::hardware::usb::gadget::setVidPid;
using ::android::hardware::usb::gadget::unlinkFunctions;

UsbGadget::UsbGadget(const char* const gadget)
    : mCurrentUsbFunctionsApplied(false),
      mMonitorFfs(gadget) {
  if (access(OS_DESC_PATH, R_OK) != 0)
    ALOGE("configfs setup not done yet");
}

Return<void> UsbGadget::getCurrentUsbFunctions(
    const sp<V1_0::IUsbGadgetCallback> &callback) {
  Return<void> ret = callback->getCurrentUsbFunctionsCb(
      mCurrentUsbFunctions, mCurrentUsbFunctionsApplied
                                ? Status::FUNCTIONS_APPLIED
                                : Status::FUNCTIONS_NOT_APPLIED);
  if (!ret.isOk())
    ALOGE("Call to getCurrentUsbFunctionsCb failed %s",
          ret.description().c_str());

  return Void();
}

Return<Status> UsbGadget::reset() {
  if (!WriteStringToFile("none", PULLUP_PATH)) {
    ALOGE("reset(): unable to clear pullup");
    return Status::ERROR;
  }

  return Status::SUCCESS;
}

V1_0::Status UsbGadget::tearDownGadget() {
  if (resetGadget() != Status::SUCCESS) return Status::ERROR;

  if (mMonitorFfs.isMonitorRunning())
    mMonitorFfs.reset();
  else
    ALOGE("mMonitor not running");

  return Status::SUCCESS;
}

static std::map<std::string, std::pair<const char *, const char *> >
supported_compositions {
  { "mass_storage", { "0x05C6", "0xF000" } },
  { "mass_storage,adb", { "0x05C6", "0x9015" } },
  { "diag,adb", { "0x05C6", "0x901D" } },
  { "diag", { "0x05C6", "0x900E" } },
  { "diag,serial_cdev,rmnet,adb", { "0x05C6", "0x9091" } },
  { "diag,serial_cdev,rmnet", { "0x05C6", "0x9092" } },
  { "rndis", { "0x05C6", "0xF00E" } },
  { "rndis,adb", { "0x05C6", "0x9024" } },
  { "rndis,diag", { "0x05C6", "0x902C" } },
  { "rndis,diag,adb", { "0x05C6", "0x902D" } },
  { "rndis,serial_cdev", { "0x05C6", "0x90B3" } },
  { "rndis,serial_cdev,adb", { "0x05C6", "0x90B4" } },
  { "rndis,serial_cdev,diag,", { "0x05C6", "0x90B5" } },
  { "rndis,serial_cdev,diag,adb", { "0x05C6", "0x90B6" } },
  { "mtp,diag", { "0x05C6", "0x901B" } },
  { "mtp,diag,adb", { "0x05C6", "0x903A" } },
  { "diag,qdss", { "0x05C6", "0x904A" } },
  { "diag,qdss,adb", { "0x05C6", "0x9060" } },
  { "rndis,diag,qdss", { "0x05C6", "0x9081" } },
  { "rndis,diag,qdss,adb", { "0x05C6", "0x9082" } },
  { "diag,qdss,rmnet", { "0x05C6", "0x9083" } },
  { "diag,qdss,rmnet,adb", { "0x05C6", "0x9084" } },
  { "ncm", { "0x05C6", "0xA4A1" } },
  { "ncm,adb", { "0x05C6", "0x908C" } },
  { "diag,serial_cdev", { "0x05C6", "0x9004" } },
  { "diag,serial_cdev,rmnet,dpl", { "0x05C6", "0x90B7" } },
  { "diag,serial_cdev,rmnet,dpl,adb", { "0x05C6", "0x90B8" } },
  { "rndis,diag,dpl", { "0x05C6", "0x90BF" } },
  { "rndis,diag,dpl,adb", { "0x05C6", "0x90C0" } },
  { "ccid", { "0x05C6", "0x90CE" } },
  { "ccid,adb", { "0x05C6", "0x90CF" } },
  { "ccid,diag", { "0x05C6", "0x90D0" } },
  { "ccid,diag,adb", { "0x05C6", "0x90D1" } },
  { "diag,serial_cdev,rmnet,ccid", { "0x05C6", "0x90D2" } },
  { "diag,serial_cdev,rmnet,ccid,adb", { "0x05C6", "0x90D3" } },
  { "diag,diag_mdm,qdss,qdss_mdm,serial_cdev,serial_cdev_mdm,rmnet",
      { "0x05C6", "0x90D7" } },
  { "diag,diag_mdm,qdss,qdss_mdm,serial_cdev,serial_cdev_mdm,rmnet,adb",
      { "0x05C6", "0x90D8" } },
  { "diag,diag_mdm,qdss,qdss_mdm,serial_cdev,serial_cdev_mdm,dpl,rmnet",
      { "0x05C6", "0x90DD" } },
  { "diag,diag_mdm,qdss,qdss_mdm,serial_cdev,serial_cdev_mdm,dpl,rmnet,adb",
      { "0x05C6", "0x90DE" } },
  { "diag,serial_cdev,rmnet,dpl,qdss", { "0x05C6", "0x90DC" } },
  { "diag,serial_cdev,rmnet,dpl,qdss,adb", { "0x05C6", "0x90DB" } },
  { "diag,uac2,adb", { "0x05C6", "0x90CA" } },
  { "diag,uac2", { "0x05C6", "0x901C" } },
  { "diag,uvc,adb", { "0x05C6", "0x90CB" } },
  { "diag,uvc", { "0x05C6", "0x90DF" } },
  { "diag,uac2,uvc,adb", { "0x05C6", "0x90CC" } },
  { "diag,uac2,uvc", { "0x05C6", "0x90E0" } },
  { "diag,diag_mdm,qdss,qdss_mdm,serial_cdev,dpl,rmnet",
      { "0x05C6", "0x90E4" } },
  { "diag,diag_mdm,qdss,qdss_mdm,serial_cdev,dpl,rmnet,adb",
      { "0x05C6", "0x90E5" } },
  { "rndis,diag,diag_mdm,qdss,qdss_mdm,serial_cdev,dpl",
      { "0x05C6", "0x90E6" } },
  { "rndis,diag,diag_mdm,qdss,qdss_mdm,serial_cdev,dpl,adb",
      { "0x05C6", "0x90E7" } },
  { "rndis,diag,qdss,serial_cdev,dpl", { "0x05C6", "0x90E8" } },
  { "rndis,diag,qdss,serial_cdev,dpl,adb", { "0x05C6", "0x90E9" } },
  { "diag,diag_mdm,adb", { "0x05C6", "0x90D9" } },
  { "diag,diag_mdm,diag_mdm2,qdss,qdss_mdm,serial_cdev,dpl,rmnet",
      { "0x05C6", "0x90F6" } },
  { "diag,diag_mdm,diag_mdm2,qdss,qdss_mdm,serial_cdev,dpl,rmnet,adb",
      { "0x05C6", "0x90F7" } },
  { "rndis,diag,diag_mdm,diag_mdm2,qdss,qdss_mdm,serial_cdev,dpl",
      { "0x05C6", "0x90F8" } },
  { "rndis,diag,diag_mdm,diag_mdm2,qdss,qdss_mdm,serial_cdev,dpl,adb",
      { "0x05C6", "0x90F9" } },
  { "diag,diag_mdm,qdss_mdm,dpl,adb", { "0x05C6", "0x90FF" } },
  { "diag,qdss,dpl,adb", { "0x05C6", "0x9104" } },
  { "diag,dpl", { "0x05C6", "0x9105" } },
  { "diag,diag_cnss,serial_cdev,rmnet,dpl,qdss,adb", { "0x05C6", "0x9110" } },
  { "diag,diag_cnss,serial_cdev,rmnet,dpl,qdss", { "0x05C6", "0x9111" } },
};

static std::string rndisFuncname() {
  std::string rndisFunc = GetProperty(RNDIS_FUNC_NAME_PROP, "");

  if (rndisFunc.empty()) {
    return "rndis";
  }

  return rndisFunc + ".rndis";
}

static std::map<std::string, std::function<std::string()> > supported_funcs {
  { "adb",              [](){ return "ffs.adb"; } },
  { "ccid",             [](){ return "ccid.ccid"; } },
  { "diag",             [](){ return GetProperty(DIAG_FUNC_NAME_PROP, "diag") + ".diag"; } },
  { "diag_cnss",        [](){ return GetProperty(DIAG_FUNC_NAME_PROP, "diag") + ".diag_mdm2"; } },
  { "diag_mdm2",        [](){ return GetProperty(DIAG_FUNC_NAME_PROP, "diag") + ".diag_mdm2"; } },
  { "diag_mdm",         [](){ return GetProperty(DIAG_FUNC_NAME_PROP, "diag") + ".diag_mdm"; } },
  { "dpl",              [](){ return GetProperty(RMNET_FUNC_NAME_PROP, "gsi") + "." + GetProperty(DPL_INST_NAME_PROP, "dpl"); } },
  { "mass_storage",     [](){ return "mass_storage.0"; } },
  { "mtp",              [](){ return "ffs.mtp"; } },
  { "ncm",              [](){ return "ncm.0"; } },
  { "ptp",              [](){ return "ffs.ptp"; } },
  { "qdss",             [](){ return "qdss." + GetProperty(QDSS_INST_NAME_PROP, "qdss"); } },
  { "qdss_mdm",         [](){ return "qdss.qdss_mdm"; } },
  { "rmnet",            [](){ return GetProperty(RMNET_FUNC_NAME_PROP, "gsi") + "." + GetProperty(RMNET_INST_NAME_PROP, "rmnet"); } },
  { "rndis",            rndisFuncname },
  { "serial_cdev",      [](){ return "cser.dun.0"; } },
  { "serial_cdev_mdm",  [](){ return "cser.dun.2"; } },
  { "uac2",             [](){ return "uac2.0"; } },
  { "uvc",              [](){ return "uvc.0"; } },
};

static int addFunctionsFromPropString(std::string prop, int &i, bool adb) {
  // tokenize the prop string and add each function individually
  for (size_t start = 0; start != std::string::npos; ) {
    size_t end = prop.find_first_of(',', start);
    std::string funcname;
    if (end == std::string::npos) {
      funcname = prop.substr(start, prop.length() - start);
      start = end;
    } else {
      funcname = prop.substr(start, end - start);
      start = end + 1;
    }

    if (!adb && funcname == "adb")
      continue;

    if (!supported_funcs.count(funcname)) {
      ALOGE("Function \"%s\" unsupported", funcname.c_str());
      return -1;
    }

    ALOGI("Adding %s", funcname.c_str());
    if (linkFunction(supported_funcs[funcname]().c_str(), i++))
      return -1;
  }

  return 0;
}

static int lookupAndSetVidPid(std::string prop) {
  if (!supported_compositions.count(prop)) {
    ALOGE("Composition \"%s\" unsupported", prop.c_str());
    return -1;
  }

  auto comp = supported_compositions[prop];
  std::string vid = comp.first;
  std::string pid = comp.second;

  if (setVidPid(vid.c_str(), pid.c_str()) != Status::SUCCESS)
    return -1;

  return 0;
}

static V1_0::Status validateAndSetVidPid(uint64_t functions) {
  V1_0::Status ret = Status::SUCCESS;
  switch (functions) {
    case static_cast<uint64_t>(GadgetFunction::ADB):
      ret = setVidPid("0x18d1", "0x4ee7");
      break;
    case static_cast<uint64_t>(GadgetFunction::MTP):
      ret = setVidPid("0x18d1", "0x4ee1");
      break;
    case GadgetFunction::ADB | GadgetFunction::MTP:
      ret = setVidPid("0x18d1", "0x4ee2");
      break;
    case static_cast<uint64_t>(GadgetFunction::RNDIS):
      ret = setVidPid("0x18d1", "0x4ee3");
      break;
    case GadgetFunction::ADB | GadgetFunction::RNDIS:
      ret = setVidPid("0x18d1", "0x4ee4");
      break;
    case static_cast<uint64_t>(GadgetFunction::PTP):
      ret = setVidPid("0x18d1", "0x4ee5");
      break;
    case GadgetFunction::ADB | GadgetFunction::PTP:
      ret = setVidPid("0x18d1", "0x4ee6");
      break;
    case static_cast<uint64_t>(GadgetFunction::MIDI):
      ret = setVidPid("0x18d1", "0x4ee8");
      break;
    case GadgetFunction::ADB | GadgetFunction::MIDI:
      ret = setVidPid("0x18d1", "0x4ee9");
      break;
    case static_cast<uint64_t>(GadgetFunction::ACCESSORY):
      ret = setVidPid("0x18d1", "0x2d00");
      break;
    case GadgetFunction::ADB | GadgetFunction::ACCESSORY:
      ret = setVidPid("0x18d1", "0x2d01");
      break;
    case static_cast<uint64_t>(GadgetFunction::AUDIO_SOURCE):
      ret = setVidPid("0x18d1", "0x2d02");
      break;
    case GadgetFunction::ADB | GadgetFunction::AUDIO_SOURCE:
      ret = setVidPid("0x18d1", "0x2d03");
      break;
    case GadgetFunction::ACCESSORY | GadgetFunction::AUDIO_SOURCE:
      ret = setVidPid("0x18d1", "0x2d04");
      break;
    case GadgetFunction::ADB | GadgetFunction::ACCESSORY |
	    GadgetFunction::AUDIO_SOURCE:
      ret = setVidPid("0x18d1", "0x2d05");
      break;
    default:
      ALOGE("Combination not supported");
      ret = Status::CONFIGURATION_NOT_SUPPORTED;
  }
  return ret;
}

static enum mdmType getModemType() {
  struct dirent* entry;
  enum mdmType mtype = INTERNAL;
  size_t pos_sda, pos_p, length;
  std::unique_ptr<DIR, int(*)(DIR*)> dir(opendir(ESOC_DEVICE_PATH), closedir);
  std::string esoc_name, path, soc_machine, esoc_dev_path = ESOC_DEVICE_PATH;

 /* On some platforms, /sys/bus/esoc/ director may not exists.*/
  if (dir == NULL)
      return mtype;

  while ((entry = readdir(dir.get())) != NULL) {
    if (entry->d_name[0] == '.')
      continue;
    path = esoc_dev_path + "/" + entry->d_name + "/esoc_name";
    if (ReadFileToString(path, &esoc_name)) {
      if (esoc_name.find("MDM") != std::string::npos ||
        esoc_name.find("SDX") != std::string::npos) {
        mtype = EXTERNAL;
        break;
      }
    }
  }
  if (ReadFileToString(SOC_MACHINE_PATH, &soc_machine)) {
    pos_sda = soc_machine.find("SDA");
    pos_p = soc_machine.find_last_of('P');
    length = soc_machine.length();
    if (pos_sda != std::string::npos || pos_p == length - 1) {
      mtype = mtype ? mtype : NONE;
      goto done;
    }
    if (mtype)
      mtype = INTERNAL_EXTERNAL;
  }
done:
  ALOGI("getModemType %d", mtype);
  return mtype;
}

V1_0::Status UsbGadget::setupFunctions(
    uint64_t functions, const sp<V1_0::IUsbGadgetCallback> &callback,
    uint64_t timeout) {
  bool ffsEnabled = false;
  int i = 0;
  const char *comp;
  enum mdmType mtype;
  std::string gadgetName = GetProperty(USB_CONTROLLER_PROP, "");
  std::string vendorProp = GetProperty(VENDOR_USB_PROP, GetProperty(PERSIST_VENDOR_USB_PROP, ""));

  if (gadgetName.empty()) {
    ALOGE("UDC name not defined");
    return Status::ERROR;
  }

  mtype = getModemType();
  if ((functions & GadgetFunction::RNDIS) != 0) {
    ALOGI("setCurrentUsbFunctions rndis");

    // for RNDIS+ADB, additional functions (diag, etc) depending on modem type
    if (functions & GadgetFunction::ADB) {
      switch (mtype) {
      case EXTERNAL:
      case INTERNAL_EXTERNAL:
        comp = "rndis,diag,diag_mdm,qdss,qdss_mdm,serial_cdev,dpl,adb";
        break;
      case INTERNAL:
        comp = "rndis,diag,qdss,serial_cdev,dpl,adb";
        break;
      default:
        comp = "rndis,adb";
        break;
      }

      ALOGI("RNDIS+ADB QC default composition: %s", comp);
      if (addFunctionsFromPropString(comp, i, false))
        return Status::ERROR;
      if (lookupAndSetVidPid(comp))
        return Status::ERROR;
    } else if (linkFunction(rndisFuncname().c_str(), i++)) {
      return Status::ERROR;
    }
  } else if (addGenericAndroidFunctions(&mMonitorFfs, functions, &ffsEnabled, &i)
              != Status::SUCCESS) {
    return Status::ERROR;
  }

  // override adb-only with additional QTI functions
  if (i == 0 && functions & GadgetFunction::ADB) {
    // If persist.vendor.usb.config or vendor.usb.config is set, look it up, parse it,
    // and link each function into the composition
    if (!vendorProp.empty()) {
      // tack on ADB to the property if not there, since we only arrive here
      // if "USB debugging enabled" is chosen which implies ADB
      if (vendorProp.find("adb") == std::string::npos)
        vendorProp += ",adb";

      ALOGI("setting composition from %s: %s", VENDOR_USB_PROP,
              vendorProp.c_str());
      if (!addFunctionsFromPropString(vendorProp, i, false))
        if (!lookupAndSetVidPid(vendorProp))
          goto enable_adb;

      // if failed, then use below default compositions
      unlinkFunctions(CONFIG_PATH);
      i = 0;
    }

    switch (mtype) {
    case EXTERNAL:
    case INTERNAL_EXTERNAL:
      comp = "diag,diag_mdm,qdss,qdss_mdm,serial_cdev,dpl,rmnet,adb";
      break;
    case INTERNAL:
      comp = "diag,serial_cdev,rmnet,dpl,qdss,adb";
      break;
    default:
      comp = "diag,adb";
      break;
    }

    ALOGI("enable QC default composition: %s", comp);
    if (addFunctionsFromPropString(comp, i, false))
      return Status::ERROR;
    if (lookupAndSetVidPid(comp))
      return Status::ERROR;
  }

enable_adb:
  // finally add ADB at the end if enabled
  if ((functions & GadgetFunction::ADB) != 0) {
    ffsEnabled = true;
    if (addAdb(&mMonitorFfs, &i) != Status::SUCCESS) return Status::ERROR;
  }

  // Pull up the gadget right away when there are no ffs functions.
  if (!ffsEnabled) {
    if (!WriteStringToFile(gadgetName, PULLUP_PATH)) return Status::ERROR;
    mCurrentUsbFunctionsApplied = true;
    if (callback)
      callback->setCurrentUsbFunctionsCb(functions, Status::SUCCESS);
    ALOGI("Gadget pullup without FFS fuctions");
    return Status::SUCCESS;
  }

  // Monitors the ffs paths to pull up the gadget when descriptors are written.
  // Also takes of the pulling up the gadget again if the userspace process
  // dies and restarts.
  mMonitorFfs.registerFunctionsAppliedCallback(
      [](bool functionsApplied, void *payload) {
        ((UsbGadget*)payload)->mCurrentUsbFunctionsApplied = functionsApplied;
      }, this);
  mMonitorFfs.startMonitor();

  ALOGI("Started monitor for FFS functions");

  if (callback) {
    bool gadgetPullup = mMonitorFfs.waitForPullUp(timeout);
    Return<void> ret = callback->setCurrentUsbFunctionsCb(
        functions, gadgetPullup ? Status::SUCCESS : Status::ERROR);
    if (!ret.isOk())
      ALOGE("setCurrentUsbFunctionsCb error %s", ret.description().c_str());
  }

  return Status::SUCCESS;
}

Return<void> UsbGadget::setCurrentUsbFunctions(
    uint64_t functions, const sp<V1_0::IUsbGadgetCallback> &callback,
    uint64_t timeout) {
  std::unique_lock<std::mutex> lk(mLockSetCurrentFunction);

  mCurrentUsbFunctions = functions;
  mCurrentUsbFunctionsApplied = false;

  // Unlink the gadget and stop the monitor if running.
  V1_0::Status status = tearDownGadget();
  if (status != Status::SUCCESS) {
    goto error;
  }

  // Leave the gadget pulled down to give time for the host to sense disconnect.
  usleep(kDisconnectWaitUs);

  if (functions == static_cast<uint64_t>(GadgetFunction::NONE)) {
    if (callback == NULL) return Void();
    Return<void> ret =
        callback->setCurrentUsbFunctionsCb(functions, Status::SUCCESS);
    if (!ret.isOk())
      ALOGE("Error while calling setCurrentUsbFunctionsCb %s",
            ret.description().c_str());
    return Void();
  }

  status = validateAndSetVidPid(functions);

  if (status != Status::SUCCESS) {
    goto error;
  }

  status = setupFunctions(functions, callback, timeout);
  if (status != Status::SUCCESS) {
    goto error;
  }

  ALOGI("Usb Gadget setcurrent functions called successfully");
  return Void();

error:
  ALOGI("Usb Gadget setcurrent functions failed");
  if (callback == NULL) return Void();
  Return<void> ret = callback->setCurrentUsbFunctionsCb(functions, status);
  if (!ret.isOk())
    ALOGE("Error while calling setCurrentUsbFunctionsCb %s",
          ret.description().c_str());
  return Void();
}
}  // namespace implementation
}  // namespace V1_1
}  // namespace gadget
}  // namespace usb
}  // namespace hardware
}  // namespace android

int main() {
  using android::base::GetProperty;
  using android::hardware::configureRpcThreadpool;
  using android::hardware::joinRpcThreadpool;
  using android::hardware::usb::gadget::V1_1::IUsbGadget;
  using android::hardware::usb::gadget::V1_1::implementation::UsbGadget;

  std::string gadgetName = GetProperty("persist.vendor.usb.controller",
      GetProperty(USB_CONTROLLER_PROP, ""));

  if (gadgetName.empty()) {
    ALOGE("UDC name not defined");
    return -1;
  }

  android::sp<IUsbGadget> service = new UsbGadget(gadgetName.c_str());

  configureRpcThreadpool(1, true /*callerWillJoin*/);
  android::status_t status = service->registerAsService();

  if (status != android::OK) {
    ALOGE("Cannot register USB Gadget HAL service");
    return 1;
  }

  ALOGI("QTI USB Gadget HAL Ready.");
  joinRpcThreadpool();
  // Under normal cases, execution will not reach this line.
  ALOGI("QTI USB Gadget HAL failed to join thread pool.");
  return 1;
}
