// Handles the host gives the game: small numbers that name host objects.
#pragma once
#include <stdint.h>

enum { HK_FREE, HK_FILE, HK_FIND, HK_THREAD };
uint64_t handle_alloc(int kind, void *data);   // INVALID_HANDLE if the table is full
void *handle_data(uint64_t handle, int kind);  // null unless the handle is live and of that kind
int handle_kind(uint64_t handle);
void handle_release(uint64_t handle);
