// gtest main for catkin_add_gtest (catkin links gtest without gtest_main).
#include <gtest/gtest.h>

int main(int argc, char ** argv)
{
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
