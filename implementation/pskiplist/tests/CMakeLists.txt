cmake_minimum_required(VERSION 3.16)

add_executable(test_pskiplist
  test_main.cpp
  test_basic.cpp
  test_concurrent.cpp
)
target_link_libraries(test_pskiplist PRIVATE pskiplist pskiplist_warnings doctest::doctest)

add_test(NAME test_pskiplist COMMAND test_pskiplist --duration=true)
set_tests_properties(test_pskiplist PROPERTIES TIMEOUT 120)
