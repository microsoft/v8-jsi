#include "V8Instrumentation.h"

#include <chrono>
#include <fstream>
#include <sstream>
#include "v8-inspector.h"
#include "v8-profiler.h"

namespace v8runtime {

V8Instrumentation::V8Instrumentation(v8::Isolate *isolate) : isolate_(isolate) {}

std::string V8Instrumentation::getRecordedGCStats() {
  v8::HeapStatistics heapStats;
  isolate_->GetHeapStatistics(&heapStats);

  std::stringstream json;
  json << "{\n";
  json << "  \"totalHeapSize\": " << heapStats.total_heap_size() << ",\n";
  json << "  \"totalHeapSizeExecutable\": " << heapStats.total_heap_size_executable() << ",\n";
  json << "  \"totalPhysicalSize\": " << heapStats.total_physical_size() << ",\n";
  json << "  \"totalAvailableSize\": " << heapStats.total_available_size() << ",\n";
  json << "  \"totalGlobalHandlesSize\": " << heapStats.total_global_handles_size() << ",\n";
  json << "  \"usedGlobalHandlesSize\": " << heapStats.used_global_handles_size() << ",\n";
  json << "  \"usedHeapSize\": " << heapStats.used_heap_size() << ",\n";
  json << "  \"heapSizeLimit\": " << heapStats.heap_size_limit() << ",\n";
  json << "  \"mallocedMemory\": " << heapStats.malloced_memory() << ",\n";
  json << "  \"externalMemory\": " << heapStats.external_memory() << ",\n";
  json << "  \"peakMallocedMemory\": " << heapStats.peak_malloced_memory() << ",\n";
  json << "  \"doesZapGarbage\": " << (heapStats.does_zap_garbage() != 0) << ",\n";
  json << "  \"numberOfNativeContexts\": " << heapStats.number_of_native_contexts() << ",\n";
  json << "  \"numberOfDetachedContexts\": " << heapStats.number_of_detached_contexts() << "\n";
  json << "}";
  return json.str();
}

std::unordered_map<std::string, int64_t> V8Instrumentation::getHeapInfo(bool /*includeExpensive*/) {
  std::unordered_map<std::string, int64_t> result;
  v8::HeapStatistics heapStats;
  isolate_->GetHeapStatistics(&heapStats);

  result["totalHeapSize"] = heapStats.total_heap_size();
  result["totalHeapSizeExecutable"] = heapStats.total_heap_size_executable();
  result["totalPhysicalSize"] = heapStats.total_physical_size();
  result["totalAvailableSize"] = heapStats.total_available_size();
  result["totalGlobalHandlesSize"] = heapStats.total_global_handles_size();
  result["usedGlobalHandlesSize"] = heapStats.used_global_handles_size();
  result["usedHeapSize"] = heapStats.used_heap_size();
  result["heapSizeLimit"] = heapStats.heap_size_limit();
  result["mallocedMemory"] = heapStats.malloced_memory();
  result["externalMemory"] = heapStats.external_memory();
  result["peakMallocedMemory"] = heapStats.peak_malloced_memory();
  result["doesZapGarbage"] = heapStats.does_zap_garbage();
  result["numberOfNativeContexts"] = heapStats.number_of_native_contexts();
  result["numberOfDetachedContexts"] = heapStats.number_of_detached_contexts();

  return result;
}

void V8Instrumentation::collectGarbage(std::string cause) {
  // Request V8 to perform a full GC
  isolate_->LowMemoryNotification();
}

void V8Instrumentation::startTrackingHeapObjectStackTraces(
    std::function<void(
        uint64_t lastSeenObjectID,
        std::chrono::microseconds timestamp,
        std::vector<HeapStatsUpdate> stats)> /*fragmentCallback*/) {}

void V8Instrumentation::stopTrackingHeapObjectStackTraces() {}

void V8Instrumentation::startHeapSampling(size_t samplingInterval) {
  v8::HeapProfiler *heap_profiler = isolate_->GetHeapProfiler();
  if (!heap_profiler)
    return;
  heap_profiler->StartSamplingHeapProfiler(static_cast<uint64_t>(samplingInterval), 64 /*stack_depth*/);
}

void V8Instrumentation::stopHeapSampling(std::ostream &os) {
  v8::HeapProfiler *heap_profiler = isolate_->GetHeapProfiler();
  if (!heap_profiler)
    return;

  v8::HandleScope handle_scope(isolate_);
  std::unique_ptr<v8::AllocationProfile> profile(heap_profiler->GetAllocationProfile());
  if (profile) {
    // Serialize the allocation profile to the output stream
    os << "{\n  \"allocations\": [\n";
    for (const auto &sample : profile->GetSamples()) {
      os << "    {\"size\": " << sample.size << ", \"count\": " << sample.count << "},\n";
    }
    os << "  ]\n}";
  } else {
    os << "{}"; // Empty JSON if no profile is available
  }
  heap_profiler->StopSamplingHeapProfiler();
}

#if JSI_VERSION >= 13

void V8Instrumentation::createSnapshotToFile(const std::string &path, const HeapSnapshotOptions &options) {
  std::ofstream file_stream(path);
  createSnapshotToStreamImpl(file_stream, options.captureNumericValue);
  file_stream.close();
}

void V8Instrumentation::createSnapshotToStream(std::ostream &os, const HeapSnapshotOptions &options) {
  createSnapshotToStreamImpl(os, options.captureNumericValue);
}

#else

void V8Instrumentation::createSnapshotToFile(const std::string &path) {
  std::ofstream file_stream(path);
  createSnapshotToStreamImpl(file_stream);
  file_stream.close();
}

void V8Instrumentation::createSnapshotToStream(std::ostream &os) {
  createSnapshotToStreamImpl(os);
}

#endif

void V8Instrumentation::createSnapshotToStreamImpl(std::ostream &os, bool captureNumericValue) {
  v8::HeapProfiler *heap_profiler = isolate_->GetHeapProfiler();
  if (heap_profiler == nullptr)
    return;
  v8::HeapProfiler::HeapSnapshotOptions snapshot_options{};
  snapshot_options.numerics_mode = captureNumericValue ? v8::HeapProfiler::NumericsMode::kExposeNumericValues
                                                       : v8::HeapProfiler::NumericsMode::kHideNumericValues;
  const v8::HeapSnapshot *snapshot = heap_profiler->TakeHeapSnapshot(snapshot_options);
  if (snapshot == nullptr)
    return;

  class StdStreamOutputStream : public v8::OutputStream {
   public:
    explicit StdStreamOutputStream(std::ostream &os) : os_(os) {}
    ~StdStreamOutputStream() override {
      os_.flush();
    }

    WriteResult WriteAsciiChunk(char *data, int size) override {
      os_.write(data, size);
      return kContinue;
    }

    void EndOfStream() override {
      os_.flush();
    }

   private:
    std::ostream &os_;
  } stream(os);
  snapshot->Serialize(&stream, v8::HeapSnapshot::kJSON);
  const_cast<v8::HeapSnapshot *>(snapshot)->Delete();
}

std::string V8Instrumentation::flushAndDisableBridgeTrafficTrace() {
  std::abort();
}

void V8Instrumentation::writeBasicBlockProfileTraceToFile(const std::string &fileName) const {
  std::abort();
}

void V8Instrumentation::dumpProfilerSymbolsToFile(const std::string &fileName) const {
  std::abort();
}

} // namespace v8runtime
