add_executable(make)
add_compile_definitions(unix _MINIX _POSIX_SOURCE)
target_sources(make PRIVATE check.c input.c macro.c main.c make.c reader.c rules.c archive.c)

target_include_directories(
  make
  PUBLIC ${CMAKE_BINARY_DIR}/include)