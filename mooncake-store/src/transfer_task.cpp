#include "transfer_task.h"

#include <glog/logging.h>

#include <algorithm>
#include <cstdlib>

#include "utils.h"
// Although included in transfer_task.h, explicit include for clarity if needed by .cpp specifics
// #include "nvshmem_transfer_engine.h"

namespace mooncake {

// Prototype for NVSHMEM PE mapping and eligibility checks
// These would need proper implementation based on system configuration and NVSHMEM setup.
static int get_nvshmem_pe_for_segment(const std::string& segment_name) {
    // TODO: Implement actual mapping from segment name (e.g., hostname or GPU ID) to NVSHMEM PE.
    // This mapping should be established during NVSHMEM initialization.
    // For now, returning a dummy PE or using a simple convention.
    // Example: if segment_name is "gpu0", return 0; "gpu1", return 1, etc.
    // This is highly dependent on how segments are named and PEs are identified.
    // TODO MULTI-NODE: Implement robust PE mapping.
    // 1. Each PE, on startup (after nvshmem_init), should determine its identity
    //    (e.g., hostname, GPU UUIDs it controls) and its NVSHMEM PE ID.
    // 2. This mapping (e.g., {"hostname_gpu0_uuid": pe_id, "hostname_gpu1_uuid": pe_id_other_gpu_same_node})
    //    should be stored in a distributed metadata store (e.g., etcd).
    //    The 'segment_name' provided by Mooncake would need to conform to a resolvable ID.
    //    For example, a segment_name could be "node_hostname/GPU_ID".
    // 3. This function would then query the metadata store for the PE ID associated with 'segment_name'.
    // 4. Cache mappings locally to avoid frequent metadata store lookups.

    // Simple mapping for testing:
    if (segment_name == "gpu0" || segment_name == "node0/gpu0") return 0;
    if (segment_name == "gpu1" || segment_name == "node0/gpu1") return 1;
    if (segment_name == "node1/gpu0") return 2; // Simulating another node

    // If segment_name is the local hostname (or similar local identifier),
    // this implies a local operation. NVSHMEM can be used for local GPU-GPU
    // or CPU-GPU on symmetric memory. The PE would be the current PE.
    // However, nvshmem_put/get take a *target* PE. If dest is local, it's more complex.
    // Typically, for P2P, one PE (source) Puts to another PE (dest).
    // If the 'segment_name' refers to the *local* node/GPU where data should be written *to*
    // by a *remote* initiator, then this PE mapping is for the *local* PE.
    // The current `TransferSubmitter` logic assumes `segment_name` in `handles` is the *remote* target.
    std::string local_simulated_hostname = "localhost_nvshmem_test"; // Or get actual local hostname
    if (segment_name == local_simulated_hostname) {
         // This case needs clarification: if the segment_name is local, is it a loopback?
         // Or is this function only for resolving remote PEs?
         // For now, assume it means the target is local, which might mean PE = my_pe().
         // But the put/get API needs a target_pe. If it's a put *to* local memory *from* local memory
         // via NVSHMEM, that's unusual for this API structure.
         // Let's assume for now `segment_name` in `handles` is always a remote PE.
        VLOG(1) << "get_nvshmem_pe_for_segment: segment '" << segment_name << "' appears local. This function expects remote segment names for PE mapping.";
        return -1; // Indicates not a remote PE to target.
    }

    LOG(WARNING) << "get_nvshmem_pe_for_segment: No specific PE mapping for segment '" << segment_name << "'. Fallback/Error.";
    return -1; // Return -1 to indicate PE not found or error.
}

extern bool is_nvshmem_engine_globally_initialized(); // Declaration for the function in nvshmem_transfer_engine.cpp

static bool is_nvshmem_eligible(const std::vector<AllocatedBuffer::Descriptor>& handles, Transport::TransferRequest::OpCode op_code) {
    // TODO MULTI-NODE: Implement robust eligibility checks.
    // 1. NVSHMEM engine must be initialized.
    // 2. The memory described by `handles` must be symmetric memory registered with NVSHMEM.
    //    This might involve checking metadata associated with the buffer_address or segment_name.
    //    For example, a global registry of NVSHMEM-allocated regions, or specific flags on memory.
    // 3. The target segment (derived from handle.segment_name_) must map to a valid, reachable PE.
    //    (i.e., get_nvshmem_pe_for_segment(handle.segment_name_) should return a valid PE).
    // 4. For inter-node, appropriate network (InfiniBand, RoCE) must be configured and up.

    if (handles.empty()) return false;

    if (!is_nvshmem_engine_globally_initialized()) {
        VLOG(2) << "NVSHMEM not eligible: engine not initialized.";
        return false;
    }

    // For current testing and multi-node conceptualization:
    // - Assume any segment starting with "gpu" or "nodeX/gpuY" is potentially NVSHMEM eligible.
    // - And that it maps to a valid PE.
    for (const auto& handle : handles) {
        if (handle.segment_name_.rfind("gpu", 0) == 0 || handle.segment_name_.rfind("node", 0) == 0) {
            if (get_nvshmem_pe_for_segment(handle.segment_name_) != -1) {
                 VLOG(2) << "NVSHMEM eligible: found handle for potential NVSHMEM segment: " << handle.segment_name_;
                return true;
            }
        }
    }
    VLOG(2) << "NVSHMEM not eligible: no handle segment_name matches 'gpu*' or 'node*' pattern, or PE mapping failed.";
    return false;
}

// ============================================================================
// FilereadWorkerPool Implementation
// ============================================================================
//to fully utilize the available ssd bandwidth, we use a default of 8 worker threads.
constexpr int kDefaultFilereadWorkers = 8;

FilereadWorkerPool::FilereadWorkerPool(std::shared_ptr<StorageBackend>& backend) : shutdown_(false) {
    VLOG(1) << "Creating FilereadWorkerPool with " << kDefaultFilereadWorkers
            << " workers";

    // Start worker threads
    workers_.reserve(kDefaultFilereadWorkers);
    for (int i = 0; i < kDefaultFilereadWorkers; ++i) {
        workers_.emplace_back(&FilereadWorkerPool::workerThread, this);
    }
    backend_ = backend;
}

FilereadWorkerPool::~FilereadWorkerPool() {
    // Signal shutdown
    {
        std::lock_guard<std::mutex> lock(queue_mutex_);
        shutdown_.store(true);
    }
    queue_cv_.notify_all();

    // Wait for all workers to finish
    for (auto& worker : workers_) {
        if (worker.joinable()) {
            worker.join();
        }
    }

    VLOG(1) << "FilereadWorkerPool destroyed";
}

void FilereadWorkerPool::submitTask(FilereadTask task) {
    {
        std::lock_guard<std::mutex> lock(queue_mutex_);
        if (shutdown_.load()) {
            LOG(WARNING)
                << "Attempting to submit task to shutdown FilereadWorkerPool";
            task.state->set_completed(ErrorCode::TRANSFER_FAIL);
            return;
        }
        task_queue_.push(std::move(task));
    }
    queue_cv_.notify_one();
}

void FilereadWorkerPool::workerThread() {
    VLOG(2) << "FilereadWorkerPool worker thread started";

    while (true) {
        FilereadTask task("", 0, {}, nullptr);

        // Wait for task or shutdown signal
        {
            std::unique_lock<std::mutex> lock(queue_mutex_);
            queue_cv_.wait(lock, [this] {
                return shutdown_.load() || !task_queue_.empty();
            });

            if (shutdown_.load() && task_queue_.empty()) {
                break;
            }

            if (!task_queue_.empty()) {
                task = std::move(task_queue_.front());
                task_queue_.pop();
            }
        }

        // Execute the task if we have one
        if (task.state) {
            try {
                if (!backend_) {
                    LOG(ERROR) << "Backend is not initialized, cannot load object";
                    task.state->set_completed(ErrorCode::TRANSFER_FAIL);
                    continue; 
                }

                auto error_code = backend_->LoadObject("", task.slices, task.file_path);
                if(error_code == ErrorCode::OK){
                    VLOG(2) << "Fileread task completed successfully with "
                            << task.file_path;
                    task.state->set_completed(ErrorCode::OK);
                }else{
                    LOG(ERROR) << "Fileread task failed for file: "
                               << task.file_path
                               << " with error code: " << toString(error_code);
                    task.state->set_completed(ErrorCode::TRANSFER_FAIL);
                }
            } catch (const std::exception& e) {
                LOG(ERROR) << "Exception during async fileread: " << e.what();
                task.state->set_completed(ErrorCode::TRANSFER_FAIL);
            }
        }
    }

    VLOG(2) << "FilereadWorkerPool worker thread exiting";
}

// ============================================================================
// MemcpyWorkerPool Implementation
// ============================================================================
// Since memcpy is bound by memory bandwidth, we only need one worker thread.
constexpr int kDefaultMemcpyWorkers = 1;

MemcpyWorkerPool::MemcpyWorkerPool() : shutdown_(false) {
    VLOG(1) << "Creating MemcpyWorkerPool with " << kDefaultMemcpyWorkers
            << " workers";

    // Start worker threads
    workers_.reserve(kDefaultMemcpyWorkers);
    for (int i = 0; i < kDefaultMemcpyWorkers; ++i) {
        workers_.emplace_back(&MemcpyWorkerPool::workerThread, this);
    }
}

MemcpyWorkerPool::~MemcpyWorkerPool() {
    // Signal shutdown
    {
        std::lock_guard<std::mutex> lock(queue_mutex_);
        shutdown_.store(true);
    }
    queue_cv_.notify_all();

    // Wait for all workers to finish
    for (auto& worker : workers_) {
        if (worker.joinable()) {
            worker.join();
        }
    }

    VLOG(1) << "MemcpyWorkerPool destroyed";
}

void MemcpyWorkerPool::submitTask(MemcpyTask task) {
    {
        std::lock_guard<std::mutex> lock(queue_mutex_);
        if (shutdown_.load()) {
            LOG(WARNING)
                << "Attempting to submit task to shutdown MemcpyWorkerPool";
            task.state->set_completed(ErrorCode::TRANSFER_FAIL);
            return;
        }
        task_queue_.push(std::move(task));
    }
    queue_cv_.notify_one();
}

void MemcpyWorkerPool::workerThread() {
    VLOG(2) << "MemcpyWorkerPool worker thread started";

    while (true) {
        MemcpyTask task({}, nullptr);

        // Wait for task or shutdown signal
        {
            std::unique_lock<std::mutex> lock(queue_mutex_);
            queue_cv_.wait(lock, [this] {
                return shutdown_.load() || !task_queue_.empty();
            });

            if (shutdown_.load() && task_queue_.empty()) {
                break;
            }

            if (!task_queue_.empty()) {
                task = std::move(task_queue_.front());
                task_queue_.pop();
            }
        }

        // Execute the task if we have one
        if (task.state) {
            try {
                for (const auto& op : task.operations) {
                    std::memcpy(op.dest, op.src, op.size);
                }

                VLOG(2) << "Memcpy task completed successfully with "
                        << task.operations.size() << " operations";
                task.state->set_completed(ErrorCode::OK);
            } catch (const std::exception& e) {
                LOG(ERROR) << "Exception during async memcpy: " << e.what();
                task.state->set_completed(ErrorCode::TRANSFER_FAIL);
            }
        }
    }

    VLOG(2) << "MemcpyWorkerPool worker thread exiting";
}

// ============================================================================
// TransferEngineOperationState Implementation
// ============================================================================

bool TransferEngineOperationState::is_completed() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (result_.has_value()) {
        return true;
    }

