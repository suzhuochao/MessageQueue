
cmake_minimum_required(VERSION 2.8)

project(myq_project)
SET( CMAKE_CXX_FLAGS_DEBUG  "${CMAKE_CXX_FLAGS_DEBUG} -m64 -DDEBUG -std=c++11   -DPICOJSON_USE_INT64 -O0  -g " )
SET( CMAKE_CXX_FLAGS_RELEASE  "${CMAKE_CXX_FLAGS_RELEASE} -m64 -DNDEBUG -std=c++11 -Wall -Wextra -Wfloat-equal -Wundef -Wcast-align -Wwrite-strings -Wlogical-op -Wmissing-declarations -Wredundant-decls -Wshadow -Woverloaded-virtual -Wformat=2  -DPICOJSON_USE_INT64 -O3  -g " )
SET(CMAKE_BUILD_TYPE debug)
include_directories(/usr/local/include include include/thirdparty /opt/zeromq/include)
link_directories( /opt/zeromq/lib /usr/local/opt/zlib/lib)
link_directories(/root/code/cpp/libzmq-master/build/lib)

add_library(myq SHARED src/myq_api.cpp)
TARGET_LINK_LIBRARIES(myq zmq)
install(TARGETS myq DESTINATION ${CMAKE_CURRENT_SOURCE_DIR}/examples/lib)
install(TARGETS myq DESTINATION ${CMAKE_CURRENT_SOURCE_DIR}/bin)
install(FILES ${CMAKE_CURRENT_SOURCE_DIR}/include/myq_api.h DESTINATION ${CMAKE_CURRENT_SOURCE_DIR}/examples/include)

add_executable(myq-test examples/myq-test.cpp)
target_link_libraries(myq-test zmq z)
install(TARGETS myq-test DESTINATION ${CMAKE_CURRENT_SOURCE_DIR}/bin)

SET( CMAKE_C_FLAGS_DEBUG  "${CMAKE_C_FLAGS_DEBUG} -m64 -DDEBUG -std=c11   -O0  -g " )
SET( CMAKE_C_FLAGS_RELEASE  "${CMAKE_C_FLAGS_RELEASE} -m64 -DNDEBUG -std=c11 -Wall -Wextra -Wfloat-equal -Wundef -Wcast-align -Wwrite-strings -Wlogical-op -Wmissing-declarations -Wredundant-decls -Wshadow  -Wformat=2  -O3  -g " )
find_package (Threads)
link_directories( ${PROJECT_BINARY_DIR}/lib )

add_executable(myq-broker examples/myq-broker.c)
target_link_libraries(myq-broker myq zmq pthread )
install(TARGETS myq-broker DESTINATION ${CMAKE_CURRENT_SOURCE_DIR}/bin)

add_executable(myq-consumer examples/myq-consumer.c)
target_link_libraries(myq-consumer myq zmq pthread)
install(TARGETS myq-consumer DESTINATION ${CMAKE_CURRENT_SOURCE_DIR}/bin)

add_executable(myq-producer examples/myq-producer.c)
target_link_libraries(myq-producer myq zmq pthread)
install(TARGETS myq-producer DESTINATION ${CMAKE_CURRENT_SOURCE_DIR}/bin)

add_executable(myq-topic examples/myq-topic.c)
target_link_libraries(myq-topic myq zmq pthread)
install(TARGETS myq-topic DESTINATION ${CMAKE_CURRENT_SOURCE_DIR}/bin)




