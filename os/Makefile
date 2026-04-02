OS_COMMON_HEADER := os/os_fileops.hpp

OS_HEADER = os/os_random.hpp

ifeq ($(OS),Windows_NT)
	OS_COMMON_SOURCE := os/windows/os_fileops.cpp
	OS_SOURCE := os/windows/os_random.cpp
else
	UNAME_S := $(shell uname -s)
	ifeq ($(UNAME_S),Darwin)
		OS_COMMON_SOURCE := os/osx/os_fileops.cpp
		OS_SOURCE := os/osx/os_random.cpp
	else
		OS_COMMON_SOURCE := os/linux/os_fileops.cpp
		OS_SOURCE := os/linux/os_random.cpp
	endif
endif