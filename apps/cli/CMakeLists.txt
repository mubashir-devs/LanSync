add_executable(lansync-cli main.cpp)
target_link_libraries(lansync-cli PRIVATE lansync_app lansync_options)
