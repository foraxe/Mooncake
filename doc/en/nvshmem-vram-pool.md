
# NVSHMEM-based VRAM Pool

## Motivation
Large language models increasingly demand more GPU memory than what a single device can provide. Mooncake currently relies on Transfer Engine to move data across DRAM and SSD tiers, but it lacks a unified VRAM layer spanning multiple GPUs and nodes. NVSHMEM exposes a global address space across GPUs and enables low-latency GPU-initiated communication. Leveraging it allows Mooncake to build a high-performance VRAM pool that scales beyond a single device.

## Goals
- Aggregate VRAM across GPUs and nodes into a single pool.
- Support remote GPU memory reads and writes without staging through host memory.
- Provide GPU-initiated `Put`/`Get` operations with minimal CPU involvement.
        - Reuse Mooncake's object-level abstractions and replication strategies.
        - Reuse existing Transfer Engine semantics so higher layers (e.g., P2P Store) can treat NVSHMEM buffers as standard segments.
        - Support dynamic membership and fault detection similar to existing DRAM/SSD pools.
        - Maintain consistency and isolation between peers while keeping management lightweight.

## Non-goals
- Persistence or durability of data stored in the VRAM pool.
- Changing the existing object placement or eviction policies of Mooncake Store.
        
        ## Architecture Overview
        Each Mooncake client process becomes an NVSHMEM processing element (PE). A symmetric heap is created on every GPU, and parts of this heap are registered with the Transfer Engine as VRAM segments. The Master Service maintains metadata mapping objects to NVSHMEM buffers and tracks which PEs host each replica.
            
            ```
            -------------    -------------
            |   Master    |----|   Metadata  |
            -------------    -------------
                   |                 |
                   |         NVSHMEM runtime
                   v                 v
              ----------     ----------
              |  Client  |<--->|  Client  |
              | (GPU 0)  |     | (GPU 1)  |
              ----------     ----------
            ```
            
### Client Responsibilities
            1. Initialize NVSHMEM and join the global PE set.
                2. Contribute a portion of local VRAM to the symmetric heap.
                3. Register heap regions as `Segment` buffers within the Transfer Engine.
                4. Serve `Get`/`Put` requests by issuing `nvshmem_put`/`nvshmem_get` from GPU kernels.
                    
### Master Extensions
                    - Record NVSHMEM symmetric addresses for each object replica.
                    - Allocate and free VRAM slices using a distributed allocator built on NVSHMEM atomics.
                        - Handle PE failure by revoking its slices and triggering re-replication.
                        
### psudo procedure: 
1. **Initialization.** Each process becomes an NVSHMEM processing element (PE) and joins an NVSHMEM team representing all GPUs in the Mooncake cluster. On startup, Transfer Engine creates a new `NVSHMEMSegment` beside the existing RAM segment. It allocates symmetric VRAM from NVSHMEM and registers these regions as Buffers.
2. **Addressing.** NVSHMEM exposes symmetric addresses; Transfer Engine maps them to segment offsets so that existing `BatchTransfer` requests can target NVSHMEM buffers. The global pool is partitioned into fixed-size chunks tracked by the master node. Chunk ownership metadata is stored in etcd alongside current object placement.
3. **Data Path.** GPUs issue `nvshmem_put/get` or atomic operations directly against remote peers. For bulk object migration, Transfer Engine selects NVSHMEM as the transport backend; RDMA and NVMe-oF remain available for DRAM or storage targets.
4. **Synchronization.** NVSHMEM’s collective barriers ensure visibility when objects are published or evicted. Transfer Engine augments its completion callbacks to invoke `nvshmem_quiet` before marking a transfer finished.

                        ## Multi-node Considerations
                        NVSHMEM uses InfiniBand or RoCE for cross-node transport. Clients provide their network endpoints during initialization so the Master can populate topology-aware replica placements. Collective operations such as `nvshmem_barrier_all` are used during startup and reconfiguration to keep the symmetric heap consistent.
                        
                        ## Synchronization and Consistency
                        Object writes follow a two-phase protocol:
                        1. Writer allocates remote slices and transfers data with `nvshmem_put`.
                            2. A final atomic flag or fence marks the object as visible. Readers poll this flag using `nvshmem_wait_until`.
                                This preserves Mooncake's guarantee that `Get` reads a fully written object.
                                

## Data Flow
1. **Put**
   1. Client sends a `Put` request to the master.
   2. The master selects target PEs, allocates space in their symmetric heaps, and returns the offsets.
   3. The client issues NVSHMEM put operations to transfer the object directly into the remote GPU memories.
   4. Once all transfers complete, metadata is updated to mark the object ready.
2. **Get**
   1. Client queries the master for the object's location.
   2. The client performs NVSHMEM get operations to read data from the selected PEs into its local GPU memory.
Because NVSHMEM provides a partitioned global address space, the same code path works within a node (across multiple GPUs) and across nodes connected by InfiniBand or RoCE.
                                        
## Failure Handling
- **GPU failure**: detected via heartbeat timeouts. Master reclaims its symmetric heap contribution and instructs remaining PEs to shrink the heap or redistribute objects.
- **Network partition**: NVSHMEM communication failures trigger client reconnection; pending operations are retried at a higher layer.

## Implementation Plan
1. **Prototype NVSHMEM transport**: wrap `nvshmem_put/get` as a new `NvshmemTransport` in Transfer Engine.
2. **Symmetric heap allocator**: implement a lock-free bitmap allocator using NVSHMEM atomics.
3. **Master integration**: extend metadata to store NVSHMEM addresses and add recovery logic. (Extend Mooncake Store's metadata schema to store symmetric heap offsets and PE identifiers.)
4. **API exposure**: add VRAM-tier options to Mooncake Store client so upper layers can allocate objects directly into the pool.
5. **Testing**: validate on multi-GPU single node, then extend to multi-node clusters.

## Open Questions
- Optimal heap sizing and fragmentation control.
- Coordinating NVSHMEM startup with dynamic node addition/removal.
- Security isolation when sharing VRAM across tenants.

## Future works
- Dynamic resizing of the symmetric heap to accommodate changing workloads.
- Coordinating with NCCL to share GPU communication channels when both are active.
- Integrating fault-tolerant replication strategies for high availability.

## References
- [NVSHMEM Documentation](https://docs.nvidia.com/nvshmem)
