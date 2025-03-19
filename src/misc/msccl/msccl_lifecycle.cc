/*************************************************************************
 * Copyright (c) Microsoft Corporation.
 * Licensed under the MIT License.
 ************************************************************************/

#include <atomic>
#include <map>
#include <mutex>
#include <set>

#include <dirent.h>
#include <dlfcn.h>
#include <error.h>
#include <link.h>

#include "alloc.h"
#include "checks.h"
#include "comm.h"
#include "transport.h"
#include "graph/topo.h"

#include "msccl/msccl_lifecycle.h"
#include "msccl/msccl_parser.h"
#include "msccl/msccl_setup.h"
#include "msccl/msccl_status.h"

NCCL_PARAM(MscclEnabled, "MSCCL_ENABLE", 1);
static std::atomic<bool> mscclInitialized;
static std::mutex mscclLifecycleMutex;

int getEnvInt(const char* env, int64_t deftVal) {
  char* str = getenv(env);
  int64_t value = deftVal;
  if (str && strlen(str) > 0) {
    errno = 0;
    value = strtoll(str, nullptr, 0);
  }
  return value;
}

bool mscclEnabled() {
  return ncclParamMscclEnabled();
}

void mscclSetIsCallerFlag() {
  mscclGetThreadLocalStatus().mscclIsCallerFlag = true;
}

void mscclClearIsCallerFlag() {
  mscclGetThreadLocalStatus().mscclIsCallerFlag = false;
}

bool mscclIsCaller() {
  return mscclGetThreadLocalStatus().mscclIsCallerFlag;
}

bool mscclAvailable() {
  return mscclEnabled() && mscclInitialized.load(std::memory_order_acquire);
}

static bool mscclCommCompatible(ncclComm_t comm) {
  std::map<uint64_t, std::set<uint64_t>> hostHashToPidHashes;
  for (int i = 0; i < comm->nRanks; i++) {
    uint64_t hostHash = comm->peerInfo[i].hostHash;
    uint64_t pidHash = comm->peerInfo[i].pidHash;
    if (hostHashToPidHashes.find(hostHash) != hostHashToPidHashes.end()) {
      auto& pidHashSet = hostHashToPidHashes[hostHash];
      if (pidHashSet.find(pidHash) != pidHashSet.end()) {
        return false;
      }
    }
    hostHashToPidHashes[hostHash].insert(pidHash);
  }
  return true;
}

static const char* mscclSchedulerPathEnv = "MSCCL_SCHEDULER";
static const char* mscclSchedulerDefaultPath = "libmsccl-scheduler.so";
static const char* mscclAlgoDirEnv = "MSCCL_ALGO_DIR";
static const char* mscclAlgoDefaultDir = "msccl-algorithms";
extern "C" bool mscclUnitTestMode() __attribute__((__weak__));
static const char* mscclUnitTestAlgoDefaultDir = "msccl-unit-test-algorithms";
static const char* mscclAlgoShareDirPath = "../share/nccl/msccl-algorithms";
static const char* mscclUnitTestAlgoShareDirPath = "../share/nccl/msccl-unit-test-algorithms";
static const char* mscclPackageInstalledAlgoShareDirPath = "/usr/share/nccl/msccl-algorithms";
static const char* mscclUnitTestPackageInstalledAlgoShareDirPath = "/usr/share/nccl/msccl-unit-test-algorithms";

