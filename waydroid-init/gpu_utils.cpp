/*
 * Copyright (C) 2026 The WayDroid-ATV Project
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#define _XOPEN_SOURCE 500
#include <iostream>
#include <fstream>
#include <algorithm>
#include <format>
#include <string>
#include <system_error>
#include <cerrno>
#include <cstring>
#include <filesystem>
#include <unistd.h>
#include <sys/sysmacros.h>

#include "log.h"
#include "gpu_utils.h"
#include "minigbm_intel_ids.h"

void GpuUtils::getKernelDriver() {
    std::error_code       error;
    std::filesystem::path devicePath(std::format("/sys/dev/char/{}:{}/device", major(this->renderNodeInfo.st_rdev), minor(this->renderNodeInfo.st_rdev))),
                          driverPath(std::filesystem::read_symlink(devicePath / "driver", error));

    if (driverPath.empty()) {
        Log::err("Failed to read GPU driver name: {}", error.message());
        return;
    }

    // Read device ID if exist
    if (std::filesystem::exists(devicePath / "device")) {
        std::string   deviceId;
        std::ifstream deviceFile(devicePath / "device");

        if (deviceFile.is_open()) {
            std::getline(deviceFile, deviceId);
            this->gpuDeviceId = std::stoi(deviceId, 0, 16);
        }
    }

    this->gpuKernelDriverName = driverPath.filename();

    // virtio normalize check
    if (this->gpuKernelDriverName == "virtio-pci" ||
        this->gpuKernelDriverName == "virtio_pci" ||
        this->gpuKernelDriverName == "virtio-mmio" ||
        this->gpuKernelDriverName == "virtio_mmio" ||
        this->gpuKernelDriverName == "virtio_gpu") {
        this->gpuKernelDriverName = driverPath.replace_filename("virtio-gpu")
    }
    
    Log::info("GPU kernel driver: {}", this->gpuKernelDriverName);
}

float GpuUtils::getIntelGpuGeneration() const {
    std::string   line;
    std::ifstream capabilities;
    size_t        delim;

    capabilities.open(std::format("/sys/kernel/debug/dri/{}/i915_capabilities", minor(this->renderNodeInfo.st_rdev)), std::ios::in);

    if (!capabilities.is_open()) {
        Log::err("Failed to read GPU capabilities: {}", strerror(errno));
        return -errno;
    }

    while (std::getline(capabilities, line)) {
        if ((delim = line.find(':')) == std::string::npos) {
            continue;
        } else if (line.substr(0, delim) == "graphics version") {
            return std::stof(line.substr(delim + 2));
        }
    }

    return -1;
}

std::string GpuUtils::getGralloc() const {
    if (gpuKernelDriverName != "i915" && gpuKernelDriverName != "xe") return driverInfo.preferredGbm;

    // Only use minigbm_intel on supported Intel GPUs
    if (std::find(std::begin(minigbm_intel_ids), std::end(minigbm_intel_ids), gpuDeviceId) == std::end(minigbm_intel_ids)) {
        return "minigbm_gbm_mesa";
    } else {
        return driverInfo.preferredGbm;
    }
}

GpuUtils::GpuUtils(const std::string &renderNode, bool swRendering) {
    float gpuGeneration;

    if (swRendering) goto swFallback;

    this->renderNode = renderNode;

    if (stat(renderNode.c_str(), &this->renderNodeInfo) == -1) {
        Log::err("Failed to access GPU render node: {}", strerror(errno));
        goto swFallback;
    }

    if (!S_ISCHR(this->renderNodeInfo.st_mode)) {
        Log::err("GPU render node is not a character device");
        goto swFallback;
    }

    getKernelDriver();

    gpuGeneration = gpuKernelDriverName == "i915" ? getIntelGpuGeneration() : 0;

    for (const auto &e : driverInfoList) {
        if (e.driverName == gpuKernelDriverName) {
            if (e.minGeneration && e.minGeneration > gpuGeneration) continue;

            this->driverInfo = e;
            return;
        }
    }

    goto swFallback;

swFallback:
    Log::err("GPU driver unsupported or unrecognized, fallback to software rendering");
    this->driverInfo = driverInfoList.back();
}