    check_task_status();
    return result_.has_value();
}

void TransferEngineOperationState::check_task_status() {
    // Check all transfers in the batch
    bool all_completed = true;
    bool has_failure = false;

    for (size_t i = 0; i < batch_size_; ++i) {
        TransferStatus status;
        Status s = engine_.getTransferStatus(batch_id_, i, status);
        if (!s.ok()) {
            LOG(ERROR) << "Failed to get transfer status for batch "
                       << batch_id_ << " task " << i << " with error "
                       << s.message();
            set_result_internal(ErrorCode::TRANSFER_FAIL);
            return;
        }

        switch (status.s) {
            case TransferStatusEnum::COMPLETED:
                // This transfer is done, continue checking others
                break;
            case TransferStatusEnum::FAILED:
            case TransferStatusEnum::CANCELED:
            case TransferStatusEnum::INVALID:
                LOG(ERROR) << "Transfer failed for batch " << batch_id_
                           << " task " << i << " with status "
                           << static_cast<int>(status.s);
                has_failure = true;
                break;
            default:
                // Transfer is still pending (PENDING, RUNNING, etc.)
                all_completed = false;
                break;
        }
    }

    if (has_failure) {
        VLOG(1) << "Setting batch " << batch_id_
                << " result to TRANSFER_FAIL due to task failures";
        set_result_internal(ErrorCode::TRANSFER_FAIL);
        return;
    }

    if (all_completed) {
        set_result_internal(ErrorCode::OK);
        return;
    }

    return;
}