static ncclResult_t mscclInternalSchedulerInit(ncclComm_t comm, int* numChannelsRequired) {
  static bool mscclAlgoMetaLoaded = false;
  mscclStatus& status = mscclGetStatus();

  *numChannelsRequired = 0;
  // Query numChannelsRequired from loaded algorithm metas
  if (mscclAlgoMetaLoaded) {
    for (auto& m : status.algoMetas) {
      if (comm->nRanks == m.nRanks) {
        *numChannelsRequired = std::max(*numChannelsRequired, m.nChannels);
      }
    }
    return ncclSuccess;
  }

  const char* mscclAlgoDir = getenv(mscclAlgoDirEnv);
  const char* mscclAlgoShareDir = nullptr;
  const char* mscclPackageInstalledAlgoShareDir = nullptr;
  std::string mscclAlgoDirStr;
  std::string mscclAlgoShareDirStr;
  std::string mscclPackageInstalledAlgoShareDirStr;
  const char *fullDirPath = nullptr;
  if (mscclAlgoDir == nullptr) {
    // Try to find default algorithm directory based on librccl.so path
    Dl_info dl_info;
    struct link_map *link_map_ptr = nullptr;
    if (!dladdr1((void *)mscclInternalSchedulerInit, &dl_info, (void **)&link_map_ptr, RTLD_DL_LINKMAP)) {
      WARN("MSCCL Internal Scheduler: dladdr1 failed");
      return ncclInvalidUsage;
    }
    std::string selfLibPath = link_map_ptr->l_name;
    mscclAlgoDirStr = selfLibPath.substr(0, selfLibPath.find_last_of("/\\") + 1);
    mscclAlgoDirStr += (mscclUnitTestMode && mscclUnitTestMode()) ? mscclUnitTestAlgoDefaultDir : mscclAlgoDefaultDir;
    mscclAlgoDir = mscclAlgoDirStr.c_str();
    // Get share Directory Paths
    mscclAlgoShareDirStr = selfLibPath.substr(0, selfLibPath.find_last_of("/\\") + 1);
    mscclAlgoShareDirStr += (mscclUnitTestMode && mscclUnitTestMode()) ? mscclUnitTestAlgoShareDirPath : mscclAlgoShareDirPath;
    mscclAlgoShareDir = mscclAlgoShareDirStr.c_str();

    mscclPackageInstalledAlgoShareDirStr = (mscclUnitTestMode && mscclUnitTestMode()) ? mscclUnitTestPackageInstalledAlgoShareDirPath : mscclPackageInstalledAlgoShareDirPath;
    mscclPackageInstalledAlgoShareDir = mscclPackageInstalledAlgoShareDirStr.c_str();
  }
  INFO(NCCL_INIT, "MSCCL: Internal Scheduler will use %s as algorithm directory and %s as share algorithm directory and %s as package installed share algorithm directory ", mscclAlgoDir, mscclAlgoShareDir, mscclPackageInstalledAlgoShareDir);
  struct dirent *entry = nullptr;
  DIR *dp = nullptr;
  dp = opendir(mscclAlgoDir);
  if (dp == nullptr) {
    //Try to find the algorithm directory under share folder based on libnccl.so path
    dp = opendir(mscclAlgoShareDir);
    if (dp == nullptr) {
      //Try to find the algorithm directory under /usr/share folder which is package installed share algorithm directory
      dp = opendir(mscclPackageInstalledAlgoShareDir);
      if (dp == nullptr) {
        WARN("MSCCL Internal Scheduler: open algorithm in share directory %s failed", mscclPackageInstalledAlgoShareDir);
        return ncclInvalidUsage;
      }
      fullDirPath = mscclPackageInstalledAlgoShareDir;
    }
    else {
      fullDirPath = mscclAlgoShareDir;
    }
  } else {
    fullDirPath = mscclAlgoDir;
  }
  INFO(NCCL_INIT, "Using MSCCL Algo files from %s", fullDirPath);
  std::set<std::string> sortedFullPaths;
  while ((entry = readdir(dp))) {
    if (entry->d_type != DT_LNK && entry->d_type != DT_REG) {
      continue;
    }
    std::string fullPath = fullDirPath;
    fullPath += "/";
    fullPath += entry->d_name;
    sortedFullPaths.insert(fullPath);
  }
  for (auto& fullPath : sortedFullPaths) {
    status.algoMetas.emplace_back();
    NCCLCHECK(mscclGetAlgoMetaFromXmlFile(fullPath.c_str(), &(status.algoMetas.back())));
    if (status.algoMetas.back().nRanks == comm->nRanks) {
      *numChannelsRequired = std::max(*numChannelsRequired, status.algoMetas.back().nChannels);
    }
  }
  if (closedir(dp)) {
    WARN("MSCCL Internal Scheduler: closedir failed, error %d", errno);
    return ncclInvalidUsage;
  }
  status.rankToAlgoHandles.resize(status.algoMetas.size());
  mscclAlgoMetaLoaded = true;
  return ncclSuccess;
}

