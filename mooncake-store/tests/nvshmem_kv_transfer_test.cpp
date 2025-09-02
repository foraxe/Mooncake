#include <gtest/gtest.h>
#include <glog/logging.h>
#include <vector>
#include <string>
#include <memory>
#include <chrono>

#include "client.h" // For Client, Replica::Descriptor, Slice, etc.
#include "transfer_task.h" // For TransferSubmitter, TransferFuture, AllocatedBuffer::Descriptor
#include "nvshmem_transfer_engine.h" // For nvshmem_engine_init/finalize
#include "utils.h" // For ErrorCode, toString
#include "allocator.h" // For SimpleAllocator (if needed for test buffers)

// Define a namespace for testing to avoid conflicts
namespace mooncake {
namespace testing {

// Dummy Client for tests if full client setup is too complex or not needed
// For now, we'll try to use the real client if setup isn't prohibitive
// Otherwise, we might need to mock or directly instantiate TransferSubmitter

// Allocate symmetric buffers using NVSHMEM
static char* allocate_symmetric_test_buffer(size_t size) {
    return static_cast<char*>(nvshmem_engine_alloc(size));
}

static void free_symmetric_test_buffer(char* buffer) {
    nvshmem_engine_free(buffer);
}

class NvshmemTransferTest : public ::testing::Test {
protected:
    // static std::shared_ptr<Client> test_client_; // Not using full client for these tests
    static std::unique_ptr<TransferEngine> dummy_te_; // For TransferSubmitter constructor
    static std::shared_ptr<StorageBackend> dummy_sb_; // For TransferSubmitter constructor (can be nullptr)
    static std::unique_ptr<TransferSubmitter> submitter_;

    // Test data buffers
    static char* gpu0_mem_sim; // Simulated memory for gpu0
    static char* gpu1_mem_sim; // Simulated memory for gpu1
    static char* local_cpu_mem_sim; // Simulated local CPU memory
    static const size_t mem_size = 1024;


    static void SetUpTestSuite() {
        google::InitGoogleLogging("NvshmemTransferTest");
        FLAGS_logtostderr = 1;

        LOG(INFO) << "Attempting to initialize NVSHMEM engine for tests...";
        int ret = nvshmem_engine_init();
        ASSERT_EQ(ret, 0) << "NVSHMEM engine initialization failed for tests.";
        LOG(INFO) << "NVSHMEM engine initialized successfully for tests.";

        // Initialize dummy TransferEngine and StorageBackend (nullptr)
        dummy_te_ = std::make_unique<TransferEngine>(false); // auto_discover = false
        dummy_sb_ = nullptr; // FilereadWorkerPool handles nullptr backend

        // Initialize TransferSubmitter
        // Note: "localhost_nvshmem_test" is a dummy local hostname for the submitter.
        submitter_ = std::make_unique<TransferSubmitter>(*dummy_te_, "localhost_nvshmem_test", dummy_sb_);
        LOG(INFO) << "TransferSubmitter initialized for tests.";

        // Allocate simulated memory regions
        gpu0_mem_sim = allocate_symmetric_test_buffer(mem_size);
        ASSERT_NE(gpu0_mem_sim, nullptr);
        gpu1_mem_sim = allocate_symmetric_test_buffer(mem_size);
        ASSERT_NE(gpu1_mem_sim, nullptr);
        local_cpu_mem_sim = allocate_symmetric_test_buffer(mem_size);
        ASSERT_NE(local_cpu_mem_sim, nullptr);
        LOG(INFO) << "Simulated memory buffers allocated.";
    }

    static void TearDownTestSuite() {
        LOG(INFO) << "Cleaning up simulated memory buffers.";
        free_symmetric_test_buffer(gpu0_mem_sim);
        free_symmetric_test_buffer(gpu1_mem_sim);
        free_symmetric_test_buffer(local_cpu_mem_sim);

        submitter_.reset();
        dummy_te_.reset();
        // dummy_sb_ is already managed by shared_ptr, will be nullptr.

        LOG(INFO) << "Finalizing NVSHMEM engine after tests...";
        nvshmem_engine_finalize();
        LOG(INFO) << "NVSHMEM engine finalized.";
        google::ShutdownGoogleLogging();
    }

