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
// limitations under the License

#include <string>
#include <tuple>
#include <vector>

#include <process/collect.hpp>
#include <process/io.hpp>
#include <process/subprocess.hpp>

#include <stout/bytes.hpp>
#include <stout/check.hpp>
#include <stout/error.hpp>
#include <stout/foreach.hpp>
#include <stout/numify.hpp>
#include <stout/os.hpp>
#include <stout/path.hpp>
#include <stout/strings.hpp>

#include <stout/os/exists.hpp>
#include <stout/os/shell.hpp>

#include "hdfs/hdfs.hpp"

using namespace process;

using std::string;
using std::vector;


struct CommandResult
{
  Option<int> status;
  string out;
  string err;
};


static Future<CommandResult> result(const Subprocess& s)
{
  CHECK_SOME(s.out());
  CHECK_SOME(s.err());

  return await(
      s.status(),
      io::read(s.out().get()),
      io::read(s.err().get()))
    .then([](const std::tuple<
        Future<Option<int>>,
        Future<string>,
        Future<string>>& t) -> Future<CommandResult> {
      Future<Option<int>> status = std::get<0>(t);
      if (!status.isReady()) {
        return Failure(
            "Failed to get the exit status of the subprocess: " +
            (status.isFailed() ? status.failure() : "discarded"));
      }

      Future<string> output = std::get<1>(t);
      if (!output.isReady()) {
        return Failure(
            "Failed to read stdout from the subprocess: " +
            (output.isFailed() ? output.failure() : "discarded"));
      }

      Future<string> error = std::get<2>(t);
      if (!error.isReady()) {
        return Failure(
            "Failed to read stderr from the subprocess: " +
            (error.isFailed() ? error.failure() : "discarded"));
      }

      CommandResult result;
      result.status = status.get();
      result.out = output.get();
      result.err = error.get();

      return result;
    });
}


Try<Owned<HDFS>> HDFS::create(const Option<string>& _hadoop)
{
  // Determine the hadoop client to use. If the user has specified
  // it, use it. If not, look for environment variable HADOOP_HOME. If
  // the environment variable is not set, assume it's on the PATH.
  string hadoop;

  if (_hadoop.isSome()) {
    hadoop = _hadoop.get();
  } else {
    Option<string> hadoopHome = os::getenv("HADOOP_HOME");
    if (hadoopHome.isSome()) {
      hadoop = path::join(hadoopHome.get(), "bin", "hadoop");
    } else {
      hadoop = "hadoop";
    }
  }

  // Check if the hadoop client is available.
  Try<string> out = os::shell(hadoop + " version 2>&1");
  if (out.isError()) {
    return Error(out.error());
  }

  return Owned<HDFS>(new HDFS(hadoop));
}


Future<bool> HDFS::exists(const string& path)
{
  Try<Subprocess> s = subprocess(
      hadoop,
      {"hadoop", "fs", "-test", "-e", absolutePath(path)},
      Subprocess::PATH("/dev/null"),
      Subprocess::PIPE(),
      Subprocess::PIPE());

  if (s.isError()) {
    return Failure("Failed to execute the subprocess: " + s.error());
  }

  return result(s.get())
    .then([](const CommandResult& result) -> Future<bool> {
      if (result.status.isNone()) {
        return Failure("Failed to reap the subprocess");
      }

      if (WIFEXITED(result.status.get())) {
        int exitCode = WEXITSTATUS(result.status.get());
        if (exitCode == 0) {
          return true;
        } else if (exitCode == 1) {
          return false;
        }
      }

      return Failure(
          "Unexpected result from the subprocess: "
          "status='" + stringify(result.status.get()) + "', " +
          "stdout='" + result.out + "', " +
          "stderr='" + result.err + "'");
    });
}


