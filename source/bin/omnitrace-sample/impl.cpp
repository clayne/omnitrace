// MIT License
//
// Copyright (c) 2022 Advanced Micro Devices, Inc. All Rights Reserved.
//
// Permission is hereby granted, free of charge, to any person obtaining a copy
// of this software and associated documentation files (the "Software"), to deal
// in the Software without restriction, including without limitation the rights
// to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
// copies of the Software, and to permit persons to whom the Software is
// furnished to do so, subject to the following conditions:
//
// The above copyright notice and this permission notice shall be included in all
// copies or substantial portions of the Software.
//
// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
// IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
// FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
// AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
// LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
// OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
// SOFTWARE.

#include "omnitrace-sample.hpp"

#include "common/delimit.hpp"
#include "common/environment.hpp"
#include "common/join.hpp"
#include "common/setup.hpp"

#include <timemory/environment.hpp>
#include <timemory/log/color.hpp>
#include <timemory/utility/argparse.hpp>
#include <timemory/utility/console.hpp>
#include <timemory/utility/join.hpp>

#include <array>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <iostream>
#include <stdexcept>
#include <string_view>
#include <unistd.h>
#include <vector>

#if !defined(OMNITRACE_USE_ROCTRACER)
#    define OMNITRACE_USE_ROCTRACER 0
#endif

#if !defined(OMNITRACE_USE_ROCPROFILER)
#    define OMNITRACE_USE_ROCPROFILER 0
#endif

namespace color = tim::log::color;
using tim::log::stream;
using namespace timemory::join;
using tim::get_env;

namespace
{
int verbose = 0;
}

std::string
get_command(const char* _argv0)
{
    return omnitrace::path::find_path(_argv0, 0, omnitrace::common::get_env("PATH", ""));
}

std::string
get_realpath(const std::string& _v)
{
    auto* _tmp = realpath(_v.c_str(), nullptr);
    auto  _ret = std::string{ _tmp };
    free(_tmp);
    return _ret;
}

void
print_command(const std::vector<char*>& _argv)
{
    if(verbose >= 1)
        stream(std::cout, color::info())
            << "Executing '" << join(array_config{ " " }, _argv) << "'...\n";
}

std::vector<char*>
get_initial_environment()
{
    std::vector<char*> _env;
    if(environ != nullptr)
    {
        int idx = 0;
        while(environ[idx] != nullptr)
            _env.emplace_back(strdup(environ[idx++]));
    }

    update_env(_env, "LD_PRELOAD",
               get_realpath(get_internal_libpath("libomnitrace-dl.so")), true);

    auto* _dl_libpath =
        realpath(get_internal_libpath("libomnitrace-dl.so").c_str(), nullptr);
    auto* _omni_libpath =
        realpath(get_internal_libpath("libomnitrace.so").c_str(), nullptr);

    update_env(_env, "OMNITRACE_USE_SAMPLING", true);
    update_env(_env, "OMNITRACE_CRITICAL_TRACE", false);
    update_env(_env, "OMNITRACE_USE_PROCESS_SAMPLING", false);
    // update_env(_env, "OMNITRACE_USE_PID", false);
    // update_env(_env, "OMNITRACE_TIME_OUTPUT", false);
    // update_env(_env, "OMNITRACE_OUTPUT_PATH", "omnitrace-output/%tag%/%launch_time%");

#if defined(OMNITRACE_USE_ROCTRACER) || defined(OMNITRACE_USE_ROCPROFILER)
    update_env(_env, "HSA_TOOLS_LIB", _dl_libpath);
    update_env(_env, "HSA_TOOLS_REPORT_LOAD_FAILURE", "1");
#endif

#if defined(OMNITRACE_USE_ROCPROFILER)
    update_env(_env, "ROCP_TOOL_LIB", _omni_libpath);
    update_env(_env, "ROCP_HSA_INTERCEPT", "1");
#endif

#if defined(OMNITRACE_USE_OMPT)
    update_env(_env, "OMP_TOOL_LIBRARIES", _dl_libpath);
#endif

    free(_dl_libpath);
    free(_omni_libpath);

    return _env;
}

