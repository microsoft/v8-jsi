#pragma once

#include <jsi/instrumentation.h>
#include <v8.h>

namespace v8runtime {

class V8Instrumentation : public facebook::jsi::Instrumentation {
 public:
  explicit V8Instrumentation(v8::Isolate *isolate);

  std::string getRecordedGCStats() override;
  std::unordered_map<std::string, int64_t> getHeapInfo(bool includeExpensive) override;
  void collectGarbage(std::string cause) override;
  void startTrackingHeapObjectStackTraces(
      std::function<
          void(uint64_t lastSeenObjectID, std::chrono::microseconds timestamp, std::vector<HeapStatsUpdate> stats)>
          fragmentCallback) override;
  void stopTrackingHeapObjectStackTraces() override;
  void startHeapSampling(size_t samplingInterval) override;
  void stopHeapSampling(std::ostream &os) override;
#if JSI_VERSION >= 13
  void createSnapshotToFile(const std::string &path, const HeapSnapshotOptions &options = {false}) override;
  void createSnapshotToStream(std::ostream &os, const HeapSnapshotOptions &options = {false}) override;
#else
  void createSnapshotToFile(const std::string &path) override;
  void createSnapshotToStream(std::ostream &os) override;
#endif
  std::string flushAndDisableBridgeTrafficTrace() override;
  void writeBasicBlockProfileTraceToFile(const std::string &fileName) const override;
  void dumpProfilerSymbolsToFile(const std::string &fileName) const override;

 private:
  void createSnapshotToStreamImpl(std::ostream &os, bool captureNumericValue = false);

 private:
  v8::Isolate *isolate_;
};

} // namespace v8runtime
