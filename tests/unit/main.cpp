#include <pthread.h>

#include "TestFramework.hpp"

namespace {
    struct Run {
        std::string filter;
        int failed = 0;
    };

    void* run_tests(void* p) {
        auto* run = static_cast<Run*>(p);
        int count = 0;
        for (const auto& test : vtest::registry()) {
            if (!run->filter.empty() && test.name.find(run->filter) == std::string::npos) continue;
            count++;
            vtest::current_failures() = 0;
            try {
                test.fn();
            } catch (const std::exception& e) {
                vtest::report(__FILE__, __LINE__, "unexpected exception", std::string(": ") + e.what());
            }
            if (vtest::current_failures() > 0) {
                run->failed++;
                std::cerr << "[FAIL] " << test.name << "\n";
            } else {
                std::cout << "[ ok ] " << test.name << "\n";
            }
        }
        std::cout << "\n" << count - run->failed << "/" << count << " unit tests passed\n";
        return nullptr;
    }
} // namespace

int main(int argc, char** argv) {
    Run run;
    run.filter = argc > 1 ? argv[1] : "";
    // Like vulpc itself, run on a 1 GiB stack: some tests build very deeply nested programs.
    pthread_attr_t attr;
    pthread_t thread;
    if (pthread_attr_init(&attr) == 0 && pthread_attr_setstacksize(&attr, static_cast<size_t>(1) << 30) == 0
        && pthread_create(&thread, &attr, run_tests, &run) == 0) {
        pthread_join(thread, nullptr);
    } else {
        run_tests(&run);
    }
    return run.failed == 0 ? 0 : 1;
}
