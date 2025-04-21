#include "V8Instrumentation.h"

#include <fstream>
#include <sstream>
#include <chrono>
#include "v8-profiler.h"
#include "v8-inspector.h"

namespace v8runtime {

V8Instrumentation::V8Instrumentation(v8::Isolate* isolate) 
  : isolate_(isolate) {
}

V8Instrumentation::~V8Instrumentation() {
  if (isTrackingHeapObjects_) {
    stopTrackingHeapObjectStackTraces();
  }
}

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
        std::vector<HeapStatsUpdate> stats)> fragmentCallback) {
  fragmentCallback_ = std::move(fragmentCallback);
  isTrackingHeapObjects_ = true;
  
  // V8 doesn't provide a direct API for tracking heap object stack traces in this way
  // We could use the heap profiler to track allocations, but would need custom code
  // to match the expected format
}

void V8Instrumentation::stopTrackingHeapObjectStackTraces() {
  isTrackingHeapObjects_ = false;
  fragmentCallback_ = nullptr;
}

void V8Instrumentation::startHeapSampling(size_t samplingInterval) {
  v8::HeapProfiler* heap_profiler = isolate_->GetHeapProfiler();
  if (heap_profiler) {
    heap_profiler->StartSamplingHeapProfiler(static_cast<int>(samplingInterval), 100000);
  }
}

void V8Instrumentation::stopHeapSampling(std::ostream& os) {
  v8::HeapProfiler* heap_profiler = isolate_->GetHeapProfiler();
  if (heap_profiler) {
    v8::HandleScope handle_scope(isolate_);
    std::unique_ptr<v8::AllocationProfile> profile(heap_profiler->GetAllocationProfile());
    if (profile) {
      // Serialize the allocation profile to the output stream
      os << "{\n  \"allocations\": [\n";
      for (const auto& sample : profile->GetSamples()) {
        os << "    {\"size\": " << sample.size << ", \"count\": " << sample.count << "},\n";
      }
      os << "  ]\n}";
    } else {
      os << "{}";  // Empty JSON if no profile is available
    }
    heap_profiler->StopSamplingHeapProfiler();
  }
}

#if JSI_VERSION >= 13
void V8Instrumentation::createSnapshotToFile(
    const std::string& path,
    const HeapSnapshotOptions& options) {
  v8::HeapProfiler* heap_profiler = isolate_->GetHeapProfiler();
  if (heap_profiler) {
    v8::HeapProfiler::HeapSnapshotOptions snapshot_options;
    snapshot_options.numerics_mode = options.captureNumericValue ? v8::HeapProfiler::NumericsMode::kExposeNumericValues : v8::HeapProfiler::NumericsMode::kHideNumericValues;
    const v8::HeapSnapshot* snapshot = heap_profiler->TakeHeapSnapshot(snapshot_options);
    if (snapshot) {
      std::ofstream file(path);
      class FileOutputStream : public v8::OutputStream {
       public:
        explicit FileOutputStream(std::ofstream& file) : file_(file) {}
        WriteResult WriteAsciiChunk(char* data, int size) override {
          file_.write(data, size);
          return kContinue;
        }
        void EndOfStream() override {}
       private:
        std::ofstream& file_;
      } stream(file);
      snapshot->Serialize(&stream, v8::HeapSnapshot::kJSON);
      file.close();
      const_cast<v8::HeapSnapshot*>(snapshot)->Delete();
    }
  }
}

void V8Instrumentation::createSnapshotToStream(
    std::ostream& os,
    const HeapSnapshotOptions& options) {
  v8::HeapProfiler* heap_profiler = isolate_->GetHeapProfiler();
  if (heap_profiler) {
    const v8::HeapSnapshot* snapshot = heap_profiler->TakeHeapSnapshot();
    if (snapshot) {
      class StreamOutputStream : public v8::OutputStream {
       public:
        explicit StreamOutputStream(std::ostream& os) : os_(os) {}
        WriteResult WriteAsciiChunk(char* data, int size) override {
          os_.write(data, size);
          return kContinue;
        }
        void EndOfStream() override {}
       private:
        std::ostream& os_;
      } stream(os);
      snapshot->Serialize(&stream, v8::HeapSnapshot::kJSON);
      const_cast<v8::HeapSnapshot*>(snapshot)->Delete();
    }
  }
}
#else
void V8Instrumentation::createSnapshotToFile(const std::string& path) {
  v8::HeapProfiler* heap_profiler = isolate_->GetHeapProfiler();
  if (heap_profiler) {
    const v8::HeapSnapshot* snapshot = heap_profiler->TakeHeapSnapshot();
    if (snapshot) {
      std::ofstream file(path);
      const_cast<v8::HeapSnapshot*>(snapshot)->Serialize(&file, v8::HeapSnapshot::kJSON);
      file.close();
      const_cast<v8::HeapSnapshot*>(snapshot)->Delete();
    }
  }
}

void V8Instrumentation::createSnapshotToStream(std::ostream& os) {
  v8::HeapProfiler* heap_profiler = isolate_->GetHeapProfiler();
  if (heap_profiler) {
    const v8::HeapSnapshot* snapshot = heap_profiler->TakeHeapSnapshot();
    if (snapshot) {
      const_cast<v8::HeapSnapshot*>(snapshot)->Serialize(&os, v8::HeapSnapshot::kJSON);
      const_cast<v8::HeapSnapshot*>(snapshot)->Delete();
    }
  }
}
#endif

std::string V8Instrumentation::flushAndDisableBridgeTrafficTrace() {
  // V8 doesn't have a built-in bridge traffic trace
  return "";
}

void V8Instrumentation::writeBasicBlockProfileTraceToFile(const std::string& fileName) const {
  // This would require the CPU profiler, which would need additional setup
  // We'll implement a basic version that writes a placeholder file
  std::ofstream file(fileName);
  file << "{}";  // Empty JSON profile
  file.close();
}

void V8Instrumentation::dumpProfilerSymbolsToFile(const std::string& fileName) const {
  // Write a simple symbols file
  std::ofstream file(fileName);
  file << "// V8 profiler symbols\n";
  file.close();
}

} // namespace v8runtime
