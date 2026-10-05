# Implementation Description: Software vGPU Slicing

## 1. Title & High-Level Scope
- **Title**: Software vGPU Slicing for Non-MDEV Hosts
- **Scope**: Provide GPU paravirtualization (GPU-PV/vGPU slicing) for hosts that lack native mediated device (mdev) support (such as consumer NVIDIA or AMD GPUs without SR-IOV or vGPU licenses). This involves developing a host-side renderer/proxy and a guest-side virtual display/compute driver that translates API calls (DirectX/Vulkan) across the hypervisor boundary via a high-speed shared memory transport, effectively multiplexing a single host GPU among multiple guests and the host itself without hardware partitioning.
- **Out of Scope**: Full hardware pass-through (PCIe VFIO), natively supported mdev implementations, CPU-only software rendering fallback (e.g. LLVMpipe).

## 2. Architecture & Inter-Component Interactions
- **Host-Side Proxy Daemon**: Intercepts guest GPU API calls, schedules and executes them on the host GPU using native Linux drivers (Vulkan/OpenGL).
- **Guest-Side Virtual GPU Driver**: A Windows kernel-mode display driver (WDDM/KMDOD) and user-mode driver (UMD) that exposes DirectX 11/12 and Vulkan capabilities to Windows applications. It intercepts graphics commands and serializes them.
- **Transport Layer**: A dedicated IVSHMEM shared memory region for command buffers and DMA-BUF exchange, coupled with VirtIO-Serial/VSOCK for synchronization.
- **Interaction Flow**: Guest application issues DirectX command -> Guest UMD serializes command into IVSHMEM ring buffer -> Host Proxy polls/waits -> Host Proxy deserializes and submits to host GPU -> Host GPU renders to shared buffer -> Host Compositor consumes DMA-BUF.

## 3. Data Structures, Protocols & Memory Layouts
- **Command Ring Buffer**:
  ```c
  struct alignas(64) vgpu_cmd_ring_header_t {
      std::atomic<uint32_t> head;
      std::atomic<uint32_t> tail;
      uint32_t capacity;
      uint32_t flags;
      uint8_t reserved[48];
  };
  ```
- **Serialization Format**: High-performance binary format, Little-Endian. Commands are categorized into State Commands, Draw Commands, Compute Commands, and Presentation Commands.
- **Memory Alignment**: 64-byte cache line alignment strictly enforced for all shared synchronization primitives.

## 4. Step-by-Step Execution Sequence
1. **Initialization**: Host proxy daemon allocates IVSHMEM segment and initializes command ring buffers. Guest driver starts and maps the IVSHMEM segment.
2. **Command Serialization**: Windows application calls DX12 `DrawInstanced`. Guest UMD writes the encoded command to the ring buffer and advances the atomic tail pointer.
3. **Execution**: Host proxy detects tail pointer advancement, reads the command, translates it to Vulkan/OpenGL, and submits it to the host GPU.
4. **Presentation**: Guest calls `Present`. Host proxy receives the present command, completes rendering to the shared surface, and notifies the Wayland client to update the corresponding `wl_surface`.

## 5. Concurrency, Threading & Synchronization
- **Lock-Free Queue**: The command ring buffer uses a lock-free Single-Producer Single-Consumer (SPSC) queue model.
- **Atomic Operations**: `std::atomic_thread_fence` with `std::memory_order_release` and `std::memory_order_acquire` are used to ensure memory visibility across the host/guest boundary.
- **Thread Ownership**: The host proxy maintains dedicated threads per guest context to prevent blocking and ensure low-latency submission.

## 6. Error Handling & Failure Modes
- **Buffer Overflow**: If the ring buffer is full, the guest UMD must block and wait (or yield) until the host consumes commands.
- **GPU Hangs/TDR**: If the host GPU hangs, the proxy must attempt recovery and signal a device reset (TDR) to the guest driver.
- **Disconnects**: If the guest VM terminates unexpectedly, the host proxy must cleanly free all associated Vulkan/OpenGL resources and unmap shared memory.

## 7. Verification & Testing Criteria
- **Unit Tests**: Serialization and deserialization of all command types must be verified.
- **Integration Tests**: A mock guest application drawing basic primitives, verified on the host proxy.
- **Performance Benchmarks**: Command submission latency must be < 2ms. Throughput must support at least 60 FPS for typical desktop rendering workloads.
- **Sanitizers**: All host proxy code must pass AddressSanitizer and LeakSanitizer with zero memory leaks.
