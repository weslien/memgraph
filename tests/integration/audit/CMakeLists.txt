set(target_name memgraph__integration__audit)
set(tester_target_name ${target_name}__tester)

add_executable(${tester_target_name} tester.cpp)
set_target_properties(${tester_target_name} PROPERTIES OUTPUT_NAME tester)
target_link_libraries(${tester_target_name} mg-communication nlohmann_json::nlohmann_json)
