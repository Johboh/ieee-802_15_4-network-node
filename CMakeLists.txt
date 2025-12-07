FILE(GLOB_RECURSE sources "./src/impl/*.*")

set(required_components nvs_flash)

idf_component_register(COMPONENT_NAME "ieee-802_15_4-network-node"
                        SRCS ${sources}
                        INCLUDE_DIRS "./src/"
                        REQUIRES ${required_components})