void TransferEngineOperationState::set_result_internal(ErrorCode error_code) {
    if (result_.has_value()) {
        LOG(ERROR) << "Attempting to set result multiple times for batch "
                   << batch_id_
                   << ". Previous result: " << static_cast<int>(result_.value())
                   << ", attempted new result: " << static_cast<int>(error_code)
                   << ". This indicates a race condition or logic error.";
        return;  // Don't crash, just return early
    }

    VLOG(1) << "Setting transfer result for batch " << batch_id_ << " to "
            << static_cast<int>(error_code);
    result_.emplace(error_code);

    cv_.notify_all();
}

void TransferEngineOperationState::wait_for_completion() {
    if (is_completed()) {
        return;
    }

    VLOG(1) << "Starting transfer engine polling for batch " << batch_id_;
    constexpr int64_t timeout_seconds = 60;
    constexpr int64_t kOneSecondInNano = 1000 * 1000 * 1000;

    const int64_t start_ts = getCurrentTimeInNano();

    while (true) {
        if (getCurrentTimeInNano() - start_ts >
            timeout_seconds * kOneSecondInNano) {
            LOG(ERROR) << "Failed to complete transfers after "
                       << timeout_seconds << " seconds for batch " << batch_id_;
            set_result_internal(ErrorCode::TRANSFER_FAIL);
            return;
        }

        std::unique_lock<std::mutex> lock(mutex_);
        check_task_status();
        if (result_.has_value()) {
            VLOG(1) << "Transfer engine operation completed for batch "
                    << batch_id_
                    << " with result: " << static_cast<int>(result_.value());
            break;
        }
        // Continue polling
        VLOG(1) << "Transfer engine operation still pending for batch "
                << batch_id_;
    }
}

