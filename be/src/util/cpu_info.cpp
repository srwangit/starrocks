// Licensed to the Apache Software Foundation (ASF) under one
// or more contributor license agreements.  See the NOTICE file
// distributed with this work for additional information
// regarding copyright ownership.  The ASF licenses this file
// to you under the Apache License, Version 2.0 (the
// "License"); you may not use this file except in compliance
// with the License.  You may obtain a copy of the License at
//
//   http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing,
// software distributed under the License is distributed on an
// "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY
// KIND, either express or implied.  See the License for the
// specific language governing permissions and limitations
// under the License.

#include "util/cpu_info.h"

#if defined(__GNUC__) && (defined(__x86_64__) || defined(__i386__))
/* GCC-compatible compiler, targeting x86/x86-64 */
#include <x86intrin.h>
#elif defined(__GNUC__) && defined(__ARM_NEON)
/* GCC-compatible compiler, targeting ARM with NEON */
#include <arm_neon.h>
#elif defined(__GNUC__) && defined(__IWMMXT__)
/* GCC-compatible compiler, targeting ARM with WMMX */
#include <mmintrin.h>
#elif (defined(__GNUC__) || defined(__xlC__)) && (defined(__VEC__) || defined(__ALTIVEC__))
/* XLC or GCC-compatible compiler, targeting PowerPC with VMX/VSX */
#include <altivec.h>
#elif defined(__GNUC__) && defined(__SPE__)
/* GCC-compatible compiler, targeting PowerPC with SPE */
#include <spe.h>
#endif

// CGROUP2_SUPER_MAGIC is the indication for cgroup v2
// It is defined in kernel 4.5+
// I copy the defintion from linux/magic.h in higher kernel
#ifndef CGROUP2_SUPER_MAGIC
#define CGROUP2_SUPER_MAGIC 0x63677270
#endif

#include <linux/magic.h>
#include <sched.h>
#include <sys/sysinfo.h>
#include <sys/vfs.h>
#include <unistd.h>

#include <algorithm>
#include <boost/algorithm/string.hpp>
#include <cctype>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <iostream>

#include "common/config.h"
#include "common/env_config.h"
#include "common/logging.h"
#include "fs/fs_util.h"
#include "gflags/gflags.h"
#include "gutil/strings/split.h"
#include "gutil/strings/substitute.h"
#include "util/errno.h"
#include "util/file_util.h"
#include "util/pretty_printer.h"
#include "util/string_parser.hpp"

using boost::algorithm::contains;
using boost::algorithm::trim;

using std::max;

DECLARE_bool(abort_on_config_error);
DEFINE_int32(num_cores, 0,
             "(Advanced) If > 0, it sets the number of cores available to"
             " Impala. Setting it to 0 means Impala will use all available cores on the machine"
             " according to /proc/cpuinfo.");

