import asyncio
import torch
from typing import Any

from mini_mooncake import get_mini_mooncake_instance

# A dummy base class since we couldn't read the real one.
# We will refine this as we learn more.
class KVConnectorBase:
    def __init__(self, *args, **kwargs):
        pass

    async def send_kv_caches_and_hidden_states(self, key: Any, data: Any):
        raise NotImplementedError

    async def recv_kv_caches_and_hidden_states(self, key: Any) -> Any:
        raise NotImplementedError

class MiniMooncakeVllmConnector(KVConnectorBase):
    """
    A vllm KV cache connector that uses the mini-mooncake in-memory store.
    """

    def __init__(self, *args, **kwargs):
        super().__init__(*args, **kwargs)
        self.mooncake = get_mini_mooncake_instance()
        print("Using MiniMooncakeVllmConnector")

    async def send_kv_caches_and_hidden_states(self, key: Any,
                                               data: Any) -> None:
        """Sends KV caches and hidden states to the mini-mooncake store."""
        # In a real implementation, the data would be tensors.
        # For this example, we'll just store the python objects.
        print(f"MiniMooncake: Storing data for key {key}")
        await self.mooncake.put(key, data)

    async def recv_kv_caches_and_hidden_states(self, key: Any) -> Any:
        """Receives KV caches and hidden states from the mini-mooncake store."""
        print(f"MiniMooncake: Retrieving data for key {key}")
        return await self.mooncake.get(key)


def patch_vllm_connector_factory():
    """
    Patches the vllm KV connector factory to use our custom connector.
    """
    # We need to find where the factory is and patch it.
    # Based on the file list, we'll assume it's in
    # `vllm.distributed.kv_transfer.kv_connector.factory`.
    # We will patch the function that creates the connector.
    # Let's assume the function is called `create_kv_connector`.

    try:
        from vllm.distributed.kv_transfer.kv_connector import factory

        original_create_kv_connector = factory.create_kv_connector

        def create_mini_mooncake_connector(*args, **kwargs):
            print("Patch: Intercepted create_kv_connector call.")
            # We ignore the original args and return our custom connector.
            return MiniMooncakeVllmConnector()

        factory.create_kv_connector = create_mini_mooncake_connector
        print("Successfully patched vllm KV connector factory.")

    except ImportError:
        print("Could not import vllm.distributed.kv_transfer.kv_connector.factory")
        print("Make sure vllm is installed.")
        raise

    except Exception as e:
        print(f"An error occurred while patching vllm: {e}")
        raise


async def main():
    """
    Main function to run the vllm server with the mini-mooncake backend.
    """
    # First, patch the vllm connector factory.
    patch_vllm_connector_factory()

    # Now, we can run the vllm server.
    # We will need to construct the arguments for the server.
    # We will use the arguments from the vllm integration guide.
    # Note: This part of the code is not runnable as-is, because it
    # requires a vllm installation and a model. This is a template
    # for how it would be used.

    # This is a placeholder for where the vllm server would be started.
    # from vllm.entrypoints.openai.api_server import main as vllm_main
    # args = [
    #     "--model", "gpt2",
    #     "--kv-transfer-config", '{"kv_connector":"MiniMooncakeVllmConnector","kv_role":"kv_producer"}'
    # ]
    # await vllm_main(args)
    print("VLLM server would be started here.")


if __name__ == "__main__":
    # To run this, you would need to have vllm installed.
    # Then you could run this script.
    # For now, we will just run the main function to demonstrate the patching.
    asyncio.run(main())
