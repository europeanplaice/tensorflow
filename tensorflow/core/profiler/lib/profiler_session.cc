/* Copyright 2018 The TensorFlow Authors. All Rights Reserved.

Licensed under the Apache License, Version 2.0 (the "License");
you may not use this file except in compliance with the License.
You may obtain a copy of the License at

    http://www.apache.org/licenses/LICENSE-2.0

Unless required by applicable law or agreed to in writing, software
distributed under the License is distributed on an "AS IS" BASIS,
WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
See the License for the specific language governing permissions and
limitations under the License.
==============================================================================*/

#include "tensorflow/core/profiler/lib/profiler_session.h"

#include <memory>
#include <utility>

#include "absl/memory/memory.h"
#include "tensorflow/core/platform/errors.h"
#include "tensorflow/core/platform/logging.h"
#include "tensorflow/core/platform/mutex.h"
#include "tensorflow/core/platform/platform.h"
#include "tensorflow/core/platform/status.h"
#include "tensorflow/core/platform/types.h"
#include "tensorflow/core/profiler/profiler_options.pb.h"
#include "tensorflow/core/profiler/protobuf/xplane.pb.h"

#if !defined(IS_MOBILE_PLATFORM)
#include "tensorflow/core/profiler/convert/post_process_single_host_xplane.h"
#include "tensorflow/core/profiler/lib/profiler_factory.h"
#include "tensorflow/core/profiler/lib/profiler_interface.h"
#include "tensorflow/core/profiler/lib/profiler_lock.h"
#include "tensorflow/core/profiler/utils/time_utils.h"
#endif

namespace tensorflow {
namespace {

ProfileOptions GetOptions(const ProfileOptions& opts) {
  if (opts.version()) return opts;
  ProfileOptions options = ProfilerSession::DefaultOptions();
  options.set_include_dataset_ops(opts.include_dataset_ops());
  return options;
}

};  // namespace

/*static*/ std::unique_ptr<ProfilerSession> ProfilerSession::Create(
    const ProfileOptions& options) {
  return absl::WrapUnique(new ProfilerSession(options));
}

tensorflow::Status ProfilerSession::Status() {
  mutex_lock l(mutex_);
  return status_;
}

Status ProfilerSession::CollectDataInternal(profiler::XSpace* space) {
  mutex_lock l(mutex_);
  TF_RETURN_IF_ERROR(status_);
#if !defined(IS_MOBILE_PLATFORM)
  LOG(INFO) << "Profiler session collecting data.";
  for (auto& profiler : profilers_) {
    profiler->Stop().IgnoreError();
  }

  for (auto& profiler : profilers_) {
    profiler->CollectData(space).IgnoreError();
  }

  // Allow another session to start.
  profiler_lock_.ReleaseIfActive();
#endif
  return Status::OK();
}

Status ProfilerSession::CollectData(profiler::XSpace* space) {
#if !defined(IS_MOBILE_PLATFORM)
  TF_RETURN_IF_ERROR(CollectDataInternal(space));
  PostProcessSingleHostXSpace(space, start_time_ns_);
#endif
  return Status::OK();
}

ProfilerSession::ProfilerSession(const ProfileOptions& options)
#if defined(IS_MOBILE_PLATFORM)
    : status_(errors::Unimplemented(
          "Profiler is unimplemented for mobile platforms.")) {
#else
    : options_(GetOptions(options)) {
  auto profiler_lock = profiler::ProfilerLock::Acquire();
  if (!profiler_lock.ok()) {
    status_ = profiler_lock.status();
    return;
  }
  profiler_lock_ = *std::move(profiler_lock);

  LOG(INFO) << "Profiler session initializing.";
  // Sleep until it is time to start profiling.
  if (options_.start_timestamp_ns() > 0) {
    int64_t sleep_duration_ns =
        options_.start_timestamp_ns() - profiler::GetCurrentTimeNanos();
    if (sleep_duration_ns < 0) {
      LOG(WARNING) << "Profiling is late by " << -sleep_duration_ns
                   << " nanoseconds and will start immediately.";
    } else {
      LOG(INFO) << "Delaying start of profiler session by "
                << sleep_duration_ns;
      profiler::SleepForNanos(sleep_duration_ns);
    }
  }

  LOG(INFO) << "Profiler session started.";
  start_time_ns_ = profiler::GetCurrentTimeNanos();

  DCHECK(profiler_lock_.Active());
  CreateProfilers(options_, &profilers_);

  for (auto& profiler : profilers_) {
    DCHECK(profiler != nullptr);
    auto start_status = profiler->Start();
    if (!start_status.ok()) {
      LOG(WARNING) << "Encountered error while starting profiler: "
                   << start_status.ToString();
    }
  }
#endif
}

ProfilerSession::~ProfilerSession() {
#if !defined(IS_MOBILE_PLATFORM)
  LOG(INFO) << "Profiler session tear down.";
  for (auto& profiler : profilers_) {
    profiler->Stop().IgnoreError();
  }

  // Allow another session to start.
  profiler_lock_.ReleaseIfActive();
#endif
}

}  // namespace tensorflow
