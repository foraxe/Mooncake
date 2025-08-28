import asyncio
from typing import Any, Dict

class MiniMooncake:
    """
    A minimal, in-memory implementation of the Mooncake KV cache store
    for single-node, disaggregated serving.
    """

    def __init__(self):
        self._store: Dict[Any, Any] = {}
        self._lock = asyncio.Lock()

    async def put(self, key: Any, value: Any):
        """
        Stores a key-value pair in the in-memory store.
        This simulates the 'send' operation.
        """
        async with self._lock:
            self._store[key] = value

    async def get(self, key: Any) -> Any:
        """
        Retrieves a value from the in-memory store by key.
        This simulates the 'receive' operation.
        """
        async with self._lock:
            return self._store.get(key)

    # These are the methods that we expect vllm to call.
    # We are mapping them to our simple put/get implementation.
    async def send_kv_caches_and_hidden_states(self, key: Any, data: Any):
        """
        Mock implementation of the send method expected by vllm's connector.
        """
        await self.put(key, data)

    async def recv_kv_caches_and_hidden_states(self, key: Any) -> Any:
        """
        Mock implementation of the receive method expected by vllm's connector.
        """
        return await self.get(key)

# Singleton instance to be used by all components in the single process.
mini_mooncake_instance = MiniMooncake()

def get_mini_mooncake_instance():
    """
    Returns the singleton instance of the MiniMooncake store.
    """
    return mini_mooncake_instance
