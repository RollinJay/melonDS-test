# melonDS-netplay-tool: headless determinism checks and measurements for rollback netplay

find_package(Threads REQUIRED)

add_executable(melonDS-netplay-tool
    NetplayTool.cpp
    OnlineTest.cpp
    HeadlessPlatform.cpp
)

target_link_libraries(melonDS-netplay-tool PRIVATE rollback-netplay core Threads::Threads)