// ============================================================================
// TransferFuture Implementation
// ============================================================================

TransferFuture::TransferFuture(std::shared_ptr<OperationState> state)
    : state_(std::move(state)) {
    CHECK(state_) << "TransferFuture requires valid state";
}

bool TransferFuture::isReady() const { return state_->is_completed(); }

ErrorCode TransferFuture::wait() {
    if (!isReady()) {
        state_->wait_for_completion();
    }
    return state_->get_result();
}

ErrorCode TransferFuture::get() { return wait(); }

TransferStrategy TransferFuture::strategy() const {
    return state_->get_strategy();
}

// ============================================================================
// TransferSubmitter Implementation
// ============================================================================

TransferSubmitter::TransferSubmitter(TransferEngine& engine,
                                     const std::string& local_hostname,
                                     std::shared_ptr<StorageBackend>& backend)
    : engine_(engine),
      local_hostname_(local_hostname),
      memcpy_pool_(std::make_unique<MemcpyWorkerPool>()),
      fileread_pool_(std::make_unique<FilereadWorkerPool>(backend)) {
    CHECK(!local_hostname_.empty()) << "Local hostname cannot be empty";

    // Read MC_STORE_MEMCPY environment variable, default to false (disabled)
    const char* env_value = std::getenv("MC_STORE_MEMCPY");
    if (env_value == nullptr) {
        memcpy_enabled_ = false;  // Default: disabled
    } else {
        std::string env_str(env_value);
        // Convert to lowercase for case-insensitive comparison
        std::transform(env_str.begin(), env_str.end(), env_str.begin(),
                       ::tolower);
        if (env_str == "false" || env_str == "0" || env_str == "no" ||
            env_str == "off") {
            memcpy_enabled_ = false;
        } else if (env_str == "true" || env_str == "1" || env_str == "yes" ||
                   env_str == "on") {
            memcpy_enabled_ = true;
        } else {
            LOG(WARNING) << "Invalid value for MC_STORE_MEMCPY: " << env_str
                         << ", defaulting to enabled";
            memcpy_enabled_ = true;
        }
    }

    VLOG(1) << "TransferSubmitter initialized with memcpy_enabled="
            << memcpy_enabled_;

    // TODO: NVSHMEM Initialization:
    // nvshmem_engine_init() should be called once globally before any NVSHMEM operations.
    // Consider calling it in a higher-level initialization routine (e.g., main application setup,
    // or when the first client/service requiring NVSHMEM is created).
    // For now, we assume it's initialized. If it's not, NVSHMEM operations will fail.
    // int init_ret = nvshmem_engine_init();
    // if (init_ret != 0) {
    //     LOG(ERROR) << "NVSHMEM Engine initialization failed with code: " << init_ret
    //                << ". NVSHMEM transfers will not be available.";
    //     // Set a flag to disable NVSHMEM strategy if init fails
    // } else {
    //     VLOG(0) << "NVSHMEM Engine initialized successfully by TransferSubmitter (or assumed to be).";
    //     // Consider calling nvshmem_engine_finalize() in ~TransferSubmitter() or a global shutdown hook.
    // }
    // nvshmem_engine_init() is currently simple, but a real implementation might take time or fail.
    // We also need a way to get local_rank or PE ID for current process to avoid self-transfer issues if PE mapping is naive.
    VLOG(0) << "NVSHMEM Engine initialization should be handled here or globally.";
}

