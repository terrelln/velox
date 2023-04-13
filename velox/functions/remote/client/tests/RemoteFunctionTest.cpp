/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include <folly/SocketAddress.h>
#include <folly/init/Init.h>
#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include "common/network/PortUtil.h"
#include "velox/common/base/Exceptions.h"
#include "velox/functions/Registerer.h"
#include "velox/functions/prestosql/Arithmetic.h"
#include "velox/functions/prestosql/CheckedArithmetic.h"
#include "velox/functions/prestosql/StringFunctions.h"
#include "velox/functions/prestosql/tests/utils/FunctionBaseTest.h"
#include "velox/functions/remote/client/Remote.h"
#include "velox/functions/remote/if/gen-cpp2/RemoteFunctionService.h"
#include "velox/functions/remote/server/RemoteFunctionService.h"
#include "velox/serializers/PrestoSerializer.h"

using ::apache::thrift::ThriftServer;
using ::facebook::velox::test::assertEqualVectors;

namespace facebook::velox::functions {
namespace {

class RemoteFunctionTest : public functions::test::FunctionBaseTest {
 public:
  RemoteFunctionTest() {
    serializer::presto::PrestoVectorSerde::registerVectorSerde();
    initializeServer();
    registerRemoteFunctions();
  }

  // Registers a few remote functions to be used in this test.
  void registerRemoteFunctions() {
    // Register the remote adapter.
    auto plusSignatures = {exec::FunctionSignatureBuilder()
                               .returnType("bigint")
                               .argumentType("bigint")
                               .argumentType("bigint")
                               .build()};
    registerRemoteFunction("remote_plus", plusSignatures, {"::1", port_});
    registerRemoteFunction("remote_wrong_port", plusSignatures, {"::1", 1});

    auto divSignatures = {exec::FunctionSignatureBuilder()
                              .returnType("double")
                              .argumentType("double")
                              .argumentType("double")
                              .build()};
    registerRemoteFunction("remote_divide", divSignatures, {"::1", port_});

    auto substrSignatures = {exec::FunctionSignatureBuilder()
                                 .returnType("varchar")
                                 .argumentType("varchar")
                                 .argumentType("integer")
                                 .build()};
    registerRemoteFunction("remote_substr", substrSignatures, {"::1", port_});

    // Registers the actual function under a different prefix. This is only
    // needed for tests since the thrift service runs in the same process.
    registerFunction<PlusFunction, int64_t, int64_t, int64_t>(
        {remotePrefix_ + ".remote_plus"});
    registerFunction<CheckedDivideFunction, double, double, double>(
        {remotePrefix_ + ".remote_divide"});
    registerFunction<SubstrFunction, Varchar, Varchar, int32_t>(
        {remotePrefix_ + ".remote_substr"});
  }

  void initializeServer() {
    auto handler =
        std::make_shared<RemoteFunctionServiceHandler>(remotePrefix_);
    server_ = std::make_shared<ThriftServer>();

    port_ = network::getFreePort();
    server_->setPort(port_);
    server_->setInterface(handler);

    thread_ = std::make_unique<std::thread>([&] { server_->serve(); });
    VELOX_CHECK(waitForRunning(), "Unable to initialize thrift server.");
    LOG(INFO) << "Thrift server is up and running in local port " << port_;
  }

  ~RemoteFunctionTest() {
    server_->stop();
    thread_->join();
    LOG(INFO) << "Thrift server stopped.";
  }

 private:
  // Loop until the server is up and running.
  bool waitForRunning() {
    for (size_t i = 0; i < 10; ++i) {
      if (server_->getServerStatus() == ThriftServer::ServerStatus::RUNNING) {
        return true;
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    return false;
  }

  uint16_t port_;
  std::shared_ptr<apache::thrift::ThriftServer> server_;
  std::unique_ptr<std::thread> thread_;

  const std::string remotePrefix_{"remote"};
};

TEST_F(RemoteFunctionTest, simple) {
  auto inputVector = makeFlatVector<int64_t>({1, 2, 3, 4, 5});
  auto results = evaluate<SimpleVector<int64_t>>(
      "remote_plus(c0, c0)", makeRowVector({inputVector}));

  auto expected = makeFlatVector<int64_t>({2, 4, 6, 8, 10});
  assertEqualVectors(expected, results);
}

TEST_F(RemoteFunctionTest, string) {
  auto inputVector =
      makeFlatVector<StringView>({"hello", "my", "remote", "world"});
  auto inputVector1 = makeFlatVector<int32_t>({2, 1, 3, 5});
  auto results = evaluate<SimpleVector<StringView>>(
      "remote_substr(c0, c1)", makeRowVector({inputVector, inputVector1}));

  auto expected = makeFlatVector<StringView>({"ello", "my", "mote", "d"});
  assertEqualVectors(expected, results);
}

/*
// Test case to exercise throwOnError, once it is implemented.
TEST_F(RemoteFunctionTest, remoteException) {
  auto input1 = makeFlatVector<double>({1, 2, 3});
  auto input2 = makeFlatVector<double>({1, 0, 3});

  EXPECT_THROW(
      evaluate<SimpleVector<double>>(
          "remote_divide(c0, c1)", makeRowVector({input1, input2})),
      VeloxUserError);

  auto results = evaluate<SimpleVector<double>>(
      "try(remote_divide(c0, c1))", makeRowVector({input1, input2}));

  auto expected = makeNullableFlatVector<double>({1, std::nullopt, 1});
  assertEqualVectors(expected, results);
}
*/

TEST_F(RemoteFunctionTest, connectionError) {
  auto inputVector = makeFlatVector<int64_t>({1, 2, 3, 4, 5});
  auto func = [&]() {
    evaluate<SimpleVector<int64_t>>(
        "remote_wrong_port(c0, c0)", makeRowVector({inputVector}));
  };

  // Check it throw and that the exception has the "connection refused"
  // substring.
  EXPECT_THROW(func(), VeloxUserError);
  try {
    func();
  } catch (const VeloxUserError& e) {
    EXPECT_THAT(e.message(), testing::HasSubstr("Connection refused"));
  }
}

} // namespace
} // namespace facebook::velox::functions

int main(int argc, char** argv) {
  testing::InitGoogleTest(&argc, argv);
  folly::init(&argc, &argv, false);
  return RUN_ALL_TESTS();
}
