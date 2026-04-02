#pragma once

#if defined(_WIN32)
#if defined(RSPCLIENT_STATIC)
#define RSPCLIENT_API
#elif defined(RSPCLIENT_BUILD_DLL)
#define RSPCLIENT_API __declspec(dllexport)
#else
#define RSPCLIENT_API __declspec(dllimport)
#endif
#elif defined(__GNUC__) && defined(RSPCLIENT_BUILD_DLL)
#define RSPCLIENT_API __attribute__((visibility("default")))
#else
#define RSPCLIENT_API
#endif