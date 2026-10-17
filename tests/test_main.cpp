#include "test.hpp"

#include <cstdlib>
#include <iostream>

void test_json_rpc();
void test_versions();
void test_schema_contract();

int main() {
    test_json_rpc();
    test_versions();
    test_schema_contract();
    if (cppmcp::test::failures != 0) {
        return EXIT_FAILURE;
    }
    std::cout << "All cppmcp foundation tests passed.\n";
    return EXIT_SUCCESS;
}
