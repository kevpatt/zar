#include "check.hpp"

#include <exception>
#include <string>

int main()
{
    using namespace zar::test;

    for (const auto& test : registry()) {
        const int before = failures;
        try {
            test.body();
        } catch (const std::exception& e) {
            fail(__FILE__, __LINE__, std::string("uncaught exception: ") + e.what());
        }
        std::cout << (failures == before ? "PASS " : "FAIL ") << test.name << '\n';
    }
    std::cout << registry().size() << " tests, " << failures << " failed checks\n";
    return failures ? 1 : 0;
}
