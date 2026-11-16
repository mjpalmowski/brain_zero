#pragma once
using BaseType_t = int;
using TaskHandle_t = void *;
enum eTaskState { eRunning, eReady, eBlocked, eSuspended, eDeleted, eInvalid };
constexpr BaseType_t pdPASS = 1;
