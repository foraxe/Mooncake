#include "nvshmem_transfer_engine.h"

#include <glog/logging.h>

#include <mpi.h>
#include <nvshmem.h>
#include <nvshmemx.h>

// Global flag guarding initialization state
static bool nvshmem_engine_globally_initialized = false;

int nvshmem_engine_init() {
    VLOG(0) << "nvshmem_engine_init called.";
    if (nvshmem_engine_globally_initialized) {
        VLOG(0) << "nvshmem_init has already been called.";
        return 0;
    }

    int mpi_initialized = 0;
    MPI_Initialized(&mpi_initialized);
    if (!mpi_initialized) {
        int provided = 0;
        MPI_Init_thread(nullptr, nullptr, MPI_THREAD_MULTIPLE, &provided);
    }

    MPI_Comm comm = MPI_COMM_WORLD;
    nvshmemx_init_attr_t attr = NVSHMEMX_INIT_ATTR_INITIALIZER;
    attr.mpi_comm = &comm;
    nvshmemx_init_attr(NVSHMEMX_INIT_WITH_MPI_COMM, &attr);

    LOG(INFO) << "NVSHMEM Engine Initialized.";
    nvshmem_engine_globally_initialized = true;
    return 0;
}

void nvshmem_engine_finalize() {
    VLOG(0) << "nvshmem_engine_finalize called.";
    if (!nvshmem_engine_globally_initialized) {
        return;
    }
    nvshmem_finalize();
    int mpi_finalized = 0;
    MPI_Finalized(&mpi_finalized);
    if (!mpi_finalized) {
        MPI_Finalize();
    }
    nvshmem_engine_globally_initialized = false;
    LOG(INFO) << "NVSHMEM Engine Finalized.";
}

// Function to check initialization status (for transfer_task.cpp)
bool is_nvshmem_engine_globally_initialized() {
    return nvshmem_engine_globally_initialized;
}

void* nvshmem_engine_alloc(size_t size) { return nvshmem_malloc(size); }

void nvshmem_engine_free(void* ptr) { nvshmem_free(ptr); }

int nvshmem_engine_put(void* dest, const void* source, size_t size, int pe) {
    nvshmem_putmem(dest, source, size, pe);
    return 0;
}

int nvshmem_engine_get(void* dest, const void* source, size_t size, int pe) {
    nvshmem_getmem(dest, source, size, pe);
    return 0;
}

void nvshmem_engine_barrier() { nvshmem_barrier_all(); }
