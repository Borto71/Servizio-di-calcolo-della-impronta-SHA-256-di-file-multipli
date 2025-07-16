cmake_minimum_required(VERSION 3.10)
project(HashServer C)

find_package(OpenSSL REQUIRED)

add_executable(server server.c)
target_link_libraries(server OpenSSL::Crypto)

# Copia il binario server nella cartella principale dopo la compilazione
add_custom_command(TARGET server POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy $<TARGET_FILE:server> ${CMAKE_SOURCE_DIR}/server)
