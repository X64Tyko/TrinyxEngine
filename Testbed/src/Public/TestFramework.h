#pragma once

#include <functional>
#include <string>
#include <vector>
#include <iostream>

namespace tnx::Testing
{
    class TestCase
    {
    public:
        std::string name;
        std::function<void(const TrinyxEngine&)> testFunc;
        
        TestCase(std::string name, std::function<void(const TrinyxEngine&)> func)
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

        void RegisterTest(const std::string& name, std::function<void(const TrinyxEngine&)> func)
        {
            tests.emplace_back(name, func);
        }

        int RunAll(const TrinyxEngine& Engine)
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
        TestRegistrar(const std::string& name, std::function<void(const TrinyxEngine&)> func)
        {
            TestRegistry::Instance().RegisterTest(name, func);
        }
    };

    std::vector<EntityID> entityIDs;

	// Runtime test registry — tests that run after the engine loop is active
	// (Logic, Render, and Jobs are all running).
	class RuntimeTestRegistry
	{
	public:
		static RuntimeTestRegistry& Instance()
		{
			static RuntimeTestRegistry instance;
			return instance;
		}

		void RegisterTest(const std::string& name, std::function<void(TrinyxEngine&)> func)
		{
			tests.emplace_back(name, std::move(func));
		}

		int RunAll(TrinyxEngine& engine)
		{
			if (tests.empty()) return 0;

			int passed = 0;
			int failed = 0;

			std::cout << "\n=== Running Runtime Tests ===\n" << std::endl;

			for (const auto& test : tests)
			{
				std::cout << "Running: " << test.first << "... ";
				try
				{
					test.second(engine);
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

			std::cout << "\n=== Runtime Test Results ===" << std::endl;
			std::cout << "Passed: " << passed << std::endl;
			std::cout << "Failed: " << failed << std::endl;
			std::cout << "Total:  " << tests.size() << std::endl;

			return failed;
		}

	private:
		std::vector<std::pair<std::string, std::function<void(TrinyxEngine&)>>> tests;
	};

	class RuntimeTestRegistrar
	{
	public:
		RuntimeTestRegistrar(const std::string& name, std::function<void(TrinyxEngine&)> func)
		{
			RuntimeTestRegistry::Instance().RegisterTest(name, std::move(func));
		}
	};
}

// Macros for easy test definition
#define TEST(TestName) \
    void TestName(const TrinyxEngine& Engine); \
    static tnx::Testing::TestRegistrar TestName##_registrar(#TestName, TestName); \
    void TestName(const TrinyxEngine& Engine)

// Runtime tests — run after the engine loop is active (threads + jobs running)
#define RUNTIME_TEST(TestName) \
    void TestName(TrinyxEngine& Engine); \
    static tnx::Testing::RuntimeTestRegistrar TestName##_runtime_registrar(#TestName, TestName); \
    void TestName(TrinyxEngine& Engine)

#define ASSERT(condition) \
    if (!(condition)) throw std::runtime_error("Assertion failed: " #condition)

#define ASSERT_EQ(a, b) \
    if ((a) != (b)) throw std::runtime_error("Assertion failed: " #a " == " #b)

#define ASSERT_NE(a, b) \
    if ((a) == (b)) throw std::runtime_error("Assertion failed: " #a " != " #b)