ncclResult_t mscclSchedulerInit(ncclComm_t comm, int* numChannelsRequired) {
  *numChannelsRequired = 0;
  comm->mscclCompatible = mscclCommCompatible(comm);
  if (!comm->mscclCompatible) {
    return ncclSuccess;
  }

  std::lock_guard<std::mutex> lock(mscclLifecycleMutex);

  mscclStatus& status = mscclGetStatus();
  bool useInternalScheduler = false;

  const char* mscclSchedulerPath = getenv(mscclSchedulerPathEnv);
  if (mscclSchedulerPath) {
    status.mscclSchedulerLib = dlopen(mscclSchedulerPath, RTLD_NOW | RTLD_LOCAL);
  } else {
    status.mscclSchedulerLib = dlopen(mscclSchedulerDefaultPath, RTLD_NOW | RTLD_LOCAL);
  }
  if (status.mscclSchedulerLib == nullptr) {
    INFO(NCCL_INIT, "MSCCL: No external scheduler found, using internal implementation");
    useInternalScheduler = true;
  } else {
    status.mscclSchedulerPtr = (mscclSchedulerInterface *)dlsym(status.mscclSchedulerLib, "mscclScheduler");
    if (status.mscclSchedulerPtr == nullptr) {
      INFO(NCCL_INIT, "MSCCL: Failed to find mscclScheduler symbol, using internal implementation");
      useInternalScheduler = true;
    }
  }

  if (useInternalScheduler) {
    NCCLCHECK(mscclInternalSchedulerInit(comm, numChannelsRequired));
  } else {
    NCCLCHECK(status.mscclSchedulerPtr->init());
    *numChannelsRequired = MAXCHANNELS;
  }

  return ncclSuccess;
}

