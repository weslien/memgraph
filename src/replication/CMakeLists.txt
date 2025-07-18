add_library(mg-replication STATIC)
add_library(mg::replication ALIAS mg-replication)
target_sources(mg-replication
        PUBLIC
        FILE_SET HEADERS
        BASE_DIRS include
        FILES
        include/replication/state.hpp
        include/replication/statefwd.hpp
        include/replication/epoch.hpp
        include/replication/config.hpp
        include/replication/status.hpp
        include/replication/replication_client.hpp
        include/replication/replication_server.hpp

        PRIVATE
        state.cpp
        epoch.cpp
        config.cpp
        status.cpp
        replication_client.cpp
        replication_server.cpp
)

find_package(fmt REQUIRED)
target_link_libraries(mg-replication
    PUBLIC mg::utils mg::kvstore nlohmann_json::nlohmann_json mg::rpc mg::slk mg::io mg::repl_coord_glue mg-flags
    PRIVATE fmt::fmt
)