std::optional<TransferFuture> TransferSubmitter::submit(
    const Replica::Descriptor& replica,
    std::vector<Slice>& slices,  Transport::TransferRequest::OpCode op_code) {

    if(replica.is_memory_replica()) {
        std::vector<AllocatedBuffer::Descriptor> handles;
        auto& mem_desc = replica.get_memory_descriptor();
        handles = mem_desc.buffer_descriptors;

        if (!validateTransferParams(handles, slices)) {
            return std::nullopt;
        }

        TransferStrategy strategy = selectStrategy(handles, slices);

        switch (strategy) {
            case TransferStrategy::LOCAL_MEMCPY:
                return submitMemcpyOperation(handles, slices, op_code);
            case TransferStrategy::TRANSFER_ENGINE:
                return submitTransferEngineOperation(handles, slices, op_code);
            case TransferStrategy::NVSHMEM_TRANSFER:
                return submitNvshmemTransferOperation(handles, slices, op_code);
            default:
                LOG(ERROR) << "Unknown transfer strategy: " << strategy;
                return std::nullopt;
        }
    }else{
        return submitFileReadOperation(replica, slices, op_code);
    }
}

std::optional<TransferFuture> TransferSubmitter::submitMemcpyOperation(
    const std::vector<AllocatedBuffer::Descriptor>& handles,
    std::vector<Slice>& slices, Transport::TransferRequest::OpCode op_code) {
    auto state = std::make_shared<MemcpyOperationState>();

    // Create memcpy operations
    std::vector<MemcpyOperation> operations;
    operations.reserve(handles.size());

    for (size_t i = 0; i < handles.size(); ++i) {
        const auto& handle = handles[i];
        const auto& slice = slices[i];

        void* dest;
        const void* src;

        if (op_code == Transport::TransferRequest::READ) {
            // READ: from handle (remote buffer) to slice (local
            // buffer)
            dest = slice.ptr;
            src = reinterpret_cast<const void*>(handle.buffer_address_);
        } else {
            // WRITE: from slice (local buffer) to handle (remote
            // buffer)
            dest = reinterpret_cast<void*>(handle.buffer_address_);
            src = slice.ptr;
        }

        operations.emplace_back(dest, src, handle.size_);
    }

    // Submit memcpy operations to worker pool for async execution
    MemcpyTask task(std::move(operations), state);
    memcpy_pool_->submitTask(std::move(task));

    VLOG(1) << "Memcpy transfer submitted to worker pool with "
            << handles.size() << " operations";

    return TransferFuture(state);
}

