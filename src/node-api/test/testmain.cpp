// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.
#include <gtest/gtest.h>

#include <filesystem>
#include <string_view>
#include "child_process.h"

namespace fs = std::filesystem;

namespace node_api_tests {

// Forward declaration
int EvaluateJSFile(int argc, char** argv);

struct NodeApiTestFixture : ::testing::Test {
  explicit NodeApiTestFixture(fs::path testProcess, fs::path jsFilePath)
      : m_testProcess(std::move(testProcess)),
        m_jsFilePath(std::move(jsFilePath)) {}
  static void SetUpTestSuite() {}
  static void TearDownTestSuite() {}
  void SetUp() override {}
  void TearDown() override {}

  void TestBody() override {
    ProcessResult result =
        spawnSync(m_testProcess.string(), {m_jsFilePath.string()});
    if (result.status == 0) {
      return;
    }
    if (!result.std_error.empty()) {
      std::stringstream errorStream(result.std_error);
      std::vector<std::string> errorLines;
      std::string line;
      while (std::getline(errorStream, line)) {
        if (!line.empty() && line[line.size() - 1] == '\r') {
          line.erase(line.size() - 1, std::string::npos);
        }
        errorLines.push_back(line);
      }
      if (errorLines.size() >= 3) {
        std::string file = errorLines[0].find("file:") == 0
                               ? errorLines[0].substr(std::size("file:") - 1)
                               : "<Unknown>";
        int line = errorLines[1].find("line:") == 0
                       ? std::stoi(errorLines[1].substr(std::size("line:") - 1))
                       : 0;
        std::string message = errorLines[2];
        std::stringstream details;
        for (size_t i = 3; i < errorLines.size(); i++) {
          details << errorLines[i] << std::endl;
        }
        GTEST_MESSAGE_AT_(file.c_str(),
                          line,
                          message.c_str(),
                          ::testing::TestPartResult::kFatalFailure)
            << details.str();
        return;
      }
    }
    ASSERT_EQ(result.status, 0);
  }

 private:
  fs::path m_testProcess;
  fs::path m_jsFilePath;
};

std::string SanitizeName(const std::string& name) {
  std::string sanitized = name;
  std::replace(sanitized.begin(), sanitized.end(), '-', '_');
  return sanitized;
}

void RegisterNodeApiTests(const char* exePathStr) {
  fs::path exePath = fs::path(exePathStr).replace_filename("node_lite.exe");
  fs::path rootJsPath = fs::path(exePath).replace_filename("test-js-files");
  for (const fs::directory_entry& dir_entry :
       fs::recursive_directory_iterator(rootJsPath)) {
    if (dir_entry.is_regular_file() && dir_entry.path().extension() == ".js") {
      fs::path jsFilePath = dir_entry.path();
      std::string testSuiteName = SanitizeName(
          jsFilePath.parent_path().parent_path().filename().string());
      std::string testName =
          SanitizeName(jsFilePath.parent_path().filename().string() + "/" +
                       jsFilePath.filename().string());
      ::testing::RegisterTest(testSuiteName.c_str(),
                              testName.c_str(),
                              nullptr,
                              nullptr,
                              jsFilePath.string().c_str(),
                              1,
                              [exePath, jsFilePath]() {
                                return new NodeApiTestFixture(exePath,
                                                              jsFilePath);
                              });
    }
  }
}

}  // namespace node_api_tests

int main(int argc, char** argv) {
  ::testing::InitGoogleTest(&argc, argv);
  node_api_tests::RegisterNodeApiTests(argv[0]);
  return RUN_ALL_TESTS();
}
