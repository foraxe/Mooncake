# Mini-Mooncake: An In-Memory KV Cache Store for Disaggregated LLM Serving

This project provides a minimal, in-memory implementation of the Mooncake KV cache store. It is designed to be used as a communication backend for disaggregated LLM serving systems, such as `vllm`, on a single machine.

## Introduction

In disaggregated LLM serving, the prefill and decode stages of inference are handled by separate processes or servers. This requires a mechanism for transferring the KV cache (the state of the transformer model) from the prefill server to the decode server. Mooncake is a system that provides a high-performance solution for this problem.

This project, "mini-mooncake", is a simplified version of the Mooncake store that works entirely in memory. It is intended for single-node deployments and as a tool for understanding the communication patterns in disaggregated serving.

## Components

### `mini_mooncake.py`

This file contains the core of the mini-mooncake implementation. It provides a simple, asynchronous key-value store using a Python dictionary. The main components are:

- **`MiniMooncake` class:** A class that implements a thread-safe, in-memory key-value store.
- **`mini_mooncake_instance`:** A singleton instance of the `MiniMooncake` class that is shared across the application.

The store provides `put` and `get` methods for storing and retrieving data. These are wrapped by `send_kv_caches_and_hidden_states` and `recv_kv_caches_and_hidden_states` to mimic the API expected by `vllm`.

### `vllm_with_mini_mooncake.py`

This file provides an example of how to use the `mini-mooncake` backend with `vllm`. It demonstrates how to:

1.  **Create a custom `vllm` KV cache connector:** The `MiniMooncakeVllmConnector` class is a custom connector that uses the `mini_mooncake` store for communication.
2.  **Patch the `vllm` connector factory:** The `patch_vllm_connector_factory` function shows how to patch the `vllm` source code at runtime to use the custom connector.

## How to Run the Example

To run the example, you would need to have `vllm` installed. Then, you could run the `vllm_with_mini_mooncake.py` script. The script would start a `vllm` server that uses the `mini-mooncake` backend for communication.

**Note:** The provided `vllm_with_mini_mooncake.py` is a template and is not runnable as-is. It requires a full `vllm` installation and a model. It is intended to demonstrate the approach of using a custom, in-memory backend for disaggregated serving.
