cmake_minimum_required(VERSION 3.13)
project(digimon_emulator)

include(${FLIPPER_LIBRARIES})

flipper_app(digimon
    SOURCES
        src/main.c
)
