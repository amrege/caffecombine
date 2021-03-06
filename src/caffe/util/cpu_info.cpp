#include <glog/logging.h>
#include <set>
#include <vector>
#include "caffe/util/cpu_info.hpp"

namespace caffe {
namespace cpu {

Processor::Processor() {
  processor = 0;
  physicalId = 0;
  siblings = 0;
  coreId = 0;
  cpuCores = 0;
}

Collection::Collection() {
  processorSpeedMHz = 0;
  totalNumberOfSockets = 0;
  totalNumberOfCpuCores = 0;
  currentProcessor = NULL;

  processors.reserve(96);

  parseCpuFile("/proc/cpuinfo");
  collectBasicCpuInformation();
}

Collection &Collection::getSingleInstance() {
  static Collection collection;
  return collection;
}

unsigned Collection::getProcessorSpeedMHz() {
  Collection &collection = getSingleInstance();
  return collection.processorSpeedMHz;
}

unsigned Collection::getTotalNumberOfSockets() {
  Collection &collection = getSingleInstance();
  return collection.totalNumberOfSockets;
}

unsigned Collection::getTotalNumberOfCpuCores() {
  Collection &collection = getSingleInstance();
  return collection.totalNumberOfCpuCores;
}

unsigned Collection::getNumberOfProcessors() {
  Collection &collection = getSingleInstance();
  return collection.processors.size();
}

const Processor &Collection::getProcessor(unsigned processorId) {
  Collection &collection = getSingleInstance();
  return collection.processors[processorId];
}

void Collection::parseCpuFile(const char *fileName) {
  FILE *file = fopen(fileName, "rb");
  if (!file) {
    return;
  }

  parseCpuFileContent(file);

  fclose(file);
}

void Collection::parseCpuFileContent(FILE *file) {
  while (!feof(file)) {
    char lineBuffer[1024];
    if (!fgets(lineBuffer, sizeof(lineBuffer), file)) {
      break;
    }

    parseCpuFileLine(lineBuffer);
  }
}

void Collection::parseCpuFileLine(const char *lineBuffer) {
  int delimiterPosition = strcspn(lineBuffer, ":");

  if (lineBuffer[delimiterPosition] == '\0') {
    currentProcessor = NULL;
  } else {
    parseValue(lineBuffer, &lineBuffer[delimiterPosition + 2]);
  }
}

void Collection::parseValue(const char *fieldName, const char *valueString) {
  if (!currentProcessor) {
    appendNewProcessor();
  }

  if (beginsWith(fieldName, "processor")) {
    return parseInteger(&currentProcessor->processor, valueString);
  }

  if (beginsWith(fieldName, "physical id")) {
    return parseInteger(&currentProcessor->physicalId, valueString);
  }

  if (beginsWith(fieldName, "siblings")) {
    return parseInteger(&currentProcessor->siblings, valueString);
  }

  if (beginsWith(fieldName, "core id")) {
    return parseInteger(&currentProcessor->coreId, valueString);
  }

  if (beginsWith(fieldName, "cpu cores")) {
    return parseInteger(&currentProcessor->cpuCores, valueString);
  }

  if (beginsWith(fieldName, "model name")) {
    return extractProcessorSpeedFromModelName(valueString);
  }
}

void Collection::appendNewProcessor() {
  processors.push_back(Processor());
  currentProcessor = &processors.back();
}

bool Collection::beginsWith(const char *lineBuffer, const char *text) const {
  while (*text) {
    if (*(lineBuffer++) != *(text++)) {
      return false;
    }
  }

  return true;
}

void Collection::parseInteger(unsigned *value, const char *text) const {
  *value = atol(text);
}

/* Function extracts CPU speed from model name. If unit is not set it is
   assumed that values below 100 are specified in GHz, otherwise MHz */
void Collection::extractProcessorSpeedFromModelName(const char *text) {
  text = strstr(text, "@");
  if (!text || processorSpeedMHz) {
    return;
  }

  char *unit;
  double speed = strtod(&text[1], &unit);

  while (isspace(*unit)) {
    unit++;
  }

  bool isMHz = !strncmp(unit, "MHz", 3);
  bool isGHz = !strncmp(unit, "GHz", 3);
  bool isGHzPossible = (speed < 100);

  if (isGHz || (isGHzPossible && !isMHz)) {
    processorSpeedMHz = 1000 * speed + 0.5;
  } else {
    processorSpeedMHz = speed + 0.5;
  }
}

void Collection::collectBasicCpuInformation() {
  std::set<unsigned> uniquePhysicalId;
  std::vector<Processor>::iterator processor = processors.begin();
  for (; processor != processors.end(); processor++) {
    uniquePhysicalId.insert(processor->physicalId);
    updateCpuInformation(*processor, uniquePhysicalId.size());
  }
}

void Collection::updateCpuInformation(const Processor &processor,
    unsigned numberOfUniquePhysicalId) {
  if (totalNumberOfSockets == numberOfUniquePhysicalId) {
    return;
  }

  totalNumberOfSockets = numberOfUniquePhysicalId;
  totalNumberOfCpuCores += processor.cpuCores;
}

#ifdef _OPENMP

/* The OpenMpManager class is responsible for determining a set of all of
   available CPU cores and delegating each core to perform other tasks. The
   first of available cores is delegated for background threads, while other
   remaining cores are dedicated for OpenMP threads. Each OpenMP thread owns
   one core for exclusive use. The number of OpenMP threads is then limited
   to the number of available cores minus one. The amount of CPU cores may
   be limited by system eg. when numactl was used. */

#include <omp.h>
#include <sched.h>

static const char *openMpEnvVars[] = {
  "OMP_CANCELLATION", "OMP_DISPLAY_ENV", "OMP_DEFAULT_DEVICE", "OMP_DYNAMIC",
  "OMP_MAX_ACTIVE_LEVELS", "OMP_MAX_TASK_PRIORITY", "OMP_NESTED",
  "OMP_NUM_THREADS", "OMP_PROC_BIND", "OMP_PLACES", "OMP_STACKSIZE",
  "OMP_SCHEDULE", "OMP_THREAD_LIMIT", "OMP_WAIT_POLICY", "GOMP_CPU_AFFINITY",
  "GOMP_DEBUG", "GOMP_STACKSIZE", "GOMP_SPINCOUNT", "GOMP_RTEMS_THREAD_POOLS",
  "KMP_AFFINITY", "KMP_NUM_THREADS", "MIC_KMP_AFFINITY",
  "MIC_OMP_NUM_THREADS", "MIC_OMP_PROC_BIND", "PHI_KMP_AFFINITY",
  "PHI_OMP_NUM_THREADS", "PHI_KMP_PLACE_THREADS", "MKL_NUM_THREADS",
  "MKL_DYNAMIC", "MKL_DOMAIN_NUM_THREADS"
};

static const unsigned numberOfOpenMpEnvVars =
  sizeof(openMpEnvVars) / sizeof(openMpEnvVars[0]);

OpenMpManager::OpenMpManager() {
  getOpenMpEnvVars();
  getCurrentCpuSet();
  getCurrentCoreSet();
}

OpenMpManager &OpenMpManager::getInstance() {
  static OpenMpManager openMpManager;
  return openMpManager;
}

void OpenMpManager::setGpuEnabled() {
  OpenMpManager &openMpManager = getInstance();
  openMpManager.isGpuEnabled = true;
}

void OpenMpManager::setGpuDisabled() {
  OpenMpManager &openMpManager = getInstance();
  openMpManager.isGpuEnabled = false;
}

// Ideally bind given thread to secondary logical core, if
// only one thread exists then bind to primary one
void OpenMpManager::bindCurrentThreadToNonPrimaryCoreIfPossible() {
  OpenMpManager &openMpManager = getInstance();
  if (openMpManager.isThreadsBindAllowed()) {
    int totalNumberOfAvailableCores = CPU_COUNT(&openMpManager.currentCoreSet);
    int logicalCoreToBindTo = totalNumberOfAvailableCores > 1 ? 1 : 0;
    openMpManager.bindCurrentThreadToLogicalCoreCpus(logicalCoreToBindTo);
  }
}

void OpenMpManager::bindOpenMpThreads() {
  OpenMpManager &openMpManager = getInstance();

  if (!openMpManager.isThreadsBindAllowed())
    return;

  openMpManager.setOpenMpThreadNumberLimit();
  #pragma omp parallel
  {
    unsigned logicalCoreId = omp_get_thread_num();
    openMpManager.bindCurrentThreadToLogicalCoreCpu(logicalCoreId);
  }
}

void OpenMpManager::getOpenMpEnvVars() {
  isAnyOpenMpEnvVarSpecified = false;
  for (unsigned i = 0; i < numberOfOpenMpEnvVars; i++) {
    if (getenv(openMpEnvVars[i])) {
      isAnyOpenMpEnvVarSpecified = true;
    }
  }
}

void OpenMpManager::getCurrentCpuSet() {
  if (sched_getaffinity(0, sizeof(currentCpuSet), &currentCpuSet)) {
    getDefaultCpuSet(&currentCpuSet);
  }
}

void OpenMpManager::getDefaultCpuSet(cpu_set_t *defaultCpuSet) {
  CPU_ZERO(defaultCpuSet);
  unsigned numberOfProcessors = Collection::getNumberOfProcessors();
  for (int processorId = 0; processorId < numberOfProcessors; processorId++) {
    CPU_SET(processorId, defaultCpuSet);
  }
}

/* Function getCurrentCoreSet() fills currentCoreSet variable with a set of
   available CPUs, where only one CPU per core is chosen. When multiple CPUs
   of single core are used, function is selecting only first one of all
   available. */

void OpenMpManager::getCurrentCoreSet() {
  unsigned numberOfProcessors = Collection::getNumberOfProcessors();
  unsigned totalNumberOfCpuCores = Collection::getTotalNumberOfCpuCores();

  cpu_set_t usedCoreSet;
  CPU_ZERO(&usedCoreSet);
  CPU_ZERO(&currentCoreSet);

  for (int processorId = 0; processorId < numberOfProcessors; processorId++) {
    if (CPU_ISSET(processorId, &currentCpuSet)) {
      unsigned coreId = processorId % totalNumberOfCpuCores;
      if (!CPU_ISSET(coreId, &usedCoreSet)) {
        CPU_SET(coreId, &usedCoreSet);
        CPU_SET(processorId, &currentCoreSet);
      }
    }
  }
}

void OpenMpManager::selectAllCoreCpus(cpu_set_t *set, unsigned physicalCoreId) {
  unsigned numberOfProcessors = Collection::getNumberOfProcessors();
  unsigned totalNumberOfCpuCores = Collection::getTotalNumberOfCpuCores();

  int processorId = physicalCoreId % totalNumberOfCpuCores;
  while (processorId < numberOfProcessors) {
    if (CPU_ISSET(processorId, &currentCpuSet)) {
      CPU_SET(processorId, set);
    }

    processorId += totalNumberOfCpuCores;
  }
}

unsigned OpenMpManager::getPhysicalCoreId(unsigned logicalCoreId) {
  unsigned numberOfProcessors = Collection::getNumberOfProcessors();

  for (int processorId = 0; processorId < numberOfProcessors; processorId++) {
    if (CPU_ISSET(processorId, &currentCoreSet)) {
      if (!logicalCoreId--) {
        return processorId;
      }
    }
  }

  LOG(FATAL) << "This should never happen!";
  return 0;
}

bool OpenMpManager::isThreadsBindAllowed() {
  return !isAnyOpenMpEnvVarSpecified && !isGpuEnabled;
}

// Limit of threads to number of logical cores available
void OpenMpManager::setOpenMpThreadNumberLimit() {
  omp_set_num_threads(CPU_COUNT(&currentCoreSet));
}

void OpenMpManager::bindCurrentThreadToLogicalCoreCpu(unsigned logicalCoreId) {
  unsigned physicalCoreId = getPhysicalCoreId(logicalCoreId);

  cpu_set_t set;
  CPU_ZERO(&set);
  CPU_SET(physicalCoreId, &set);
  sched_setaffinity(0, sizeof(set), &set);
}

void OpenMpManager::bindCurrentThreadToLogicalCoreCpus(unsigned logicalCoreId) {
  unsigned physicalCoreId = getPhysicalCoreId(logicalCoreId);

  cpu_set_t set;
  CPU_ZERO(&set);
  selectAllCoreCpus(&set, physicalCoreId);
  sched_setaffinity(0, sizeof(set), &set);
}

void OpenMpManager::printVerboseInformation() {
  OpenMpManager &openMpManager = getInstance();

  LOG(INFO) << "Processor speed [MHz]: "
    << Collection::getProcessorSpeedMHz();

  LOG(INFO) << "Total number of sockets: "
    << Collection::getTotalNumberOfSockets();

  LOG(INFO) << "Total number of CPU cores: "
    << Collection::getTotalNumberOfCpuCores();

  LOG(INFO) << "Total number of processors: "
    << Collection::getNumberOfProcessors();

  LOG(INFO) << "GPU is used: "
    << (openMpManager.isGpuEnabled ? "yes" : "no");

  LOG(INFO) << "OpenMP environmental variables are specified: "
    << (openMpManager.isAnyOpenMpEnvVarSpecified ? "yes" : "no");

  LOG(INFO) << "OpenMP thread bind allowed: "
    << (openMpManager.isThreadsBindAllowed() ? "yes" : "no");

  LOG(INFO) << "Number of OpenMP threads: "
    << omp_get_max_threads();
}

#endif  // _OPENMP

}  // namespace cpu
}  // namespace caffe
