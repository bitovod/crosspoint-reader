add_executable(PdbReaderTest
  PdbReaderTest.cpp
  ${REPO_ROOT}/lib/Txt/PdbReader.cpp
)

target_include_directories(PdbReaderTest PRIVATE
  ${REPO_ROOT}/lib/Txt
)

target_link_libraries(PdbReaderTest PRIVATE
  crosspoint_test_common
  GTest::gtest_main
)

gtest_discover_tests(PdbReaderTest)
