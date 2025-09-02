#ifndef NVSHMEM_TRANSPORT_H_
#define NVSHMEM_TRANSPORT_H_

#include <memory>
#include <string>
#include <vector>

#include "nvshmem_transfer_engine.h"
#include "transport/transport.h"

namespace mooncake {

class NvshmemTransport : public Transport {
   public:
    NvshmemTransport();
    ~NvshmemTransport();

    Status submitTransfer(BatchID batch_id,
                          const std::vector<TransferRequest>& entries) override;

    Status submitTransferTask(
        const std::vector<TransferRequest*>& request_list,
        const std::vector<TransferTask*>& task_list) override;

    Status getTransferStatus(BatchID batch_id, size_t task_id,
                             TransferStatus& status) override;

    static void* allocatePinnedLocalMemory(size_t length);
    static void freePinnedLocalMemory(void* addr);

   protected:
    int install(std::string& local_server_name,
                std::shared_ptr<TransferMetadata> meta,
                std::shared_ptr<Topology> topo) override;

    int registerLocalMemory(void* addr, size_t length,
                            const std::string& location, bool remote_accessible,
                            bool update_metadata = true) override;

    int unregisterLocalMemory(void* addr, bool update_metadata = true) override;

    int registerLocalMemoryBatch(const std::vector<BufferEntry>& buffer_list,
                                 const std::string& location) override;

    int unregisterLocalMemoryBatch(
        const std::vector<void*>& addr_list) override;

    const char* getName() const override { return "nvshmem"; }
};

}  // namespace mooncake

#endif  // NVSHMEM_TRANSPORT_H_
