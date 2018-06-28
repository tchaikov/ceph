set(oyster_srcs
  main.cc
  OSD.cc
  OSDMapCache.cc)
add_executable(oyster ${oyster_srcs})
target_link_libraries(oyster crimson)