Future<Bytes> HDFS::du(const string& _path)
{
  const string path = absolutePath(_path);

  Try<Subprocess> s = subprocess(
      hadoop,
      {"hadoop", "fs", "-du", path},
      Subprocess::PATH("/dev/null"),
      Subprocess::PIPE(),
      Subprocess::PIPE());

  if (s.isError()) {
    return Failure("Failed to execute the subprocess: " + s.error());
  }

  return result(s.get())
    .then([path](const CommandResult& result) -> Future<Bytes> {
      if (result.status.isNone()) {
        return Failure("Failed to reap the subprocess");
      }

      if (result.status.get() != 0) {
        return Failure(
            "Unexpected result from the subprocess: "
            "status='" + stringify(result.status.get()) + "', " +
            "stdout='" + result.out + "', " +
            "stderr='" + result.err + "'");
      }

      // We expect 2 space-separated output fields; a number of bytes
      // then the name of the path we gave. The 'hadoop' command can
      // emit various WARN or other log messages, so we make an effort
      // to scan for the field we want.
      foreach (const string& line, strings::tokenize(result.out, "\n")) {
        // Note that we use tokenize() rather than split() since
        // fields can be delimited by multiple spaces.
        vector<string> fields = strings::tokenize(line, " \t");

        if (fields.size() == 2 && fields[1] == path) {
          Result<size_t> size = numify<size_t>(fields[0]);
          if (size.isSome()) {
            return Bytes(size.get());
          }
        }
      }

      return Failure("Unexpected output format: '" + result.out + "'");
    });
}


Future<Nothing> HDFS::rm(const string& path)
{
  Try<Subprocess> s = subprocess(
      hadoop,
      {"hadoop", "fs", "-rm", absolutePath(path)},
      Subprocess::PATH("/dev/null"),
      Subprocess::PIPE(),
      Subprocess::PIPE());

  if (s.isError()) {
    return Failure("Failed to execute the subprocess: " + s.error());
  }

  return result(s.get())
    .then([](const CommandResult& result) -> Future<Nothing> {
      if (result.status.isNone()) {
        return Failure("Failed to reap the subprocess");
      }

      if (result.status.get() != 0) {
        return Failure(
            "Unexpected result from the subprocess: "
            "status='" + stringify(result.status.get()) + "', " +
            "stdout='" + result.out + "', " +
            "stderr='" + result.err + "'");
      }

      return Nothing();
    });
}


Future<Nothing> HDFS::copyFromLocal(const string& from, const string& to)
{
  if (!os::exists(from)) {
    return Failure("Failed to find '" + from + "'");
  }

  Try<Subprocess> s = subprocess(
      hadoop,
      {"hadoop", "fs", "-copyFromLocal", from, absolutePath(to)},
      Subprocess::PATH("/dev/null"),
      Subprocess::PIPE(),
      Subprocess::PIPE());

  if (s.isError()) {
    return Failure("Failed to execute the subprocess: " + s.error());
  }

  return result(s.get())
    .then([](const CommandResult& result) -> Future<Nothing> {
      if (result.status.isNone()) {
        return Failure("Failed to reap the subprocess");
      }

      if (result.status.get() != 0) {
        return Failure(
            "Unexpected result from the subprocess: "
            "status='" + stringify(result.status.get()) + "', " +
            "stdout='" + result.out + "', " +
            "stderr='" + result.err + "'");
      }

      return Nothing();
    });
}


Future<Nothing> HDFS::copyToLocal(const string& from, const string& to)
{
  Try<Subprocess> s = subprocess(
      hadoop,
      {"hadoop", "fs", "-copyToLocal", absolutePath(from), to},
      Subprocess::PATH("/dev/null"),
      Subprocess::PIPE(),
      Subprocess::PIPE());

  if (s.isError()) {
    return Failure("Failed to execute the subprocess: " + s.error());
  }

  return result(s.get())
    .then([](const CommandResult& result) -> Future<Nothing> {
      if (result.status.isNone()) {
        return Failure("Failed to reap the subprocess");
      }

      if (result.status.get() != 0) {
        return Failure(
            "Unexpected result from the subprocess: "
            "status='" + stringify(result.status.get()) + "', " +
            "stdout='" + result.out + "', " +
            "stderr='" + result.err + "'");
      }

      return Nothing();
    });
}


string HDFS::absolutePath(const string& hdfsPath)
{
  if (strings::startsWith(hdfsPath, "hdfs://") ||
      strings::startsWith(hdfsPath, "/")) {
    return hdfsPath;
  }

  return path::join("", hdfsPath);
}