std::optional<TransferFuture> TransferSubmitter::submitTransferEngineOperation(
    const std::vector<AllocatedBuffer::Descriptor>& handles,
    std::vector<Slice>& slices, Transport::TransferRequest::OpCode op_code) {
    // Create transfer requests
    std::vector<Transport::TransferRequest> requests;
    requests.reserve(handles.size());

    for (size_t i = 0; i < handles.size(); ++i) {
        const auto& handle = handles[i];
        const auto& slice = slices[i];

        Transport::SegmentHandle seg =
            engine_.openSegment(handle.segment_name_);
        if (seg == static_cast<uint64_t>(ERR_INVALID_ARGUMENT)) {
            LOG(ERROR) << "Failed to open segment " << handle.segment_name_;
            return std::nullopt;
        }

        Transport::TransferRequest request;
        request.opcode = op_code;
        request.source = static_cast<char*>(slice.ptr);
        request.target_id = seg;
        request.target_offset = handle.buffer_address_;
        request.length = handle.size_;

        requests.emplace_back(request);
    }

    // Allocate batch ID
    const size_t batch_size = requests.size();
    BatchID batch_id = engine_.allocateBatchID(batch_size);
    if (batch_id == Transport::INVALID_BATCH_ID) {
        LOG(ERROR) << "Failed to allocate batch ID";
        return std::nullopt;
    }

    // Submit transfer
    Status s = engine_.submitTransfer(batch_id, requests);
    if (!s.ok()) {
        LOG(ERROR) << "Failed to submit all transfers, error code is "
                   << s.code();
        // Note: batch_id will be freed by TransferEngineOperationState
        // destructor if we create the state object, otherwise we need to free
        // it here
        engine_.freeBatchID(batch_id);
        return std::nullopt;
    }

    // Create state with transfer engine context - no polling thread
    // needed
    auto state = std::make_shared<TransferEngineOperationState>(
        engine_, batch_id, batch_size);

    return TransferFuture(state);
}

std::optional<TransferFuture> TransferSubmitter::submitFileReadOperation(
    const Replica::Descriptor& replica, std::vector<Slice>& slices, 
    Transport::TransferRequest::OpCode op_code) {
    auto state = std::make_shared<FilereadOperationState>();
    auto disk_replica = replica.get_disk_descriptor();
    std::string file_path = disk_replica.file_path;
    size_t file_length = disk_replica.file_size;

    // Submit memcpy operations to worker pool for async execution
    FilereadTask task(file_path, file_length, slices, state);
    fileread_pool_->submitTask(std::move(task));

    VLOG(1) << "Fileread transfer submitted to worker pool with "
            << file_path ;

    return TransferFuture(state);
}

std::optional<TransferFuture> TransferSubmitter::submitNvshmemTransferOperation(
    const std::vector<AllocatedBuffer::Descriptor>& handles,
    std::vector<Slice>& slices, Transport::TransferRequest::OpCode op_code) {

    VLOG(1) << "Attempting to submit NVSHMEM transfer operation.";
    auto state = std::make_shared<NvshmemOperationState>();

    // TODO: Ensure nvshmem_engine_init() has been called successfully before this point.
    // This might be done in TransferSubmitter's constructor or globally.

    for (size_t i = 0; i < handles.size(); ++i) {
        const auto& handle = handles[i]; // Remote memory descriptor
        const auto& slice = slices[i];   // Local memory descriptor

        // Determine the target PE. The segment_name in handle should map to a PE.
        // The remote buffer address is handle.buffer_address_
        // The local buffer is slice.ptr
        // Size is handle.size_ (should be same as slice.size)
        int target_pe = get_nvshmem_pe_for_segment(handle.segment_name_);

        VLOG(2) << "NVSHMEM Op: " << ((op_code == Transport::TransferRequest::READ) ? "GET" : "PUT")
                << "Slice Local Ptr: " << slice.ptr
                << ", Handle Remote Addr: " << reinterpret_cast<void*>(handle.buffer_address_)
                << ", Size: " << handle.size_
                << ", Target PE: " << target_pe
                << ", Segment Name: " << handle.segment_name_;

        int result = -1;
        if (op_code == Transport::TransferRequest::READ) {
            // READ from remote GPU (handle) to local memory (slice)
            // nvshmem_engine_get(local_dest, remote_source, size, remote_pe)
            result = nvshmem_engine_get(slice.ptr, reinterpret_cast<const void*>(handle.buffer_address_), handle.size_, target_pe);
        } else { // WRITE
            // WRITE from local memory (slice) to remote GPU (handle)
            // nvshmem_engine_put(remote_dest, local_source, size, remote_pe)
            result = nvshmem_engine_put(reinterpret_cast<void*>(handle.buffer_address_), slice.ptr, handle.size_, target_pe);
        }

        if (result != 0) {
            LOG(ERROR) << "NVSHMEM engine operation failed for handle " << i
                       << " (segment: " << handle.segment_name_ << ", address: " << reinterpret_cast<void*>(handle.buffer_address_)
                       << ") with op_code " << op_code << ". Error code: " << result;
            state->set_completed(ErrorCode::TRANSFER_FAIL);
            return TransferFuture(state); // Return on first failure
        }
    }

    // If all operations are successful (in this synchronous model)
    // A barrier might be needed here if operations are truly async and need synchronization point
    // For now, nvshmem_engine_put/get are blocking calls.
    // If they were non-blocking, a nvshmem_engine_barrier() or similar sync would be needed.
    // nvshmem_engine_barrier(); // Optional: consider if needed for true async or batching logic

    state->set_completed(ErrorCode::OK);
    VLOG(1) << "NVSHMEM transfer operation submitted successfully with " << handles.size() << " operations.";
    return TransferFuture(state);
}

