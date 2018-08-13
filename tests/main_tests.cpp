#include <gtest/gtest.h>

int main(int argc, char** argv) {
    testing::InitGoogleTest(&argc, argv);
    RUN_ALL_TESTS();
#ifdef _MSC_VER
    std::cout << "[press enter]" << std::endl;
    std::getchar(); // keep console window open until Return keystroke
#endif
    return 0;
}
