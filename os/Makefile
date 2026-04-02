OS_HEADER = os/os_random.hpp

ifeq ($(OS),Windows_NT)
	OS_SOURCE := os/windows/os_random.cpp
else
	UNAME_S := $(shell uname -s)
	ifeq ($(UNAME_S),Darwin)
		OS_SOURCE := os/osx/os_random.cpp
	else
		OS_SOURCE := os/linux/os_random.cpp
	endif
endif