std::string
get_internal_libpath(const std::string& _lib)
{
    auto _exe = std::string_view{ realpath("/proc/self/exe", nullptr) };
    auto _pos = _exe.find_last_of('/');
    auto _dir = std::string{ "./" };
    if(_pos != std::string_view::npos) _dir = _exe.substr(0, _pos);
    return omnitrace::common::join("/", _dir, "..", "lib", _lib);
}

template <typename Tp>
void
update_env(std::vector<char*>& _environ, std::string_view _env_var, Tp&& _env_val,
           bool _append)
{
    auto _key = join("", _env_var, "=");
    for(auto& itr : _environ)
    {
        if(!itr) continue;
        if(std::string_view{ itr }.find(_key) == 0)
        {
            if(_append)
            {
                auto _val = std::string{ itr }.substr(_key.length());
                free(itr);
                itr = strdup(
                    omnitrace::common::join('=', _env_var, join(":", _env_val, _val))
                        .c_str());
            }
            else
            {
                free(itr);
                itr = strdup(omnitrace::common::join('=', _env_var, _env_val).c_str());
            }
            return;
        }
    }
    _environ.emplace_back(
        strdup(omnitrace::common::join('=', _env_var, _env_val).c_str()));
}

std::vector<char*>
parse_args(int argc, char** argv, std::vector<char*>& _env)
{
    using parser_t     = tim::argparse::argument_parser;
    using parser_err_t = typename parser_t::result_type;

    auto help_check = [](parser_t& p, int _argc, char** _argv) {
        std::set<std::string> help_args = { "-h", "--help", "-?" };
        return (p.exists("help") || _argc == 1 ||
                (_argc > 1 && help_args.find(_argv[1]) != help_args.end()));
    };

    auto _pec        = EXIT_SUCCESS;
    auto help_action = [&_pec, argc, argv](parser_t& p) {
        if(_pec != EXIT_SUCCESS)
        {
            std::stringstream msg;
            msg << "Error in command:";
            for(int i = 0; i < argc; ++i)
                msg << " " << argv[i];
            msg << "\n\n";
            stream(std::cerr, color::fatal()) << msg.str();
            std::cerr << std::flush;
        }

        p.print_help();
        exit(_pec);
    };

    auto parser = parser_t(argv[0]);

    parser.on_error([](parser_t&, const parser_err_t& _err) {
        stream(std::cerr, color::fatal()) << _err << "\n";
        exit(EXIT_FAILURE);
    });

    const auto* _cputime_desc =
        R"(Sample based on a CPU-clock timer (default). Accepts zero or more arguments:
    %{INDENT}%0. Enables sampling based on CPU-clock timer.
    %{INDENT}%1. Interrupts per second. E.g., 100 == sample every 10 milliseconds of CPU-time.
    %{INDENT}%2. Delay (in seconds of CPU-clock time). I.e., how long each thread should wait before taking first sample.
    %{INDENT}%3+ Thread IDs to target for sampling, starting at 0 (the main thread).
    %{INDENT}%   May be specified as index or range, e.g., '0 2-4' will be interpreted as:
    %{INDENT}%      sample the main thread (0), do not sample the first child thread but sample the 2nd, 3rd, and 4th child threads)";

    const auto* _realtime_desc =
        R"(Sample based on a real-clock timer. Accepts zero or more arguments:
    %{INDENT}%0. Enables sampling based on real-clock timer.
    %{INDENT}%1. Interrupts per second. E.g., 100 == sample every 10 milliseconds of realtime.
    %{INDENT}%2. Delay (in seconds of real-clock time). I.e., how long each thread should wait before taking first sample.
    %{INDENT}%3+ Thread IDs to target for sampling, starting at 0 (the main thread).
    %{INDENT}%   May be specified as index or range, e.g., '0 2-4' will be interpreted as:
    %{INDENT}%      sample the main thread (0), do not sample the first child thread but sample the 2nd, 3rd, and 4th child threads
    %{INDENT}%   When sampling with a real-clock timer, please note that enabling this will cause threads which are typically "idle"
    %{INDENT}%   to consume more resources since, while idle, the real-clock time increases (and therefore triggers taking samples)
    %{INDENT}%   whereas the CPU-clock time does not.)";

    const auto* _hsa_interrupt_desc =
        R"(Set the value of the HSA_ENABLE_INTERRUPT environment variable.