TransferStrategy TransferSubmitter::selectStrategy(
    const std::vector<AllocatedBuffer::Descriptor>& handles,
    const std::vector<Slice>& slices) const {
    // TODO: Add a check here to see if nvshmem_engine is initialized and available.
    // For now, assume nvshmem_active is true if we want to test this path.
    // bool nvshmem_initialized = nvshmem_engine_is_initialized(); // Needs this function in nvshmem_transfer_engine
    bool nvshmem_initialized = true; // TODO: check real NVSHMEM initialization

    if (nvshmem_initialized && is_nvshmem_eligible(handles, Transport::TransferRequest::READ)) { // OpCode doesn't strictly matter for eligibility check here
        VLOG(1) << "Selected NVSHMEM_TRANSFER strategy.";
        return TransferStrategy::NVSHMEM_TRANSFER;
    }

    // Check if memcpy operations are enabled via environment variable
    if (!memcpy_enabled_) {
        VLOG(2) << "Memcpy operations disabled via MC_STORE_MEMCPY environment "
                   "variable, and NVSHMEM not eligible/active. Falling back to TRANSFER_ENGINE.";
        return TransferStrategy::TRANSFER_ENGINE;
    }

    // Check conditions for local memcpy optimization
    if (isLocalTransfer(handles)) {
        VLOG(1) << "Selected LOCAL_MEMCPY strategy.";
        return TransferStrategy::LOCAL_MEMCPY;
    }

    VLOG(1) << "No special strategy (NVSHMEM/MEMCPY) applicable, falling back to TRANSFER_ENGINE.";
    return TransferStrategy::TRANSFER_ENGINE;
}

bool TransferSubmitter::isLocalTransfer(
    const std::vector<AllocatedBuffer::Descriptor>& handles) const {
    return std::all_of(handles.begin(), handles.end(),
                       [this](const auto& handle) {
                           return handle.segment_name_ == local_hostname_;
                       });
}

bool TransferSubmitter::validateTransferParams(
    const std::vector<AllocatedBuffer::Descriptor>& handles,
    const std::vector<Slice>& slices) const {
    if (handles.empty()) {
        LOG(ERROR) << "handles is empty";
        return false;
    }

    if (handles.size() > slices.size()) {
        LOG(ERROR) << "invalid_partition_count handles_size=" << handles.size()
                   << " slices_size=" << slices.size();
        return false;
    }

    for (size_t i = 0; i < handles.size(); ++i) {
        if (handles[i].size_ != slices[i].size) {
            LOG(ERROR) << "Size of replica partition " << i << " ("
                       << handles[i].size_
                       << ") does not match provided buffer (" << slices[i].size
                       << ")";
            return false;
        }
    }

    return true;
}

}  // namespace mooncake
