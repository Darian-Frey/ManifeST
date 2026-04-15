add_executable(test_metadata   test_metadata.cpp)
target_link_libraries(test_metadata   PRIVATE manifest_lib)
add_test(NAME test_metadata   COMMAND test_metadata)

add_executable(test_identifier test_identifier.cpp)
target_link_libraries(test_identifier PRIVATE manifest_lib)
add_test(NAME test_identifier COMMAND test_identifier)

add_executable(test_database   test_database.cpp)
target_link_libraries(test_database   PRIVATE manifest_lib)
add_test(NAME test_database   COMMAND test_database)