%{INDENT}%  ROCm version 5.2 and older have a bug which will cause a deadlock if a sample is taken while waiting for the signal
%{INDENT}%  that a kernel completed -- which happens when sampling with a real-clock timer. We require this option to be set to
%{INDENT}%  when --realtime is specified to make users aware that, while this may fix the bug, it can have a negative impact on
%{INDENT}%  performance.
%{INDENT}%  Values:
%{INDENT}%    0     avoid triggering the bug, potentially at the cost of reduced performance
%{INDENT}%    1     do not modify how ROCm is notified about kernel completion)";

    auto _realtime_reqs = (get_env("HSA_ENABLE_INTERRUPT", std::string{}, false).empty())
                              ? std::vector<std::string>{ "hsa-interrupt" }
                              : std::vector<std::string>{};

#if OMNITRACE_USE_ROCTRACER == 0 && OMNITRACE_USE_ROCPROFILER == 0
    _realtime_reqs.clear();
#endif

    const auto* _trace_policy_desc =
        R"(Policy for new data when the buffer size limit is reached:
    %{INDENT}%- discard     : new data is ignored
    %{INDENT}%- ring_buffer : new data overwrites oldest data)";

    auto _add_separator = [&](std::string _v, const std::string& _desc) {
        parser.add_argument({ "" }, "");
        parser
            .add_argument({ join("", "[", _v, "]") },
                          (_desc.empty()) ? _desc : join({ "", "(", ")" }, _desc))
            .color(tim::log::color::info());
        parser.add_argument({ "" }, "");
    };

    parser.enable_help();

    auto _cols = std::get<0>(tim::utility::console::get_columns());
    if(_cols > parser.get_help_width() + 8)
        parser.set_description_width(
            std::min<int>(_cols - parser.get_help_width() - 8, 120));

    _add_separator("DEBUG OPTIONS", "");
    parser.add_argument({ "--monochrome" }, "Disable colorized output")
        .max_count(1)
        .dtype("bool")
        .action([&](parser_t& p) {
            auto _colorized = !p.get<bool>("monochrome");
            p.set_use_color(_colorized);
            update_env(_env, "OMNITRACE_COLORIZED_LOG", (_colorized) ? "1" : "0");
            update_env(_env, "COLORIZED_LOG", (_colorized) ? "1" : "0");
        });
    parser.add_argument({ "--debug" }, "Debug output")
        .max_count(1)
        .action([&](parser_t& p) {
            update_env(_env, "OMNITRACE_DEBUG", p.get<bool>("debug"));
        });
    parser.add_argument({ "-v", "--verbose" }, "Verbose output")
        .count(1)
        .action([&](parser_t& p) {
            auto _v = p.get<int>("verbose");
            verbose = _v;
            update_env(_env, "OMNITRACE_VERBOSE", _v);
        });

    _add_separator("GENERAL OPTIONS", "");
    parser.add_argument({ "-c", "--config" }, "Configuration file")
        .min_count(0)
        .dtype("filepath")
        .action([&](parser_t& p) {
            update_env(
                _env, "OMNITRACE_CONFIG_FILE",
                join(array_config{ ":" }, p.get<std::vector<std::string>>("config")));
        });
    parser
        .add_argument({ "-o", "--output" },
                      "Output path. Accepts 1-2 parameters corresponding to the output "
                      "path and the output prefix")
        .min_count(1)
        .max_count(2)
        .action([&](parser_t& p) {
            auto _v = p.get<std::vector<std::string>>("output");
            update_env(_env, "OMNITRACE_OUTPUT_PATH", _v.at(0));
            if(_v.size() > 1) update_env(_env, "OMNITRACE_OUTPUT_PREFIX", _v.at(1));
        });
    parser
        .add_argument({ "-T", "--trace" }, "Generate a detailed trace (perfetto output)")
        .max_count(1)
        .action([&](parser_t& p) {
            update_env(_env, "OMNITRACE_USE_PERFETTO", p.get<bool>("trace"));
        });
    parser
        .add_argument(
            { "-P", "--profile" },
            "Generate a call-stack-based profile (conflicts with --flat-profile)")
        .max_count(1)
        .conflicts({ "flat-profile" })
        .action([&](parser_t& p) {
            update_env(_env, "OMNITRACE_USE_TIMEMORY", p.get<bool>("profile"));
        });
    parser
        .add_argument({ "-F", "--flat-profile" },
                      "Generate a flat profile (conflicts with --profile)")
        .max_count(1)
        .conflicts({ "profile" })
        .action([&](parser_t& p) {
            update_env(_env, "OMNITRACE_USE_TIMEMORY", p.get<bool>("flat-profile"));
            update_env(_env, "OMNITRACE_FLAT_PROFILE", p.get<bool>("flat-profile"));
        });
    parser
        .add_argument({ "-H", "--host" },
                      "Enable sampling host-based metrics for the process. E.g. CPU "
                      "frequency, memory usage, etc.")
        .max_count(1)
        .action([&](parser_t& p) {
            auto _h = p.get<bool>("host");
            auto _d = p.get<bool>("device");
            update_env(_env, "OMNITRACE_USE_PROCESS_SAMPLING", _h || _d);
            update_env(_env, "OMNITRACE_CPU_FREQ_ENABLED", _h);
        });
    parser
        .add_argument({ "-D", "--device" },
                      "Enable sampling device-based metrics for the process. E.g. GPU "
                      "temperature, memory usage, etc.")
        .max_count(1)
        .action([&](parser_t& p) {
            auto _h = p.get<bool>("host");
            auto _d = p.get<bool>("device");
            update_env(_env, "OMNITRACE_USE_PROCESS_SAMPLING", _h || _d);
            update_env(_env, "OMNITRACE_USE_ROCM_SMI", _d);
        });

    _add_separator("TRACING OPTIONS", "");
    parser
        .add_argument({ "--trace-file" },
                      "Specify the trace output filename. Relative filepath will be with "
                      "respect to output path and output prefix.")
        .count(1)
        .dtype("filepath")
        .action([&](parser_t& p) {
            update_env(_env, "OMNITRACE_PERFETTO_FILE", p.get<int64_t>("trace-file"));
        });
    parser
        .add_argument({ "--trace-buffer-size" },
                      "Size limit for the trace output (in KB)")
        .count(1)
        .dtype("KB")
        .action([&](parser_t& p) {
            update_env(_env, "OMNITRACE_PERFETTO_BUFFER_SIZE_KB",
                       p.get<int64_t>("trace-buffer-size"));
        });
    parser.add_argument({ "--trace-fill-policy" }, _trace_policy_desc)
        .count(1)
        .choices({ "discard", "ring_buffer" })
        .action([&](parser_t& p) {
            update_env(_env, "OMNITRACE_PERFETTO_FILL_POLICY",
                       p.get<std::string>("trace-fill-policy"));
        });

    _add_separator("PROFILE OPTIONS", "");
    parser.add_argument({ "--profile-format" }, "Data formats for profiling results")
        .min_count(1)
        .max_count(3)
        .requires({ "profile|flat-profile" })
        .choices({ "text", "json", "console" })
        .action([&](parser_t& p) {
            auto _v = p.get<std::set<std::string>>("profile");
            update_env(_env, "OMNITRACE_USE_TIMEMORY", true);
            if(!_v.empty())
            {
                update_env(_env, "OMNITRACE_TEXT_OUTPUT", _v.count("text") != 0);
                update_env(_env, "OMNITRACE_JSON_OUTPUT", _v.count("json") != 0);
                update_env(_env, "OMNITRACE_COUT_OUTPUT", _v.count("console") != 0);
            }
        });

    parser
        .add_argument({ "--profile-diff" },
                      "Generate a diff output b/t the profile collected and an existing "
                      "profile from another run Accepts 1-2 parameters corresponding to "
                      "the input path and the input prefix")
        .min_count(1)
        .max_count(2)
        .action([&](parser_t& p) {
            auto _v = p.get<std::vector<std::string>>("profile-diff");
            update_env(_env, "OMNITRACE_DIFF_OUTPUT", true);
            update_env(_env, "OMNITRACE_INPUT_PATH", _v.at(0));
            if(_v.size() > 1) update_env(_env, "OMNITRACE_INPUT_PREFIX", _v.at(1));
        });

    _add_separator("HOST/DEVICE (PROCESS SAMPLING) OPTIONS", "");
    parser
        .add_argument({ "--process-freq" },
                      "Set the default host/device sampling frequency "
                      "(number of interrupts per second)")
        .count(1)
        .action([&](parser_t& p) {
            update_env(_env, "OMNITRACE_PROCESS_SAMPLING_FREQ",
                       p.get<double>("process-freq"));
        });
    parser
        .add_argument({ "--process-wait" }, "Set the default wait time (i.e. delay) "
                                            "before taking first host/device sample "
                                            "(in seconds of realtime)")
        .count(1)
        .action([&](parser_t& p) {
            update_env(_env, "OMNITRACE_PROCESS_SAMPLING_DELAY",
                       p.get<double>("process-wait"));
        });
    parser
        .add_argument(
            { "--process-duration" },
            "Set the duration of the host/device sampling (in seconds of realtime)")
        .count(1)
        .action([&](parser_t& p) {
            update_env(_env, "OMNITRACE_SAMPLING_PROCESS_DURATION",
                       p.get<double>("process-duration"));
        });
    parser
        .add_argument({ "--cpus" },
                      "CPU IDs for frequency sampling. Supports integers and/or ranges")
        .dtype("int or range")
        .requires({ "host" })
        .action([&](parser_t& p) {
            update_env(
                _env, "OMNITRACE_PROCESS_SAMPLING_CPUS",
                join(array_config{ "," }, p.get<std::vector<std::string>>("cpus")));
        });
    parser
        .add_argument({ "--gpus" },
                      "GPU IDs for SMI queries. Supports integers and/or ranges")
        .dtype("int or range")
        .requires({ "device" })
        .action([&](parser_t& p) {
            update_env(
                _env, "OMNITRACE_PROCESS_SAMPLING_GPUS",
                join(array_config{ "," }, p.get<std::vector<std::string>>("gpus")));
        });

    _add_separator("GENERAL SAMPLING OPTIONS", "");
    parser
        .add_argument({ "-f", "--freq" }, "Set the default sampling frequency "
                                          "(number of interrupts per second)")
        .count(1)
        .action([&](parser_t& p) {
            update_env(_env, "OMNITRACE_SAMPLING_FREQ", p.get<double>("freq"));
        });
    parser
        .add_argument(
            { "-w", "--wait" },
            "Set the default wait time (i.e. delay) before taking first sample "
            "(in seconds). This delay time is based on the clock of the sampler, i.e., a "
            "delay of 1 second for CPU-clock sampler may not equal 1 second of realtime")
        .count(1)
        .action([&](parser_t& p) {
            update_env(_env, "OMNITRACE_SAMPLING_DELAY", p.get<double>("wait"));
        });
    parser
        .add_argument(
            { "-d", "--duration" },
            "Set the duration of the sampling (in seconds of realtime). I.e., it is "
            "possible (currently) to set a CPU-clock time delay that exceeds the "
            "real-time duration... resulting in zero samples being taken")
        .count(1)
        .action([&](parser_t& p) {
            update_env(_env, "OMNITRACE_SAMPLING_DURATION", p.get<double>("duration"));
        });
    parser
        .add_argument({ "-t", "--tids" },
                      "Specify the default thread IDs for sampling, where 0 (zero) is "
                      "the main thread and each thread created by the target application "
                      "is assigned an atomically incrementing value.")
        .min_count(1)
        .action([&](parser_t& p) {
            update_env(_env, "OMNITRACE_SAMPLING_TIDS",
                       join(array_config{ ", " }, p.get<std::vector<int64_t>>("tids")));
        });

    _add_separator("SAMPLING TIMER OPTIONS", "");
    parser.add_argument({ "--cputime" }, _cputime_desc)
        .min_count(0)
        .action([&](parser_t& p) {
            auto _v = p.get<std::deque<std::string>>("cputime");
            update_env(_env, "OMNITRACE_SAMPLING_CPUTIME", true);
            if(!_v.empty())
            {
                update_env(_env, "OMNITRACE_SAMPLING_CPUTIME_FREQ", _v.front());
                _v.pop_front();
            }
            if(!_v.empty())
            {
                update_env(_env, "OMNITRACE_SAMPLING_CPUTIME_DELAY", _v.front());
                _v.pop_front();
            }
            if(!_v.empty())
            {
                update_env(_env, "OMNITRACE_SAMPLING_CPUTIME_TIDS",
                           join(array_config{ "," }, _v));
            }
        });

    parser.add_argument({ "--realtime" }, _realtime_desc)
        .min_count(0)
        .requires(std::move(_realtime_reqs))
        .action([&](parser_t& p) {
            auto _v = p.get<std::deque<std::string>>("realtime");
            update_env(_env, "OMNITRACE_SAMPLING_REALTIME", true);
            if(!_v.empty())
            {
                update_env(_env, "OMNITRACE_SAMPLING_REALTIME_FREQ", _v.front());
                _v.pop_front();
            }
            if(!_v.empty())
            {
                update_env(_env, "OMNITRACE_SAMPLING_REALTIME_DELAY", _v.front());
                _v.pop_front();
            }
            if(!_v.empty())
            {
                update_env(_env, "OMNITRACE_SAMPLING_REALTIME_TIDS",
                           join(array_config{ "," }, _v));
            }
        });

    std::set<std::string> _backend_choices = { "all",         "kokkosp",     "mpip",
                                               "ompt",        "rcclp",       "rocm-smi",
                                               "roctracer",   "rocprofiler", "roctx",
                                               "mutex-locks", "spin-locks",  "rw-locks" };

