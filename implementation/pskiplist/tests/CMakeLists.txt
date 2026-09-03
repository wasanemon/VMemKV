cmake_minimum_required(VERSION 3.16)

add_executable(test_pskiplist
  test_main.cpp
  normal/test_functional.cpp
  normal/test_concurrent.cpp
  normal/test_mmap_storage.cpp
  error/test_functional.cpp
)
target_include_directories(test_pskiplist PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(test_pskiplist PRIVATE pskiplist pskiplist_warnings doctest::doctest)

add_test(NAME test_pskiplist COMMAND test_pskiplist --duration=true)
set_tests_properties(test_pskiplist PROPERTIES TIMEOUT 120)