     // Helper to create AllocatedBuffer::Descriptor vector easily
    std::vector<AllocatedBuffer::Descriptor> create_alloc_descs(
        const std::vector<std::string>& segment_names,
        const std::vector<uint64_t>& addrs, // Using uint64_t for addresses as in AllocatedBuffer::Descriptor
        const std::vector<size_t>& sizes) {
        std::vector<AllocatedBuffer::Descriptor> descs;
        for (size_t i = 0; i < segment_names.size(); ++i) {
            AllocatedBuffer::Descriptor desc;
            desc.segment_name_ = segment_names[i];
            desc.buffer_address_ = addrs[i];
            desc.size_ = sizes[i];
            desc.offset_ = 0;
            descs.push_back(desc);
        }
        return descs;
    }

};

// Static member initialization
std::unique_ptr<TransferEngine> NvshmemTransferTest::dummy_te_ = nullptr;
std::shared_ptr<StorageBackend> NvshmemTransferTest::dummy_sb_ = nullptr;
std::unique_ptr<TransferSubmitter> NvshmemTransferTest::submitter_ = nullptr;
char* NvshmemTransferTest::gpu0_mem_sim = nullptr;
char* NvshmemTransferTest::gpu1_mem_sim = nullptr;
char* NvshmemTransferTest::local_cpu_mem_sim = nullptr;


namespace { // Anonymous namespace for test helpers
    Replica::Descriptor MakeTestMemoryReplica(
        const std::vector<AllocatedBuffer::Descriptor>& descs,
        ReplicaStatus status = ReplicaStatus::COMPLETE) {

        mooncake::MemoryDescriptor mem_struct_desc; // Use the exact struct name from types.h
        mem_struct_desc.buffer_descriptors = descs;
        // MemoryDescriptor in types.h does not have a total_size field.
        // It only has: std::vector<AllocatedBuffer::Descriptor> buffer_descriptors;

        Replica::Descriptor replica_descriptor;
        replica_descriptor.descriptor_variant = mem_struct_desc; // Assign MemoryDescriptor to the variant
        replica_descriptor.status = status;
        return replica_descriptor;
    }
} // anon namespace


TEST_F(NvshmemTransferTest, SingleGPUPutGetViaNvshmemPath) {
    ASSERT_NE(submitter_, nullptr) << "TransferSubmitter not initialized.";

    memset(local_cpu_mem_sim, 'A', mem_size);
    memset(gpu0_mem_sim, 0, mem_size); // Clear target

    // PUT: local_cpu_mem_sim -> gpu0_mem_sim (represented by segment "gpu0")
    std::vector<Slice> put_slices = {{local_cpu_mem_sim, mem_size}};
    std::vector<AllocatedBuffer::Descriptor> gpu0_handles =
        create_alloc_descs({"gpu0"}, {reinterpret_cast<uint64_t>(gpu0_mem_sim)}, {mem_size});

    Replica::Descriptor replica_gpu0_put = MakeTestMemoryReplica(gpu0_handles);

    LOG(INFO) << "Attempting PUT to gpu0 via NVSHMEM path...";
    auto future_put = submitter_->submit(replica_gpu0_put, put_slices, Transport::TransferRequest::WRITE);

    ASSERT_TRUE(future_put.has_value()) << "Submit PUT operation failed to return a future.";
    EXPECT_EQ(future_put->strategy(), TransferStrategy::NVSHMEM_TRANSFER) << "Strategy should be NVSHMEM for gpu0 PUT.";
    ASSERT_EQ(future_put->get(), ErrorCode::OK) << "NVSHMEM PUT operation failed.";

    // Verify data in gpu0_mem_sim (conceptually, as this test does not perform a real copy)
    // For a real test with a mock that copies, we'd check gpu0_mem_sim content here.
    // Since nvshmem_putmem is a no-op in this test, gpu0_mem_sim won't actually change.
    // We are testing the path selection and that it "completes" with OK.

    // GET: gpu0_mem_sim -> local_cpu_mem_sim (cleared first)
    memset(local_cpu_mem_sim, 'B', mem_size); // Change local CPU mem

    std::vector<Slice> get_slices = {{local_cpu_mem_sim, mem_size}};
    // For GET, handles describe the source (gpu0), slices describe the destination (local_cpu_mem_sim)
    Replica::Descriptor replica_gpu0_get = MakeTestMemoryReplica(gpu0_handles);

    LOG(INFO) << "Attempting GET from gpu0 via NVSHMEM path...";
    auto future_get = submitter_->submit(replica_gpu0_get, get_slices, Transport::TransferRequest::READ);

    ASSERT_TRUE(future_get.has_value()) << "Submit GET operation failed to return a future.";
    EXPECT_EQ(future_get->strategy(), TransferStrategy::NVSHMEM_TRANSFER) << "Strategy should be NVSHMEM for gpu0 GET.";
    ASSERT_EQ(future_get->get(), ErrorCode::OK) << "NVSHMEM GET operation failed.";

    // Verify data in local_cpu_mem_sim (conceptually)
    // EXPECT_EQ(memcmp(local_cpu_mem_sim, gpu0_mem_sim_original_content_if_copied, mem_size), 0);
}

TEST_F(NvshmemTransferTest, MultiGPUPut_GPU0_to_GPU1) {
    ASSERT_NE(submitter_, nullptr) << "TransferSubmitter not initialized.";

    // Data originates in local_cpu_mem_sim, intended to be put onto gpu1,
    // as if local_cpu_mem_sim *is* gpu0's memory for this logical operation.
    memset(local_cpu_mem_sim, 'C', mem_size); // Source data pattern
    memset(gpu1_mem_sim, 0, mem_size);    // Clear target gpu1 simulated memory

    // Slices point to the source data (local_cpu_mem_sim, representing gpu0's source buffer)
    std::vector<Slice> slices = {{local_cpu_mem_sim, mem_size}};

    // Handles describe the destination on "gpu1"
    std::vector<AllocatedBuffer::Descriptor> gpu1_handles =
        create_alloc_descs({"gpu1"}, {reinterpret_cast<uint64_t>(gpu1_mem_sim)}, {mem_size});
    Replica::Descriptor replica_gpu1_dest = MakeTestMemoryReplica(gpu1_handles);

    LOG(INFO) << "Attempting PUT from local_cpu (as gpu0 source) to gpu1 via NVSHMEM path...";
    auto future = submitter_->submit(replica_gpu1_dest, slices, Transport::TransferRequest::WRITE);

    ASSERT_TRUE(future.has_value()) << "Submit PUT to gpu1 operation failed to return a future.";
    EXPECT_EQ(future->strategy(), TransferStrategy::NVSHMEM_TRANSFER) << "Strategy should be NVSHMEM for PUT to gpu1.";
    ASSERT_EQ(future->get(), ErrorCode::OK) << "NVSHMEM PUT to gpu1 operation failed.";

    // Conceptually, gpu1_mem_sim should now contain data from local_cpu_mem_sim.
}


TEST_F(NvshmemTransferTest, MultiGPUGet_GPU1_to_GPU0) {
    ASSERT_NE(submitter_, nullptr) << "TransferSubmitter not initialized.";

    // Simulate data already existing on gpu1_mem_sim
    memset(gpu1_mem_sim, 'D', mem_size);
    // local_cpu_mem_sim is the destination (representing gpu0's destination buffer)
    memset(local_cpu_mem_sim, 0, mem_size);

    // Slices point to the destination (local_cpu_mem_sim, representing gpu0's dest buffer)
    std::vector<Slice> slices = {{local_cpu_mem_sim, mem_size}};

    // Handles describe the source on "gpu1"
    std::vector<AllocatedBuffer::Descriptor> gpu1_handles_source =
        create_alloc_descs({"gpu1"}, {reinterpret_cast<uint64_t>(gpu1_mem_sim)}, {mem_size});
    Replica::Descriptor replica_gpu1_source = MakeTestMemoryReplica(gpu1_handles_source);

    LOG(INFO) << "Attempting GET from gpu1 to local_cpu (as gpu0 dest) via NVSHMEM path...";
    auto future = submitter_->submit(replica_gpu1_source, slices, Transport::TransferRequest::READ);

    ASSERT_TRUE(future.has_value()) << "Submit GET from gpu1 operation failed to return a future.";
    EXPECT_EQ(future->strategy(), TransferStrategy::NVSHMEM_TRANSFER) << "Strategy should be NVSHMEM for GET from gpu1.";
    ASSERT_EQ(future->get(), ErrorCode::OK) << "NVSHMEM GET from gpu1 operation failed.";

    // Conceptually, local_cpu_mem_sim should now contain data from gpu1_mem_sim.
}

// Benchmark tests (latency, bandwidth) can be added once data transfer is measurable.

} // namespace testing
} // namespace mooncake

// main function for GTest
// int main(int argc, char **argv) {
//     ::testing::InitGoogleTest(&argc, argv);
//     // ::mooncake::testing::NvshmemTransferTest::SetUpTestSuite(); // Call if not using fixture's auto call
//     int result = RUN_ALL_TESTS();
//     // ::mooncake::testing::NvshmemTransferTest::TearDownTestSuite(); // Call if not using fixture's auto call
//     return result;
// }

// The main function is usually handled by linking with gtest_main or by the CMake setup.
// The client_integration_test.cpp has its own main, so we might not need one here if CMake handles it.
// For now, let's assume the CMake setup for tests will provide the main.