ncclResult_t mscclInit(ncclComm_t comm) {
  if (comm->intraRanks > 1) {
    mscclInitialized.store(false, std::memory_order_release);
    INFO(NCCL_INIT, "MSCCL doesn't support multiple GPUs in one process and is not available");
    return ncclSuccess;
  }
  // Always initialize thread local status
  mscclThreadLocalStatus threadLocalStatus = mscclGetThreadLocalStatus();
  threadLocalStatus.groupStatus = mscclNoGroup;
  threadLocalStatus.groupDepth = 0;
  threadLocalStatus.captureId = ULLONG_MAX;
  threadLocalStatus.captureStatus = mscclNoCapture;

  {
    std::lock_guard<std::mutex> lock(mscclLifecycleMutex);

    mscclStatus& status = mscclGetStatus();

    // freeAlgoHandles, needsProxy and needsFence are initialized globally once and before algorithm pre-processing and connection
    if (!mscclInitialized.load(std::memory_order_acquire)) {
      status.freeAlgoHandles.resize(MSCCL_MAX_NUM_ALGOS);
      for (int i = 0; i < MSCCL_MAX_NUM_ALGOS; i++) {
        status.freeAlgoHandles[i] = MSCCL_MAX_NUM_ALGOS - i - 1;
      }
      status.needsProxy = false;
      status.needsFence = false;
    }

    // Pre-process all algorithms for internal scheduler and for different comms.
    // This is a temp fix to bypass the issue that stream cannot be synchronized during HIP graph capturing,
    // should use dynamic loading approach after the issue is fixed.
    if (comm->mscclCompatible && !status.mscclSchedulerPtr) {
      for (size_t i = 0; i < status.algoMetas.size(); i++) {
        auto &m = status.algoMetas[i];
        if (m.nRanks == comm->nRanks) {
          // Load algorithms
          if (status.rankToAlgoHandles[i].find(comm->rank) == status.rankToAlgoHandles[i].end()) {
            NCCLCHECK(mscclLoadAlgo(m.filePath.c_str(), &(status.rankToAlgoHandles[i][comm->rank]), comm->rank));
          }
          // Connect algorithms
          mscclAlgoHandle_t mscclAlgoHandle = status.rankToAlgoHandles[i][comm->rank];
          if (status.connectedAlgos[comm].find(mscclAlgoHandle) == status.connectedAlgos[comm].end()) {
            NCCLCHECK(mscclSetupConnections(status.hostAlgos[mscclAlgoHandle], comm));
            status.connectedAlgos[comm].insert(mscclAlgoHandle);
          }
        }
      }
    }

    if (mscclInitialized.load(std::memory_order_acquire)) {
      return ncclSuccess;
    }

    status.scratchBuffer = nullptr;
    status.scratchBufferSize = 0;
    status.workIndex = 1;
    NCCLCHECK(ncclCudaCalloc(&status.syncFlags, MSCCL_MAX_NUM_THREAD_BLOCKS));
    status.lastStream = nullptr;

    mscclInitialized.store(true, std::memory_order_release);
  }


  size_t maxLocalSizeBytes = 0, mscclMaxLocalSizeBytes = 0;
  cudaDeviceGetLimit(&maxLocalSizeBytes, cudaLimitStackSize);
  NCCLCHECK(mscclInitKernelsForDevice(comm->cudaArch, &mscclMaxLocalSizeBytes));
  if (mscclMaxLocalSizeBytes > maxLocalSizeBytes && getEnvInt("NCCL_SET_STACK_SIZE", 0) == 1) {
    // Reset the maximum kernel stack size of all msccl kernels to avoid
    // a CUDA memory reconfig on load (c.f. NVSHMEM issue)
    TRACE(NCCL_INIT, "Msccl Resetting cudaLimitStackSize to %zi", mscclMaxLocalSizeBytes);
    CUDACHECKIGNORE(cudaDeviceSetLimit(cudaLimitStackSize, mscclMaxLocalSizeBytes));
  }

  INFO(NCCL_INIT, "MSCCL: Initialization finished");
  return ncclSuccess;
}

ncclResult_t mscclGroupStart() {
  mscclThreadLocalStatus& threadLocalStatus = mscclGetThreadLocalStatus();
  threadLocalStatus.groupDepth++;
  if (threadLocalStatus.groupStatus == mscclNoGroup) {
    threadLocalStatus.groupStatus = mscclGroupSupportedOp;
  }
  return ncclSuccess;
}

static ncclResult_t mscclInternalSchedulerSelectAlgo(struct mscclSchedulerParam* param) {
  mscclStatus& status = mscclGetStatus();
  param->scheduled = false;

  // Current MSCCL doesn't support pre/post op
  if (param->op >= ncclAvg) {
    return ncclSuccess;
  }
  //nccl imp: ncclRedOp_t netOp = info->op == ncclAvg || info->op >= ncclNumOps ? ncclSum : info->op;

  // Whether the algorithm is in-place
  bool isInPlace = false;
  if (param->func == mscclFuncReduce ||
      param->func == mscclFuncBroadcast ||
      param->func == mscclFuncAllReduce ||
      param->func == mscclFuncAllToAll) {
    isInPlace = param->sendBuff == param->recvBuff;
  } else if (param->func == mscclFuncAllGather ||
             param->func == mscclFuncGather) {
    isInPlace = (char*)param->sendBuff == (char*)param->recvBuff + param->rank * param->count * ncclTypeSize(param->dataType);
  } else if (param->func == mscclFuncReduceScatter ||
             param->func == mscclFuncScatter) {
    isInPlace = (char*)param->recvBuff == (char*)param->sendBuff + param->rank * param->count * ncclTypeSize(param->dataType);
  }

  // Search suitable algorithms
  for (size_t i = 0; i < status.algoMetas.size(); i++) {
    auto &m = status.algoMetas[i];
    size_t nBytes = param->count * ncclTypeSize(param->dataType) * m.sizeMultiplier;
    bool msgSizeIsValid =
      param->count > 0 && ((param->count * m.sizeMultiplier) % m.nChunksPerLoop) == 0 &&
      nBytes >= m.minBytes && (m.maxBytes == 0 || nBytes <= m.maxBytes);
    if (msgSizeIsValid &&
        m.nRanks == param->nRanks &&
        m.func == param->func &&
        (isInPlace ? m.inPlace : m.outOfPlace)) {
      param->handle = status.rankToAlgoHandles[i][param->rank];
      param->scheduled = true;
      TRACE("MSCCL: SchedulerSelectAlgo: Algo %s is selected", m.filePath.c_str());
      return ncclSuccess;
    }
  }

  return ncclSuccess;
}

