#pragma once

#include <functional>
#include <string>
#include <string_view>
#include <vector>
#include <iostream>
#include <stdexcept>

class TrinyxEngine; // forward decl — full type only needed at call sites

namespace tnx::Testing
{
    // -----------------------------------------------------------------------
    // Result of a single test run.
    // -----------------------------------------------------------------------
    enum class TestStatus { Passed, Failed, Skipped };

    struct TestResult
    {
        std::string Name;
        TestStatus  Status = TestStatus::Passed;
        std::string Message; // failure reason or skip reason
    };

    // -----------------------------------------------------------------------
    // Signals that a test was intentionally skipped (e.g. wrong build config).
    // Caught by the runner — not a failure.
    // -----------------------------------------------------------------------
    struct TestSkipped
    {
        std::string Reason;
        explicit TestSkipped(std::string reason) : Reason(std::move(reason)) {}
    };

    // -----------------------------------------------------------------------
    // Internal helpers used by both registry RunFiltered implementations.
    // -----------------------------------------------------------------------
    inline bool IsSelected(const std::string& name, const std::vector<std::string>& filter)
    {
        for (const auto& f : filter)
            if (name == f) return true;
        return false;
    }

    inline void PrintSummary(int passed, int failed, int skipped, const char* label)
    {
        std::cout << "\n--- " << label << " Results ---\n";
        std::cout << "  Passed:  " << passed  << "\n";
        std::cout << "  Failed:  " << failed  << "\n";
        std::cout << "  Skipped: " << skipped << "\n";
        std::cout << "  Total:   " << (passed + failed + skipped) << "\n";
    }

    // -----------------------------------------------------------------------
    // Pre-engine-loop tests (TestRegistry)
    // Run inside PostInitialize, before the main loop starts.
    // -----------------------------------------------------------------------
    class TestRegistry
    {
    public:
        static TestRegistry& Instance()
        {
            static TestRegistry instance;
            return instance;
        }

        void Register(const std::string& name, std::function<void(const TrinyxEngine&)> func)
        {
            tests.emplace_back(name, std::move(func));
        }

        // Run all tests whose name appears in `filter`.
        // If `filter` is empty, all registered tests run.
        // Returns number of failures (0 = success).
        int RunFiltered(const TrinyxEngine& engine, const std::vector<std::string>& filter)
        {
            int passed  = 0;
            int failed  = 0;
            int skipped = 0;

            std::cout << "\n=== Pre-loop Tests ===\n";
            for (const auto& [name, fn] : tests)
            {
                if (!filter.empty() && !IsSelected(name, filter))
                    continue;

                std::cout << "  " << name << " ... ";
                std::cout.flush();
                try
                {
                    fn(engine);
                    std::cout << "PASSED\n";
                    ++passed;
                }
                catch (const TestSkipped& s)
                {
                    std::cout << "SKIPPED (" << s.Reason << ")\n";
                    ++skipped;
                }
                catch (const std::exception& e)
                {
                    std::cout << "FAILED\n    " << e.what() << "\n";
                    ++failed;
                }
                catch (...)
                {
                    std::cout << "FAILED (unknown exception)\n";
                    ++failed;
                }
            }
            PrintSummary(passed, failed, skipped, "Pre-loop");
            return failed;
        }

        // List all registered test names (for --list-tests).
        void ListTests() const
        {
            for (const auto& [name, _] : tests)
                std::cout << "  [pre]  " << name << "\n";
        }

    private:
        std::vector<std::pair<std::string, std::function<void(const TrinyxEngine&)>>> tests;
    };

    // -----------------------------------------------------------------------
    // Runtime tests (RuntimeTestRegistry)
    // Run inside PostStart, after threads + jobs are active.
    // -----------------------------------------------------------------------
    class RuntimeTestRegistry
    {
    public:
        static RuntimeTestRegistry& Instance()
        {
            static RuntimeTestRegistry instance;
            return instance;
        }

