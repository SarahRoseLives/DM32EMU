add_executable(test_virtual_rf test_virtual_rf.cpp)
target_link_libraries(test_virtual_rf PRIVATE dm32core)
add_test(NAME virtual_rf COMMAND test_virtual_rf)

add_executable(test_modem test_modem.cpp)
target_link_libraries(test_modem PRIVATE dm32core)
add_test(NAME modem COMMAND test_modem)

add_executable(test_codec_backup test_codec_backup.cpp)
target_link_libraries(test_codec_backup PRIVATE dm32core)
add_test(NAME codec_backup COMMAND test_codec_backup)

add_executable(test_codec_fixture test_codec_fixture.cpp)
target_link_libraries(test_codec_fixture PRIVATE dm32core)
add_test(NAME codec_fixture COMMAND test_codec_fixture)
set_tests_properties(codec_fixture PROPERTIES SKIP_RETURN_CODE 77)

add_executable(test_firmware_dual_rf test_firmware_dual_rf.cpp)
target_link_libraries(test_firmware_dual_rf PRIVATE dm32core)
add_test(NAME firmware_dual_rf COMMAND test_firmware_dual_rf)
set_tests_properties(firmware_dual_rf PROPERTIES SKIP_RETURN_CODE 77 TIMEOUT 240)
