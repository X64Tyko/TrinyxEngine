#pragma once

#include <functional>
#include <string>
#include <vector>
#include <iostream>

namespace Strigid::Testing
{
    class TestCase
    {
    public:
        std::string name;
        std::function<void(const StrigidEngine&)> testFunc;
        
        TestCase(std::string name, std::function<void(const StrigidEngine&)> func)
            : name(std::move(name)), testFunc(std::move(func)) {}
    };

    class TestRegistry
    {
    public:
        static TestRegistry& Instance()
        {
            static TestRegistry instance;
            return instance;
        }

        void RegisterTest(const std::string& name, std::function<void(const StrigidEngine&)> func)
        {
            tests.emplace_back(name, func);
        }

        int RunAll(const StrigidEngine& Engine)
        {
            int passed = 0;
            int failed = 0;

            std::cout << "\n=== Running Tests ===\n" << std::endl;

            for (const auto& test : tests)
            {
                std::cout << "Running: " << test.name << "... ";
                try
                {
                    test.testFunc(Engine);
                    std::cout << "PASSED" << std::endl;
                    passed++;
                }
                catch (const std::exception& e)
                {
                    std::cout << "FAILED\n  Error: " << e.what() << std::endl;
                    failed++;
                }
                catch (...)
                {
                    std::cout << "FAILED\n  Unknown error" << std::endl;
                    failed++;
                }
            }

            std::cout << "\n=== Test Results ===" << std::endl;
            std::cout << "Passed: " << passed << std::endl;
            std::cout << "Failed: " << failed << std::endl;
            std::cout << "Total:  " << tests.size() << std::endl;

            return failed;
        }

    private:
        std::vector<TestCase> tests;
    };

    // Helper for automatic test registration
    class TestRegistrar
    {
    public:
        TestRegistrar(const std::string& name, std::function<void(const StrigidEngine&)> func)
        {
            TestRegistry::Instance().RegisterTest(name, func);
        }
    };

    std::vector<EntityID> entityIDs;
}

// Macros for easy test definition
#define TEST(TestName) \
    void TestName(const StrigidEngine& Engine); \
    static Strigid::Testing::TestRegistrar TestName##_registrar(#TestName, TestName); \
    void TestName(const StrigidEngine& Engine)

#define ASSERT(condition) \
    if (!(condition)) throw std::runtime_error("Assertion failed: " #condition)

#define ASSERT_EQ(a, b) \
    if ((a) != (b)) throw std::runtime_error("Assertion failed: " #a " == " #b)

#define ASSERT_NE(a, b) \
    if ((a) == (b)) throw std::runtime_error("Assertion failed: " #a " != " #b)