        void Register(const std::string& name, std::function<void(TrinyxEngine&)> func)
        {
            tests.emplace_back(name, std::move(func));
        }

        // Returns number of failures.
        int RunFiltered(TrinyxEngine& engine, const std::vector<std::string>& filter)
        {
            if (tests.empty()) return 0;

            int passed  = 0;
            int failed  = 0;
            int skipped = 0;

            std::cout << "\n=== Runtime Tests ===\n";
            for (const auto& [name, fn] : tests)
            {
                if (!filter.empty() && !IsSelected(name, filter))
                    continue;

                std::cout << "  " << name << " ... ";
                std::cout.flush();
                try
                {
                    fn(engine);
                    std::cout << "PASSED\n";
                    ++passed;
                }
                catch (const TestSkipped& s)
                {
                    std::cout << "SKIPPED (" << s.Reason << ")\n";
                    ++skipped;
                }
                catch (const std::exception& e)
                {
                    std::cout << "FAILED\n    " << e.what() << "\n";
                    ++failed;
                }
                catch (...)
                {
                    std::cout << "FAILED (unknown exception)\n";
                    ++failed;
                }
            }
            PrintSummary(passed, failed, skipped, "Runtime");
            return failed;
        }

        void ListTests() const
        {
            for (const auto& [name, _] : tests)
                std::cout << "  [runtime]  " << name << "\n";
        }

    private:
        std::vector<std::pair<std::string, std::function<void(TrinyxEngine&)>>> tests;
    };

    // -----------------------------------------------------------------------
    // Auto-registrar helpers (used by the TEST / RUNTIME_TEST macros)
    // -----------------------------------------------------------------------
    struct TestRegistrar
    {
        TestRegistrar(const std::string& name, std::function<void(const TrinyxEngine&)> fn)
        {
            TestRegistry::Instance().Register(name, std::move(fn));
        }
    };

    struct RuntimeTestRegistrar
    {
        RuntimeTestRegistrar(const std::string& name, std::function<void(TrinyxEngine&)> fn)
        {
            RuntimeTestRegistry::Instance().Register(name, std::move(fn));
        }
    };

    // -----------------------------------------------------------------------
    // Print both registries' test names.
    // -----------------------------------------------------------------------
    inline void ListAllTests()
    {
        std::cout << "\nRegistered tests:\n";
        TestRegistry::Instance().ListTests();
        RuntimeTestRegistry::Instance().ListTests();
    }

} // namespace tnx::Testing

// ---------------------------------------------------------------------------
// Macros
// ---------------------------------------------------------------------------

// Pre-engine-loop test. Receives const TrinyxEngine& Engine.
#define TEST(TestName) \
    static void TestName##_Impl(const TrinyxEngine& Engine); \
    static tnx::Testing::TestRegistrar TestName##_registrar(#TestName, TestName##_Impl); \
    static void TestName##_Impl(const TrinyxEngine& Engine)

// Runtime test. Receives TrinyxEngine& Engine. Runs after threads + jobs are active.
#define RUNTIME_TEST(TestName) \
    static void TestName##_Impl(TrinyxEngine& Engine); \
    static tnx::Testing::RuntimeTestRegistrar TestName##_runtime_registrar(#TestName, TestName##_Impl); \
    static void TestName##_Impl(TrinyxEngine& Engine)

// Throw to signal intentional skip (not a failure).
// Usage: SKIP_TEST("Needs TNX_ENABLE_NETWORK")
#define SKIP_TEST(reason) throw tnx::Testing::TestSkipped(reason)

// Assertion macros
#define ASSERT(condition) \
    if (!(condition)) throw std::runtime_error("Assertion failed: " #condition)

#define ASSERT_EQ(a, b) \
    if ((a) != (b)) throw std::runtime_error("Assertion failed: " #a " == " #b)

#define ASSERT_NE(a, b) \
    if ((a) == (b)) throw std::runtime_error("Assertion failed: " #a " != " #b)

