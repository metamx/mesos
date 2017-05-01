// Licensed to the Apache Software Foundation (ASF) under one
// or more contributor license agreements.  See the NOTICE file
// distributed with this work for additional information
// regarding copyright ownership.  The ASF licenses this file
// to you under the Apache License, Version 2.0 (the
// "License"); you may not use this file except in compliance
// with the License.  You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include <string>

#include <mesos/secret/resolver.hpp>

#include <process/future.hpp>
#include <process/gtest.hpp>

#include <stout/gtest.hpp>

#include "tests/mesos.hpp"

using process::Future;
using process::Owned;

using mesos::internal::slave::Fetcher;
using mesos::internal::slave::MesosContainerizer;

using mesos::master::detector::MasterDetector;

using std::string;

namespace mesos {
namespace internal {
namespace tests {

const char SECRET_VALUE[] = "password";
const char SECRET_ENV_NAME[] = "My_SeCrEt";

class EnvironmentSecretIsolatorTest : public MesosTest {};


// This test verifies that the environment secrets are resolved when launching a
// task.
TEST_F(EnvironmentSecretIsolatorTest, ResolveSecret)
{
  Try<Owned<cluster::Master>> master = StartMaster();
  ASSERT_SOME(master);

  mesos::internal::slave::Flags flags = CreateSlaveFlags();

  Fetcher fetcher;
  Try<SecretResolver*> secretResolver = SecretResolver::create();
  EXPECT_SOME(secretResolver);

  Try<MesosContainerizer*> containerizer =
    MesosContainerizer::create(flags, false, &fetcher, secretResolver.get());
  EXPECT_SOME(containerizer);

  Owned<MasterDetector> detector = master.get()->createDetector();
  Try<Owned<cluster::Slave>> slave =
    StartSlave(detector.get(), containerizer.get());
  ASSERT_SOME(slave);

  MockScheduler sched;
  MesosSchedulerDriver driver(
      &sched, DEFAULT_FRAMEWORK_INFO, master.get()->pid, DEFAULT_CREDENTIAL);

  EXPECT_CALL(sched, registered(&driver, _, _));

  Future<std::vector<Offer>> offers;
  EXPECT_CALL(sched, resourceOffers(&driver, _))
    .WillOnce(FutureArg<1>(&offers))
    .WillRepeatedly(Return()); // Ignore subsequent offers.

  driver.start();

  AWAIT_READY(offers);
  EXPECT_NE(0u, offers.get().size());

  const string commandString = strings::format(
      "env; test \"$%s\" = \"%s\"",
      string(SECRET_ENV_NAME),
      string(SECRET_VALUE));

  CommandInfo command;
  command.set_value(commandString);

  // Request a secret.
  // TODO(kapil): Update createEnvironment() to support secrets.
  mesos::Environment::Variable *env =
    command.mutable_environment()->add_variables();
  env->set_name(SECRET_ENV_NAME);
  env->set_type(mesos::Environment::Variable::SECRET);

  mesos::Secret* secret = env->mutable_secret();
  secret->set_type(Secret::VALUE);
  secret->mutable_value()->set_data(SECRET_VALUE);

  TaskInfo task = createTask(
      offers.get()[0].slave_id(),
      Resources::parse("cpus:0.1;mem:32").get(),
      command);

  // NOTE: Successful tasks will output two status updates.
  Future<TaskStatus> statusRunning;
  Future<TaskStatus> statusFinished;
  EXPECT_CALL(sched, statusUpdate(&driver, _))
    .WillOnce(FutureArg<1>(&statusRunning))
    .WillOnce(FutureArg<1>(&statusFinished));

  driver.launchTasks(offers.get()[0].id(), {task});

  AWAIT_READY(statusRunning);
  EXPECT_EQ(TASK_RUNNING, statusRunning.get().state());
  AWAIT_READY(statusFinished);
  EXPECT_EQ(TASK_FINISHED, statusFinished.get().state());

  driver.stop();
  driver.join();
}

} // namespace tests {
} // namespace internal {
} // namespace mesos {