static ncclResult_t mscclSchedulerSelectAlgo(struct mscclSavedSchedulerParam* param) {
  mscclStatus& status = mscclGetStatus();
  if (status.mscclSchedulerPtr) {
    NCCLCHECK(status.mscclSchedulerPtr->selectAlgo(&(param->p)));
  } else {
    NCCLCHECK(mscclInternalSchedulerSelectAlgo(&(param->p)));
  }
  return ncclSuccess;
}

static ncclResult_t mscclSetSavedSchedulerParam(
  const void* sendBuff, const size_t sendCounts[], const size_t sDisPls[],
  void* recvBuff, const size_t recvCounts[], const size_t rDisPls[],
  size_t count, ncclDataType_t dataType, int root, int peer, ncclRedOp_t op,
  mscclFunc_t func, ncclComm_t comm, cudaStream_t stream,
  struct mscclSavedSchedulerParam* param) {
  param->p.sendBuff = sendBuff;
  param->p.sendCounts = sendCounts;
  param->p.sDisPls = sDisPls;
  param->p.recvBuff = recvBuff;
  param->p.recvCounts = recvCounts;
  param->p.rDisPls = rDisPls;
  param->p.count = count;
  param->p.dataType = dataType;
  param->p.root = root;
  param->p.peer = peer;
  param->p.op = op;
  param->p.func = func;
  param->p.rank = comm->rank;
  param->p.nRanks = comm->nRanks;
  param->comm = comm;
  param->stream = stream;
  return ncclSuccess;
}

static ncclResult_t mscclSaveCountsAndDispls(struct mscclSavedSchedulerParam* param) {
  if (param->p.sendCounts) {
    param->savedSendCounts.assign(param->p.sendCounts, param->p.sendCounts + param->p.nRanks);
    param->p.sendCounts = param->savedSendCounts.data();
    param->savedSDisPls.assign(param->p.sDisPls, param->p.sDisPls + param->p.nRanks);
    param->p.sDisPls = param->savedSDisPls.data();
    param->savedRecvCounts.assign(param->p.recvCounts, param->p.recvCounts + param->p.nRanks);
    param->p.recvCounts = param->savedRecvCounts.data();
    param->savedRDisPls.assign(param->p.rDisPls, param->p.rDisPls + param->p.nRanks);
    param->p.rDisPls = param->savedRDisPls.data();
  }
  return ncclSuccess;
}

static ncclResult_t mscclRunSavedParams() {
  mscclThreadLocalStatus& threadLocalStatus = mscclGetThreadLocalStatus();

  for (size_t i = 0; i < threadLocalStatus.savedSchedulerParams.size(); i++) {
    auto& param = threadLocalStatus.savedSchedulerParams[i];
    NCCLCHECK(mscclRunAlgo(
      param.p.sendBuff, param.p.sendCounts, param.p.sDisPls,
      param.p.recvBuff, param.p.recvCounts, param.p.rDisPls,
      param.p.count, param.p.dataType, param.p.root, param.p.peer, param.p.op, param.p.handle, param.comm, param.stream));
  }
  threadLocalStatus.savedSchedulerParams.clear();
  return ncclSuccess;
}