#if !defined(OMNITRACE_USE_MPI) && !defined(OMNITRACE_USE_MPI_HEADERS)
    _backend_choices.erase("mpip");
#endif

#if !defined(OMNITRACE_USE_OMPT)
    _backend_choices.erase("ompt");
#endif

#if !defined(OMNITRACE_USE_RCCL)
    _backend_choices.erase("rcclp");
#endif

#if !defined(OMNITRACE_USE_ROCM_SMI)
    _backend_choices.erase("rocm-smi");
#endif

#if !defined(OMNITRACE_USE_ROCTRACER)
    _backend_choices.erase("roctracer");
    _backend_choices.erase("roctx");
#endif

#if !defined(OMNITRACE_USE_ROCPROFILER)
    _backend_choices.erase("rocprofiler");
#endif

    _add_separator("BACKEND OPTIONS", "These options control region information captured "
                                      "w/o sampling or instrumentation");
    parser.add_argument({ "-I", "--include" }, "Include data from these backends")
        .choices(_backend_choices)
        .action([&](parser_t& p) {
            auto _v      = p.get<std::set<std::string>>("include");
            auto _update = [&](const auto& _opt, bool _cond) {
                if(_cond || _v.count("all") > 0) update_env(_env, _opt, true);
            };
            _update("OMNITRACE_USE_KOKKOSP", _v.count("kokkosp") > 0);
            _update("OMNITRACE_USE_MPIP", _v.count("mpip") > 0);
            _update("OMNITRACE_USE_OMPT", _v.count("ompt") > 0);
            _update("OMNITRACE_USE_RCCLP", _v.count("rcclp") > 0);
            _update("OMNITRACE_USE_ROCTX", _v.count("roctx") > 0);
            _update("OMNITRACE_USE_ROCM_SMI", _v.count("rocm-smi") > 0);
            _update("OMNITRACE_USE_ROCTRACER", _v.count("roctracer") > 0);
            _update("OMNITRACE_USE_ROCPROFILER", _v.count("rocprofiler") > 0);
            _update("OMNITRACE_TRACE_THREAD_LOCKS", _v.count("mutex-locks") > 0);
            _update("OMNITRACE_TRACE_THREAD_RW_LOCKS", _v.count("rw-locks") > 0);
            _update("OMNITRACE_TRACE_THREAD_SPIN_LOCKS", _v.count("spin-locks") > 0);
        });

    parser.add_argument({ "-E", "--exclude" }, "Exclude data from these backends")
        .choices(_backend_choices)
        .action([&](parser_t& p) {
            auto _v      = p.get<std::set<std::string>>("exclude");
            auto _update = [&](const auto& _opt, bool _cond) {
                if(_cond || _v.count("all") > 0) update_env(_env, _opt, false);
            };
            _update("OMNITRACE_USE_KOKKOSP", _v.count("kokkosp") > 0);
            _update("OMNITRACE_USE_MPIP", _v.count("mpip") > 0);
            _update("OMNITRACE_USE_OMPT", _v.count("ompt") > 0);
            _update("OMNITRACE_USE_RCCLP", _v.count("rcclp") > 0);
            _update("OMNITRACE_USE_ROCTX", _v.count("roctx") > 0);
            _update("OMNITRACE_USE_ROCM_SMI", _v.count("rocm-smi") > 0);
            _update("OMNITRACE_USE_ROCTRACER", _v.count("roctracer") > 0);
            _update("OMNITRACE_USE_ROCPROFILER", _v.count("rocprofiler") > 0);
            _update("OMNITRACE_TRACE_THREAD_LOCKS", _v.count("mutex-locks") > 0);
            _update("OMNITRACE_TRACE_THREAD_RW_LOCKS", _v.count("rw-locks") > 0);
            _update("OMNITRACE_TRACE_THREAD_SPIN_LOCKS", _v.count("spin-locks") > 0);
        });

    _add_separator("HARDWARE COUNTER OPTIONS", "");
    parser
        .add_argument({ "-C", "--cpu-events" },
                      "Set the CPU hardware counter events to record (ref: "
                      "`omnitrace-avail -H -c CPU`)")
        .set_default(std::set<std::string>{})
        .action([&](parser_t& p) {
            auto _events =
                join(array_config{ "," }, p.get<std::vector<std::string>>("cpu-events"));
            update_env(_env, "OMNITRACE_PAPI_EVENTS", _events);
        });

