cmake_minimum_required(VERSION 3.10)

set(SOURCES
    mqtt.c
    pack.c
)

add_executable(mqtt ${SOURCES})

