#ifndef NVSHMEM_TRANSFER_ENGINE_H_
#define NVSHMEM_TRANSFER_ENGINE_H_

#include <stddef.h>

// Initializes the NVSHMEM environment.
// Returns 0 on success, -1 on failure.
int nvshmem_engine_init();

// Finalizes the NVSHMEM environment.
void nvshmem_engine_finalize();

// Allocates a block of symmetric memory.
// Returns a pointer to the allocated memory, or NULL on failure.
void* nvshmem_engine_alloc(size_t size);

// Deallocates a block of symmetric memory.
void nvshmem_engine_free(void* ptr);

// Puts data from local memory to remote memory.
// Returns 0 on success, -1 on failure.
int nvshmem_engine_put(void* dest, const void* source, size_t size, int pe);

// Gets data from remote memory to local memory.
// Returns 0 on success, -1 on failure.
int nvshmem_engine_get(void* dest, const void* source, size_t size, int pe);

// Performs a barrier synchronization across all PEs.
void nvshmem_engine_barrier();

// Checks if the NVSHMEM engine has been initialized.
// Returns true if initialized, false otherwise.
// Note: This is an addition for better state checking, not part of standard NVSHMEM.
bool is_nvshmem_engine_globally_initialized();

#endif // NVSHMEM_TRANSFER_ENGINE_H_