static ncclResult_t mscclFallBackSavedParams() {
  mscclThreadLocalStatus& threadLocalStatus = mscclGetThreadLocalStatus();
  mscclSetIsCallerFlag();
  for (auto& param : threadLocalStatus.savedSchedulerParams) {
    switch (param.p.func) {
      case mscclFuncReduce:
        NCCLCHECK(ncclReduce(param.p.sendBuff, param.p.recvBuff, param.p.count, param.p.dataType,
          param.p.op, param.p.root, param.comm, param.stream));
        break;
      case mscclFuncBroadcast:
        NCCLCHECK(ncclBroadcast(param.p.sendBuff, param.p.recvBuff, param.p.count, param.p.dataType,
          param.p.root, param.comm, param.stream));
        break;
      case mscclFuncAllReduce:
        NCCLCHECK(ncclAllReduce(param.p.sendBuff, param.p.recvBuff, param.p.count, param.p.dataType,
          param.p.op, param.comm, param.stream));
        break;
      case mscclFuncReduceScatter:
        NCCLCHECK(ncclReduceScatter(param.p.sendBuff, param.p.recvBuff, param.p.count, param.p.dataType,
          param.p.op, param.comm, param.stream));
        break;
      case mscclFuncAllGather:
        NCCLCHECK(ncclAllGather(param.p.sendBuff, param.p.recvBuff, param.p.count, param.p.dataType,
          param.comm, param.stream));
        break;
      case mscclFuncSend:
        NCCLCHECK(ncclSend(param.p.sendBuff, param.p.count, param.p.dataType,
          param.p.peer, param.comm, param.stream));
        break;
      case mscclFuncRecv:
        NCCLCHECK(ncclRecv(param.p.recvBuff, param.p.count, param.p.dataType,
          param.p.peer, param.comm, param.stream));
        break;
      case mscclFuncAllToAll:
        NCCLCHECK(ncclAllToAll(param.p.sendBuff, param.p.recvBuff, param.p.count, param.p.dataType,
          param.comm, param.stream));
        break;
      default:
        WARN("Invalid MSCCL function type in saved parameter");
        return ncclInvalidUsage;
    }
  }
  mscclClearIsCallerFlag();
  threadLocalStatus.savedSchedulerParams.clear();
  return ncclSuccess;
}

ncclResult_t mscclEnqueueCheck(
    const void* sendBuff, const size_t sendCounts[], const size_t sDisPls[],
    void* recvBuff, const size_t recvCounts[], const size_t rDisPls[],
    size_t count, ncclDataType_t dataType, int root, int peer, ncclRedOp_t op,
    mscclFunc_t func, ncclComm_t comm, cudaStream_t stream) {
  mscclThreadLocalStatus& threadLocalStatus = mscclGetThreadLocalStatus();
  threadLocalStatus.savedSchedulerParams.push_back({});
  NCCLCHECK(mscclSetSavedSchedulerParam(
    sendBuff, sendCounts, sDisPls, recvBuff, recvCounts, rDisPls,
    count, dataType, root, peer, op, func, comm, stream,
    &threadLocalStatus.savedSchedulerParams.back()));

  switch (threadLocalStatus.groupStatus) {
    INFO(NCCL_ALL, "----print groupStatus.");
    case mscclNoGroup:
      INFO(NCCL_ALL, "----print mscclNoGroup. comm->mscclCompatible: %d", comm->mscclCompatible);
      if (comm->mscclCompatible) {
            NCCLCHECK(mscclSchedulerSelectAlgo(&threadLocalStatus.savedSchedulerParams.back()));
            if (threadLocalStatus.savedSchedulerParams.back().p.scheduled) {
              NCCLCHECK(mscclRunSavedParams());
              break;
            }
        }
      NCCLCHECK(mscclFallBackSavedParams());
      break;
    case mscclGroupSupportedOp:
      INFO(NCCL_ALL, "----print mscclGroupSupportedOp. comm->mscclCompatible: %d", comm->mscclCompatible);
      if (comm->mscclCompatible) {
          NCCLCHECK(mscclSchedulerSelectAlgo(&threadLocalStatus.savedSchedulerParams.back()));
          if (threadLocalStatus.savedSchedulerParams.back().p.scheduled) {
            // Only save counts and displs when there is suitable MSCCL algorithm for this
            NCCLCHECK(mscclSaveCountsAndDispls(&threadLocalStatus.savedSchedulerParams.back()));
            break;
          }
        }
      threadLocalStatus.groupStatus = mscclGroupUnsupportedOp;
      NCCLCHECK(mscclFallBackSavedParams());
    case mscclGroupUnsupportedOp:
      INFO(NCCL_ALL, "----print mscclGroupUnsupportedOp. ");
      NCCLCHECK(mscclFallBackSavedParams());
      break;
    default:
      return ncclInvalidUsage;
  }
  return ncclSuccess;
}