namespace starrocks {

bool CpuInfo::initialized_ = false;
int64_t CpuInfo::hardware_flags_ = 0;
int64_t CpuInfo::cycles_per_ms_;
int CpuInfo::num_cores_ = 1;
int CpuInfo::max_num_cores_ = 1;
std::string CpuInfo::model_name_ = "unknown";
bool CpuInfo::is_cgroup_with_cpuset_ = false;
bool CpuInfo::is_cgroup_with_cpu_quota_ = false;
int CpuInfo::max_num_numa_nodes_;
std::unique_ptr<int[]> CpuInfo::core_to_numa_node_;
std::vector<vector<int>> CpuInfo::numa_node_to_cores_;
std::vector<size_t> CpuInfo::cpuset_cores_;
std::set<size_t> CpuInfo::offline_cores_;
std::vector<int> CpuInfo::numa_node_core_idx_;
std::vector<long> CpuInfo::cache_sizes;
std::vector<long> CpuInfo::cache_line_sizes;

static struct {
    string name;
    int64_t flag;
} flag_mappings[] = {
        {"ssse3", CpuInfo::SSSE3},     {"sse4_1", CpuInfo::SSE4_1},     {"sse4_2", CpuInfo::SSE4_2},
        {"popcnt", CpuInfo::POPCNT},   {"avx", CpuInfo::AVX},           {"avx2", CpuInfo::AVX2},
        {"avx512f", CpuInfo::AVX512F}, {"avx512bw", CpuInfo::AVX512BW},
};

// Helper function to parse for hardware flags.
// values contains a list of space-seperated flags.  check to see if the flags we
// care about are present.
// Returns a bitmap of flags.
int64_t ParseCPUFlags(const string& values) {
    int64_t flags = 0;
    for (auto& flag_mapping : flag_mappings) {
        if (contains(values, flag_mapping.name)) {
            flags |= flag_mapping.flag;
        }
    }
    return flags;
}

void CpuInfo::init() {
    if (initialized_) return;
    string line;
    string name;
    string value;

    float max_mhz = 0;
    int num_cores = 0;

    // Read from /proc/cpuinfo
    std::ifstream cpuinfo("/proc/cpuinfo");
    while (cpuinfo) {
        getline(cpuinfo, line);
        size_t colon = line.find(':');
        if (colon != string::npos) {
            name = line.substr(0, colon - 1);
            value = line.substr(colon + 1, string::npos);
            trim(name);
            trim(value);
            if (name.compare("flags") == 0) {
                hardware_flags_ |= ParseCPUFlags(value);
            } else if (name.compare("cpu MHz") == 0) {
                // Every core will report a different speed.  We'll take the max, assuming
                // that when impala is running, the core will not be in a lower power state.
                // TODO: is there a more robust way to do this, such as
                // Window's QueryPerformanceFrequency()
                float mhz = atof(value.c_str());
                max_mhz = max(mhz, max_mhz);
            } else if (name.compare("processor") == 0) {
                ++num_cores;
            } else if (name.compare("model name") == 0) {
                model_name_ = value;
            }
        }
    }

    if (max_mhz != 0) {
        cycles_per_ms_ = max_mhz * 1000;
    } else {
        cycles_per_ms_ = 1000000;
    }

    if (num_cores > 0) {
        num_cores_ = num_cores;
    }
    _init_offline_cores();
    _init_num_cores_with_cgroup();
    if (num_cores_ <= 0) {
        num_cores_ = 1;
    }
    if (config::num_cores > 0) num_cores_ = config::num_cores;
    max_num_cores_ = get_nprocs_conf();

    // Print a warning if something is wrong with sched_getcpu().
#ifdef HAVE_SCHED_GETCPU
    if (sched_getcpu() == -1) {
        LOG(WARNING) << "Kernel does not support sched_getcpu(). Performance may be impacted.";
    }
#else
    LOG(WARNING) << "Built on a system without sched_getcpu() support. Performance may"
                 << " be impacted.";
#endif

    _init_numa();
    _init_cache_info();
    initialized_ = true;
}

void CpuInfo::_init_numa() {
    // Use the NUMA info in the /sys filesystem. which is part of the Linux ABI:
    // see https://www.kernel.org/doc/Documentation/ABI/stable/sysfs-devices-node and
    // https://www.kernel.org/doc/Documentation/ABI/testing/sysfs-devices-system-cpu
    // The filesystem entries are only present if the kernel was compiled with NUMA support.
    core_to_numa_node_.reset(new int[max_num_cores_]);

    if (!std::filesystem::is_directory("/sys/devices/system/node")) {
        LOG(WARNING) << "/sys/devices/system/node is not present - no NUMA support";
        // Assume a single NUMA node.
        max_num_numa_nodes_ = 1;
        std::fill_n(core_to_numa_node_.get(), max_num_cores_, 0);
        _init_numa_node_to_cores();
        return;
    }

    // Search for node subdirectories - node0, node1, node2, etc to determine possible
    // NUMA nodes.
    max_num_numa_nodes_ = 0;
    for (const auto& item : std::filesystem::directory_iterator("/sys/devices/system/node")) {
        const string filename = item.path().filename().string();
        if (filename.find("node") == 0) ++max_num_numa_nodes_;
    }
    if (max_num_numa_nodes_ == 0) {
        LOG(WARNING) << "Could not find nodes in /sys/devices/system/node";
        max_num_numa_nodes_ = 1;
    }

    // Check which NUMA node each core belongs to based on the existence of a symlink
    // to the node subdirectory.
    for (int core = 0; core < max_num_cores_; ++core) {
        bool found_numa_node = false;
        for (int node = 0; node < max_num_numa_nodes_; ++node) {
            if (std::filesystem::exists(strings::Substitute("/sys/devices/system/cpu/cpu$0/node$1", core, node))) {
                core_to_numa_node_[core] = node;
                found_numa_node = true;
                break;
            }
        }
        if (!found_numa_node) {
            LOG(WARNING) << "Could not determine NUMA node for core " << core << " from /sys/devices/system/cpu/";
            core_to_numa_node_[core] = 0;
        }
    }
    _init_numa_node_to_cores();
}

std::vector<size_t> CpuInfo::parse_cpus(const std::string& cpus_str) {
    std::vector<size_t> cpuids;
    std::vector<std::string> fields = strings::Split(cpus_str, ",", strings::SkipWhitespace());
    for (const auto& field : fields) {
        StringParser::ParseResult result;
        if (field.find('-') == std::string::npos) {
            auto cpu_id = StringParser::string_to_int<int32_t>(field.data(), field.size(), &result);
            if (result == StringParser::PARSE_SUCCESS) {
                cpuids.emplace_back(cpu_id);
            }
            continue;
        }

        std::vector<std::string> pair = strings::Split(field, "-", strings::SkipWhitespace());
        if (pair.size() != 2) {
            continue;
        }
        std::string& start_str = pair[0];
        std::string& end_str = pair[1];
        auto start = StringParser::string_to_int<int32_t>(start_str.data(), start_str.size(), &result);
        if (result != StringParser::PARSE_SUCCESS) {
            continue;
        }
        auto end = StringParser::string_to_int<int32_t>(end_str.data(), end_str.size(), &result);
        if (result != StringParser::PARSE_SUCCESS) {
            continue;
        }
        for (int i = start; i <= end; i++) {
            cpuids.emplace_back(i);
        }
    }
    return cpuids;
}

void CpuInfo::_init_num_cores_with_cgroup() {
    bool running_in_docker = fs::path_exist("/.dockerenv");
    if (!running_in_docker) {
        return;
    }
    struct statfs fs;
    if (statfs("/sys/fs/cgroup", &fs) < 0) {
        LOG(WARNING) << "Fail to get file system statistics. err: " << errno_to_string(errno);
        return;
    }

    std::string cfs_period_us_str;
    std::string cfs_quota_us_str;
    std::string cpuset_str;
    if (fs.f_type == TMPFS_MAGIC) {
        // cgroup v1
        if (!FileUtil::read_whole_content("/sys/fs/cgroup/cpu/cpu.cfs_period_us", cfs_period_us_str)) {
            return;
        }

        if (!FileUtil::read_whole_content("/sys/fs/cgroup/cpu/cpu.cfs_quota_us", cfs_quota_us_str)) {
            return;
        }

        if (!FileUtil::read_whole_content("/sys/fs/cgroup/cpuset/cpuset.cpus", cpuset_str)) {
            return;
        }
    } else if (fs.f_type == CGROUP2_SUPER_MAGIC) {
        // cgroup v2
        if (!FileUtil::read_contents("/sys/fs/cgroup/cpu.max", cfs_quota_us_str, cfs_period_us_str)) {
            return;
        }

        if (!FileUtil::read_whole_content("/sys/fs/cgroup/cpuset.cpus", cpuset_str)) {
            return;
        }
    }

    int32_t cfs_num_cores = num_cores_;
    {
        StringParser::ParseResult result;
        auto cfs_period_us =
                StringParser::string_to_int<int64_t>(cfs_period_us_str.data(), cfs_period_us_str.size(), &result);
        if (result != StringParser::PARSE_SUCCESS) {
            cfs_period_us = -1;
        }
        auto cfs_quota_us =
                StringParser::string_to_int<int64_t>(cfs_quota_us_str.data(), cfs_quota_us_str.size(), &result);
        if (result != StringParser::PARSE_SUCCESS) {
            cfs_quota_us = -1;
        }
        if (cfs_quota_us > 0 && cfs_period_us > 0) {
            cfs_num_cores = cfs_quota_us / cfs_period_us;
            is_cgroup_with_cpu_quota_ = true;
        }
    }

    int32_t cpuset_num_cores = num_cores_;
    if (!cpuset_str.empty() &&
        std::any_of(cpuset_str.begin(), cpuset_str.end(), [](char c) { return !std::isspace(c); })) {
        cpuset_cores_ = parse_cpus(cpuset_str);
        std::erase_if(cpuset_cores_, [&](const size_t core) { return offline_cores_.contains(core); });
        cpuset_num_cores = cpuset_cores_.size();
        is_cgroup_with_cpuset_ = true;
    }

    if (cfs_num_cores < num_cores_ || cpuset_num_cores < num_cores_) {
        num_cores_ = std::max(1, std::min(cfs_num_cores, cpuset_num_cores));
        LOG(INFO) << "Init docker hardware cores by cgroup's config, cfs_num_cores=" << cfs_num_cores
                  << ", cpuset_num_cores=" << cpuset_num_cores << ", final num_cores=" << num_cores_;
    }
}

void CpuInfo::_init_numa_node_to_cores() {
    DCHECK(numa_node_to_cores_.empty());
    numa_node_to_cores_.resize(max_num_numa_nodes_);
    numa_node_core_idx_.resize(max_num_cores_);
    for (int core = 0; core < max_num_cores_; ++core) {
        std::vector<int>* cores_of_node = &numa_node_to_cores_[core_to_numa_node_[core]];
        numa_node_core_idx_[core] = cores_of_node->size();
        cores_of_node->push_back(core);
    }
}

void CpuInfo::_init_offline_cores() {
    offline_cores_.clear();
    std::string offline_cores_str;
    if (!FileUtil::read_whole_content("/sys/devices/system/cpu/offline", offline_cores_str)) {
        return;
    }

    std::vector<size_t> offline_cores = parse_cpus(offline_cores_str);
    offline_cores_.insert(offline_cores.begin(), offline_cores.end());
}

int CpuInfo::get_current_core() {
    // sched_getcpu() is not supported on some old kernels/glibcs (like the versions that
    // shipped with CentOS 5). In that case just pretend we're always running on CPU 0
    // so that we can build and run with degraded perf.
#ifdef HAVE_SCHED_GETCPU
    int cpu = sched_getcpu();
    if (cpu < 0) return 0;
    if (cpu >= max_num_cores_) {
        LOG_FIRST_N(WARNING, 5) << "sched_getcpu() return value " << cpu
                                << ", which is greater than get_nprocs_conf() retrun value " << max_num_cores_
                                << ", now is " << get_nprocs_conf();
        cpu %= max_num_cores_;
    }
    return cpu;
#else
    return 0;
#endif
}

void CpuInfo::_init_cache_info() {
    cache_sizes.resize(NUM_CACHE_LEVELS);
    cache_line_sizes.resize(NUM_CACHE_LEVELS);
    _get_cache_info(cache_sizes.data(), cache_line_sizes.data());
}

void CpuInfo::_get_cache_info(long cache_sizes[NUM_CACHE_LEVELS], long cache_line_sizes[NUM_CACHE_LEVELS]) {
#ifdef __APPLE__
    // On Mac OS X use sysctl() to get the cache sizes
    size_t len = 0;
    sysctlbyname("hw.cachesize", NULL, &len, NULL, 0);
    uint64_t* data = static_cast<uint64_t*>(malloc(len));
    sysctlbyname("hw.cachesize", data, &len, NULL, 0);
    DCHECK(len / sizeof(uint64_t) >= 3);
    for (size_t i = 0; i < NUM_CACHE_LEVELS; ++i) {
        cache_sizes[i] = data[i];
    }
    size_t linesize;
    size_t sizeof_linesize = sizeof(linesize);
    sysctlbyname("hw.cachelinesize", &linesize, &sizeof_linesize, NULL, 0);
    for (size_t i = 0; i < NUM_CACHE_LEVELS; ++i) cache_line_sizes[i] = linesize;
#else
    // Call sysconf to query for the cache sizes
    // Note: on some systems (e.g. RHEL 5 on AWS EC2), this returns 0 instead of the
    // actual cache line size.
    cache_sizes[L1_CACHE] = sysconf(_SC_LEVEL1_DCACHE_SIZE);
    cache_sizes[L2_CACHE] = sysconf(_SC_LEVEL2_CACHE_SIZE);
    cache_sizes[L3_CACHE] = sysconf(_SC_LEVEL3_CACHE_SIZE);

    cache_line_sizes[L1_CACHE] = sysconf(_SC_LEVEL1_DCACHE_LINESIZE);
    cache_line_sizes[L2_CACHE] = sysconf(_SC_LEVEL2_CACHE_LINESIZE);
    cache_line_sizes[L3_CACHE] = sysconf(_SC_LEVEL3_CACHE_LINESIZE);
#endif
}

std::string CpuInfo::debug_string() {
    DCHECK(initialized_);
    std::stringstream stream;
    long cache_sizes[NUM_CACHE_LEVELS];
    long cache_line_sizes[NUM_CACHE_LEVELS];
    _get_cache_info(cache_sizes, cache_line_sizes);

    string L1 =
            strings::Substitute("L1 Cache: $0 (Line: $1)", PrettyPrinter::print(cache_sizes[L1_CACHE], TUnit::BYTES),
                                PrettyPrinter::print(cache_line_sizes[L1_CACHE], TUnit::BYTES));
    string L2 =
            strings::Substitute("L2 Cache: $0 (Line: $1)", PrettyPrinter::print(cache_sizes[L2_CACHE], TUnit::BYTES),
                                PrettyPrinter::print(cache_line_sizes[L2_CACHE], TUnit::BYTES));
    string L3 =
            strings::Substitute("L3 Cache: $0 (Line: $1)", PrettyPrinter::print(cache_sizes[L3_CACHE], TUnit::BYTES),
                                PrettyPrinter::print(cache_line_sizes[L3_CACHE], TUnit::BYTES));
    stream << "Cpu Info:" << std::endl
           << "  Model: " << model_name_ << std::endl
           << "  Cores: " << num_cores_ << std::endl
           << "  Max Possible Cores: " << max_num_cores_ << std::endl
           << "  " << L1 << std::endl
           << "  " << L2 << std::endl
           << "  " << L3 << std::endl
           << "  Hardware Supports:" << std::endl;
    for (auto& flag_mapping : flag_mappings) {
        if (is_supported(flag_mapping.flag)) {
            stream << "    " << flag_mapping.name << std::endl;
        }
    }
    stream << "  Numa Nodes: " << max_num_numa_nodes_ << std::endl;
    stream << "  Numa Nodes of Cores:";
    for (int core = 0; core < max_num_cores_; ++core) {
        stream << " " << core << "->" << core_to_numa_node_[core] << " |";
    }
    stream << std::endl;

    auto print_cores = [&stream](const std::string& title, const auto& cores) {
        stream << "  " << title << ": ";
        if (cores.empty()) {
            stream << "None";
        } else {
            bool is_first = true;
            for (const int core : cores) {
                if (!is_first) {
                    stream << ",";
                }
                is_first = false;
                stream << core;
            }
        }
        stream << std::endl;
    };

    print_cores("Cores from CGroup CPUSET", cpuset_cores_);
    print_cores("Offline Cores", offline_cores_);

    return stream.str();
}

std::vector<size_t> CpuInfo::get_core_ids() {
    std::vector<size_t> core_ids;
    if (!cpuset_cores_.empty()) {
        core_ids = cpuset_cores_;
    } else {
        for (const auto& core_ids_of_node : numa_node_to_cores_) {
            core_ids.insert(core_ids.end(), core_ids_of_node.begin(), core_ids_of_node.end());
        }
    }

    std::erase_if(core_ids, [&](const size_t core) { return offline_cores_.contains(core); });

    return core_ids;
}

std::vector<std::string> CpuInfo::unsupported_cpu_flags_from_current_env() {
    std::vector<std::string> unsupported_flags;
    for (auto& flag_mapping : flag_mappings) {
        if (!is_supported(flag_mapping.flag)) {
            // AVX is skipped due to there is no condition compile flags for it
            // case CpuInfo::AVX:
            bool unsupported = false;
            switch (flag_mapping.flag) {
#if defined(__x86_64__) && defined(__SSSE3__)
            case CpuInfo::SSSE3:
                unsupported = true;
                break;
#endif
#if defined(__x86_64__) && defined(__SSE4_1__)
            case CpuInfo::SSE4_1:
                unsupported = true;
                break;
#endif
#if defined(__x86_64__) && defined(__SSE4_2__)
            case CpuInfo::SSE4_2:
                unsupported = true;
                break;
#endif
#if defined(__x86_64__) && defined(__AVX2__)
            case CpuInfo::AVX2:
                unsupported = true;
                break;
#endif
#if defined(__x86_64__) && defined(__AVX512F__)
            case CpuInfo::AVX512F:
                unsupported = true;
                break;
#endif
#if defined(__x86_64__) && defined(__AVX512BW__)
            case CpuInfo::AVX512BW:
                unsupported = true;
                break;
#endif
            }
            if (unsupported) {
                unsupported_flags.push_back(flag_mapping.name);
            }
        }
    }
    return unsupported_flags;
}

} // namespace starrocks
