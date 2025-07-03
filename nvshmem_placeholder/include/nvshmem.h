// Placeholder nvshmem.h
#ifndef NVSHMEM_H_
#define NVSHMEM_H_

void nvshmem_init();
void nvshmem_finalize();
int nvshmem_my_pe();
int nvshmem_n_pes();
void *nvshmem_malloc(size_t size);
void nvshmem_free(void *ptr);
void nvshmem_putmem(void *dest, const void *source, size_t nelems, int pe);
void nvshmem_getmem(void *dest, const void *source, size_t nelems, int pe);
void nvshmem_barrier_all();

#endif // NVSHMEM_H_
