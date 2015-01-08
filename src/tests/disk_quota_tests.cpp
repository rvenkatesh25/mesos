/**
 * Licensed to the Apache Software Foundation (ASF) under one
 * or more contributor license agreements.  See the NOTICE file
 * distributed with this work for additional information
 * regarding copyright ownership.  The ASF licenses this file
 * to you under the Apache License, Version 2.0 (the
 * "License"); you may not use this file except in compliance
 * with the License.  You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include <string>
#include <vector>

#include <gmock/gmock.h>

#include <mesos/mesos.hpp>
#include <mesos/resources.hpp>

#include <process/gtest.hpp>
#include <process/pid.hpp>

#include <stout/fs.hpp>
#include <stout/gtest.hpp>
#include <stout/os.hpp>
#include <stout/path.hpp>

#include "master/master.hpp"

#include "slave/flags.hpp"
#include "slave/slave.hpp"

#include "slave/containerizer/isolators/posix/disk.hpp"

#include "tests/mesos.hpp"
#include "tests/utils.hpp"

using namespace process;

using namespace mesos;
using namespace mesos::internal;
using namespace mesos::internal::tests;

using std::string;
using std::vector;

using testing::_;
using testing::Return;

using mesos::internal::master::Master;

using mesos::internal::slave::DiskUsageCollector;
using mesos::internal::slave::Slave;


class DiskUsageCollectorTest : public TemporaryDirectoryTest {};


// TODO(jieyu): Consider adding a test to verify that minimal check
// interval is honored.


// This test verifies the usage of a file.
TEST_F(DiskUsageCollectorTest, File)
{
  // Create a file and write 8K bytes.
  string path = path::join(os::getcwd(), "file");
  ASSERT_SOME(os::write(path, string(Kilobytes(8).bytes(), 'x')));

  DiskUsageCollector collector(Milliseconds(1));

  Future<Bytes> usage = collector.usage(path);
  AWAIT_READY(usage);

  // NOTE: A typical file system needs more disk space to keep meta
  // data. So the check here is not a strict equal-to check.
  EXPECT_GE(usage.get(), Kilobytes(8));
}


// This test verifies the usage of a directory.
TEST_F(DiskUsageCollectorTest, Directory)
{
  // Create files and subdirectories in the working directory.
  string file1 = path::join(os::getcwd(), "file1");
  string file2 = path::join(os::getcwd(), "file2");

  string dir = path::join(os::getcwd(), "dir");
  string file3 = path::join(dir, "file3");
  string file4 = path::join(dir, "file4");

  ASSERT_SOME(os::mkdir(dir));

  ASSERT_SOME(os::write(file1, string(Kilobytes(8).bytes(), 'x')));
  ASSERT_SOME(os::write(file2, string(Kilobytes(4).bytes(), 'y')));
  ASSERT_SOME(os::write(file3, string(Kilobytes(1).bytes(), 'z')));
  ASSERT_SOME(os::write(file4, string(Kilobytes(2).bytes(), '1')));

  DiskUsageCollector collector(Milliseconds(1));

  Future<Bytes> usage = collector.usage(os::getcwd());
  AWAIT_READY(usage);

  EXPECT_GE(usage.get(), Kilobytes(15));
}


// This test verifies that symbolic links are not followed.
TEST_F(DiskUsageCollectorTest, SymbolicLink)
{
  string file = path::join(os::getcwd(), "file");
  ASSERT_SOME(os::write(file, string(Kilobytes(8).bytes(), 'x')));

  // Create a symbolic link to the current directory.
  string link = path::join(os::getcwd(), "link");
  ASSERT_SOME(fs::symlink(os::getcwd(), link));

  DiskUsageCollector collector(Milliseconds(1));

  Future<Bytes> usage1 = collector.usage(os::getcwd());
  Future<Bytes> usage2 = collector.usage(link);

  AWAIT_READY(usage1);
  EXPECT_GE(usage1.get(), Kilobytes(8));
  EXPECT_LT(usage1.get(), Kilobytes(16));

  AWAIT_READY(usage2);
  EXPECT_LT(usage2.get(), Kilobytes(8));
}


class DiskQuotaTest : public MesosTest {};


// This test verifies that the container will be killed if the disk
// usage exceeds its quota.
TEST_F(DiskQuotaTest, DiskUsageExceedsQuota)
{
  Try<PID<Master>> master = StartMaster();
  ASSERT_SOME(master);

  slave::Flags flags = CreateSlaveFlags();
  flags.isolation = "posix/cpu,posix/mem,posix/disk";
  flags.disk_quota_check_interval = Milliseconds(1);

  Try<PID<Slave>> slave = StartSlave(flags);
  ASSERT_SOME(slave);

  MockScheduler sched;
  MesosSchedulerDriver driver(
      &sched, DEFAULT_FRAMEWORK_INFO, master.get(), DEFAULT_CREDENTIAL);

  EXPECT_CALL(sched, registered(&driver, _, _));

  Future<vector<Offer>> offers;
  EXPECT_CALL(sched, resourceOffers(&driver, _))
    .WillOnce(FutureArg<1>(&offers))
    .WillRepeatedly(Return());        // Ignore subsequent offers.

  driver.start();

  AWAIT_READY(offers);
  EXPECT_FALSE(offers.get().empty());

  Offer offer = offers.get()[0];

  // Create a task which requests 1MB disk, but actually uses more
  // than 2MB disk.
  TaskInfo task = createTask(
      offer.slave_id(),
      Resources::parse("cpus:1;mem:128;disk:1").get(),
      "dd if=/dev/zero of=file bs=1048576 count=2 && sleep 1000");

  Future<TaskStatus> status1;
  Future<TaskStatus> status2;
  EXPECT_CALL(sched, statusUpdate(&driver, _))
    .WillOnce(FutureArg<1>(&status1))
    .WillOnce(FutureArg<1>(&status2));

  driver.launchTasks(offer.id(), {task});

  AWAIT_READY(status1);
  EXPECT_EQ(task.task_id(), status1.get().task_id());
  EXPECT_EQ(TASK_RUNNING, status1.get().state());

  AWAIT_READY(status2);
  EXPECT_EQ(task.task_id(), status2.get().task_id());
  EXPECT_EQ(TASK_FAILED, status2.get().state());

  driver.stop();
  driver.join();

  Shutdown();
}