#if defined(OMNITRACE_USE_ROCPROFILER)
    parser
        .add_argument({ "-G", "--gpu-events" },
                      "Set the GPU hardware counter events to record (ref: "
                      "`omnitrace-avail -H -c GPU`)")
        .set_default(std::set<std::string>{})
        .action([&](parser_t& p) {
            auto _events =
                join(array_config{ "," }, p.get<std::vector<std::string>>("gpu-events"));
            update_env(_env, "OMNITRACE_ROCM_EVENTS", _events);
        });
#endif

    _add_separator("MISCELLANEOUS OPTIONS", "");
    parser
        .add_argument({ "-i", "--inlines" },
                      "Include inline info in output when available")
        .max_count(1)
        .action([&](parser_t& p) {
            update_env(_env, "OMNITRACE_SAMPLING_INCLUDE_INLINES",
                       p.get<bool>("inlines"));
        });

    parser.add_argument({ "--hsa-interrupt" }, _hsa_interrupt_desc)
        .count(1)
        .dtype("int")
        .choices({ 0, 1 })
        .action([&](parser_t& p) {
            update_env(_env, "HSA_ENABLE_INTERRUPT", p.get<int>("hsa-interrupt"));
        });

    auto _inpv = std::vector<char*>{};
    auto _outv = std::vector<char*>{};
    bool _hash = false;
    for(int i = 0; i < argc; ++i)
    {
        if(_hash)
        {
            _outv.emplace_back(argv[i]);
        }
        else if(std::string_view{ argv[i] } == "--")
        {
            _hash = true;
        }
        else
        {
            _inpv.emplace_back(argv[i]);
        }
    }

    auto _cerr = parser.parse_args(_inpv.size(), _inpv.data());
    if(help_check(parser, argc, argv))
        help_action(parser);
    else if(_cerr)
        throw std::runtime_error(_cerr.what());

    if(parser.exists("realtime") && !parser.exists("cputime"))
        update_env(_env, "OMNITRACE_SAMPLING_CPUTIME", false);
    if(parser.exists("profile") && parser.exists("flat-profile"))
        throw std::runtime_error(
            "Error! '--profile' argument conflicts with '--flat-profile' argument");

    return _outv;
}