ncclResult_t mscclGroupEnd() {
  mscclThreadLocalStatus& threadLocalStatus = mscclGetThreadLocalStatus();
  threadLocalStatus.groupDepth--;
  if (threadLocalStatus.groupDepth == 0) {
    if (threadLocalStatus.groupStatus == mscclGroupSupportedOp) {
      NCCLCHECK(mscclRunSavedParams());
    }
    threadLocalStatus.groupStatus = mscclNoGroup;
  }
  return ncclSuccess;
}

static ncclResult_t mscclInternalSchedulerTeardown() {
  ncclResult_t ret = ncclSuccess, tmpRet = ncclSuccess;
  mscclStatus& status = mscclGetStatus();
  for (auto &m : status.rankToAlgoHandles) {
    for (auto &p : m) {
      tmpRet = mscclUnloadAlgo(p.second);
      if (ret == ncclSuccess) {
        ret = tmpRet;
      }
    }
  }
  status.algoMetas.clear();
  status.rankToAlgoHandles.clear();
  return ret;
}

ncclResult_t mscclTeardown() {
  // Always teardown thread local status
  mscclThreadLocalStatus threadLocalStatus = mscclGetThreadLocalStatus();
  threadLocalStatus.savedSchedulerParams.clear();

  {
    std::lock_guard<std::mutex> lock(mscclLifecycleMutex);

    if (!mscclInitialized.load(std::memory_order_acquire)) {
      return ncclSuccess;
    }
    mscclStatus& status = mscclGetStatus();
    for (auto &p : status.hostAlgos) {
      free(p.second);
      status.freeAlgoHandles.push_back(p.first);
    }
    for (auto &p : status.devAlgos) {
      NCCLCHECK(ncclCudaFree(p.second));
    }
    NCCLCHECK(ncclCudaFree(status.scratchBuffer));
    NCCLCHECK(ncclCudaFree(status.syncFlags));
    status.hostAlgos.clear();
    status.devAlgos.clear();
    status.freeAlgoHandles.clear();
    status.scratchBuffer = nullptr;
    status.scratchBufferSize = 0;
    status.connectedAlgos.clear();
    if (status.mscclSchedulerPtr) {
      NCCLCHECK(status.mscclSchedulerPtr->teardown());
      status.mscclSchedulerPtr = nullptr;
      dlclose(status.mscclSchedulerLib);
      status.mscclSchedulerLib = nullptr;
    } else {
      NCCLCHECK(mscclInternalSchedulerTeardown());
    }
    mscclInitialized.store(false, std::memory_order_release);
  }

  INFO(NCCL_INIT, "MSCCL: Teardown finished");
  return ncclSuccess;
}
