#include "nvshmem_transfer_engine.h"
#include "nvshmem.h" // Using the placeholder nvshmem.h for now

#include <stdio.h> // For printf in case of errors
#include <glog/logging.h> // For VLOG

// Global flag for testing purposes
bool nvshmem_engine_globally_initialized = false;

int nvshmem_engine_init() {
    VLOG(0) << "nvshmem_engine_init called.";
    // In a real scenario, nvshmem_init(); might not return a status.
    // We'll assume it initializes correctly or handles errors internally/aborts.
    // For now, as our placeholder nvshmem_init is void, we'll just call it.
    nvshmem_init();

    // Simulate PE detection for placeholder
    // In real NVSHMEM, nvshmem_n_pes() would be valid after nvshmem_init().
    // Our placeholder nvshmem.h doesn't implement a backing for nvshmem_n_pes() state.
    // For testing purposes, let's assume init is successful if it doesn't crash.
    // We could add checks here if nvshmem_n_pes() or nvshmem_my_pe() return
    // indicative error values, though standard OpenSHMEM/NVSHMEM typically
    // doesn't return error codes from init but might abort on critical failure.

    // Placeholder check:
    // if (nvshmem_n_pes() <= 0) {
    //     LOG(ERROR) << "NVSHMEM engine initialization failed: Invalid number of PEs detected by placeholder.";
    //     nvshmem_engine_globally_initialized = false;
    //     return -1;
    // }

    LOG(INFO) << "NVSHMEM Engine (Placeholder) Initialized.";
    nvshmem_engine_globally_initialized = true;
    return 0; // Assuming success
}

void nvshmem_engine_finalize() {
    VLOG(0) << "nvshmem_engine_finalize called.";
    nvshmem_finalize();
    nvshmem_engine_globally_initialized = false;
    LOG(INFO) << "NVSHMEM Engine (Placeholder) Finalized.";
}

// Function to check initialization status (for transfer_task.cpp)
bool is_nvshmem_engine_globally_initialized() {
    return nvshmem_engine_globally_initialized;
}

void* nvshmem_engine_alloc(size_t size) {
    return nvshmem_malloc(size);
}

void nvshmem_engine_free(void* ptr) {
    nvshmem_free(ptr);
}

int nvshmem_engine_put(void* dest, const void* source, size_t size, int pe) {
    // NVSHMEM put operations are typically `void` and assume success or handle errors
    // via mechanisms like error handlers or program termination on critical failure.
    // For a simple API, we'll wrap the call. If specific error checking is needed,
    // it would depend on the NVSHMEM version and configuration.
    nvshmem_putmem(dest, source, size, pe);
    // Assuming the operation is successful if it doesn't abort.
    // Real error handling might involve checking NVSHMEM error states if available,
    // or relying on its error handling mechanisms.
    return 0; // Placeholder success
}

int nvshmem_engine_get(void* dest, const void* source, size_t size, int pe) {
    // Similar to put, get operations are often `void`.
    nvshmem_getmem(dest, source, size, pe);
    // Assuming success.
    return 0; // Placeholder success
}

void nvshmem_engine_barrier() {
    nvshmem_barrier_all();
}
