#include "transport/nvshmem_transport/nvshmem_transport.h"

#include <glog/logging.h>

namespace mooncake {

NvshmemTransport::NvshmemTransport() { nvshmem_engine_init(); }

NvshmemTransport::~NvshmemTransport() { nvshmem_engine_finalize(); }

Status NvshmemTransport::submitTransfer(
    BatchID batch_id, const std::vector<TransferRequest>& entries) {
    auto& batch_desc = *((BatchDesc*)(batch_id));
    if (batch_desc.task_list.size() + entries.size() > batch_desc.batch_size) {
        return Status::InvalidArgument(
            "NvshmemTransport: Exceed the limitation of capacity");
    }
    size_t task_id = batch_desc.task_list.size();
    batch_desc.task_list.resize(task_id + entries.size());
    for (const auto& request : entries) {
        TransferTask& task = batch_desc.task_list[task_id++];
        Slice* slice = getSliceCache().allocate();
        slice->source_addr = request.source;
        slice->length = request.length;
        slice->opcode = request.opcode;
        slice->target_id = request.target_id;
        slice->status = Slice::PENDING;
        slice->task = &task;
        __sync_fetch_and_add(&task.slice_count, 1);
        int rc = 0;
        if (request.opcode == TransferRequest::READ) {
            rc = nvshmem_engine_get(
                slice->source_addr,
                reinterpret_cast<const void*>(request.target_offset),
                request.length, request.target_id);
        } else {
            rc = nvshmem_engine_put(
                reinterpret_cast<void*>(request.target_offset),
                slice->source_addr, request.length, request.target_id);
        }
        if (rc == 0)
            slice->markSuccess();
        else
            slice->markFailed();
    }
    return Status::OK();
}

Status NvshmemTransport::submitTransferTask(
    const std::vector<TransferRequest*>& request_list,
    const std::vector<TransferTask*>& task_list) {
    for (size_t i = 0; i < request_list.size(); ++i) {
        const auto& request = *request_list[i];
        auto& task = *task_list[i];
        Slice* slice = getSliceCache().allocate();
        slice->source_addr = request.source;
        slice->length = request.length;
        slice->opcode = request.opcode;
        slice->target_id = request.target_id;
        slice->status = Slice::PENDING;
        slice->task = &task;
        task.slice_list.push_back(slice);
        __sync_fetch_and_add(&task.slice_count, 1);
        int rc = 0;
        if (request.opcode == TransferRequest::READ) {
            rc = nvshmem_engine_get(
                slice->source_addr,
                reinterpret_cast<const void*>(request.target_offset),
                request.length, request.target_id);
        } else {
            rc = nvshmem_engine_put(
                reinterpret_cast<void*>(request.target_offset),
                slice->source_addr, request.length, request.target_id);
        }
        if (rc == 0)
            slice->markSuccess();
        else
            slice->markFailed();
    }
    return Status::OK();
}

Status NvshmemTransport::getTransferStatus(BatchID batch_id, size_t task_id,
                                           TransferStatus& status) {
    auto& batch_desc = *((BatchDesc*)(batch_id));
    const size_t task_count = batch_desc.task_list.size();
    if (task_id >= task_count) {
        return Status::InvalidArgument(
            "NvshmemTransport::getTransferStatus invalid argument");
    }
    auto& task = batch_desc.task_list[task_id];
    status.transferred_bytes = task.transferred_bytes;
    uint64_t success_slice_count = task.success_slice_count;
    uint64_t failed_slice_count = task.failed_slice_count;
    if (success_slice_count + failed_slice_count == task.slice_count) {
        if (failed_slice_count) {
            status.s = TransferStatusEnum::FAILED;
        } else {
            status.s = TransferStatusEnum::COMPLETED;
        }
        task.is_finished = true;
    } else {
        status.s = TransferStatusEnum::WAITING;
    }
    return Status::OK();
}

void* NvshmemTransport::allocatePinnedLocalMemory(size_t length) {
    return nvshmem_engine_alloc(length);
}

void NvshmemTransport::freePinnedLocalMemory(void* addr) {
    nvshmem_engine_free(addr);
}

int NvshmemTransport::install(std::string& local_server_name,
                              std::shared_ptr<TransferMetadata> meta,
                              std::shared_ptr<Topology> topo) {
    metadata_ = meta;
    local_server_name_ = local_server_name;
    auto desc = std::make_shared<SegmentDesc>();
    if (!desc) return ERR_MEMORY;
    desc->name = local_server_name_;
    desc->protocol = "nvshmem";
    metadata_->addLocalSegment(LOCAL_SEGMENT_ID, local_server_name_,
                               std::move(desc));
    return 0;
}

int NvshmemTransport::registerLocalMemory(void* addr, size_t length,
                                          const std::string& location,
                                          bool remote_accessible,
                                          bool update_metadata) {
    (void)remote_accessible;
    BufferDesc desc;
    desc.addr = (uint64_t)addr;
    desc.length = length;
    desc.name = location;
    return metadata_->addLocalMemoryBuffer(desc, update_metadata);
}

int NvshmemTransport::unregisterLocalMemory(void* addr, bool update_metadata) {
    return metadata_->removeLocalMemoryBuffer(addr, update_metadata);
}

int NvshmemTransport::registerLocalMemoryBatch(
    const std::vector<BufferEntry>& buffer_list, const std::string& location) {
    for (auto& buffer : buffer_list) {
        registerLocalMemory(buffer.addr, buffer.length, location, true, false);
    }
    return 0;
}

int NvshmemTransport::unregisterLocalMemoryBatch(
    const std::vector<void*>& addr_list) {
    for (auto& addr : addr_list) {
        unregisterLocalMemory(addr, false);
    }
    return 0;
}

}  // namespace mooncake
