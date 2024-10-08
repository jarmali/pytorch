#pragma once

#if defined(__CUDA_ARCH__) && (__CUDA_ARCH__ >= 900) && CUDART_VERSION >= 12010
#define NVCC_SUPPORTS_MULTICAST 1
#endif

#include <ATen/ATen.h>

namespace c10d::symmetric_memory {

constexpr size_t max_num_threads_per_block = 1024;
constexpr size_t max_num_blocks = 8;

template <typename T>
__inline__ size_t get_alignment(T ptr_or_size) {
  auto val = reinterpret_cast<uintptr_t>(ptr_or_size);
  if (val % 16 == 0) {
    return 16;
  } else if (val % 8 == 0) {
    return 8;
  } else if (val % 4 == 0) {
    return 4;
  } else if (val % 2 == 0) {
    return 2;
  } else {
    return 1;
  }
}

template <>
__inline__ size_t get_alignment<size_t>(size_t size) {
  return get_alignment(reinterpret_cast<void*>(size));
}

template <bool Value, class... Args>
inline constexpr bool dependent_bool_value = Value;

template <class... Args>
inline constexpr bool dependent_false = dependent_bool_value<false, Args...>;

template <auto... Args>
inline constexpr bool dependent_false_nt =
    dependent_bool_value<false, decltype(Args)...>;

enum class MemOpSem {
  Relaxed,
  Acquire,
  Release,
  AcqRel,
};

#define CAS_ASM(addr, compare, val, old_val, sem)                 \
  asm volatile("atom.global" sem ".sys.cas.b32 %0, [%1], %2, %3;" \
               : "=r"(old_val)                                    \
               : "l"(addr), "r"(compare), "r"(val)                \
               : "memory");

template <MemOpSem Sem>
__device__ __forceinline__ uint32_t
cas(uint32_t* addr, uint32_t compare, uint32_t val) {
#if defined(USE_ROCM) || (defined(__CUDA_ARCH__) && (__CUDA_ARCH__ < 800))
  CUDA_KERNEL_ASSERT(false);
#else
  uint32_t old_val;
  if constexpr (Sem == MemOpSem::Relaxed) {
    CAS_ASM(addr, compare, val, old_val, ".relaxed");
  } else if constexpr (Sem == MemOpSem::Acquire) {
    CAS_ASM(addr, compare, val, old_val, ".acquire");
  } else if constexpr (Sem == MemOpSem::Release) {
    CAS_ASM(addr, compare, val, old_val, ".release");
  } else {
    static_assert(dependent_false_nt<Sem>);
  }
  return old_val;
#endif
}

template <MemOpSem Sem>
__device__ __forceinline__ void put_signal(uint32_t* addr) {
  while (cas<Sem>(addr, 0, 1) != 0)
    ;
}

template <MemOpSem Sem>
__device__ __forceinline__ void wait_signal(uint32_t* addr) {
  while (cas<Sem>(addr, 1, 0) != 1)
    ;
}

// Synchronizes blocks all remote blocks with the matching blockIdx.
// This itself is not a barrier across all remote threads, but it can be used
// to express different synchronization patterns.
//
// Pattern 0: Ensures that all writes to symm_mem buffers from previous
// kernels, on any device, are visible to the current kernel:
//
//   sync_remote_blocks<MemOpSem::Relaxed>(...);
//   __syncthreads();
//
// Pattern 1: Ensures that all writes to symm_mem buffers from the current
// block are visible to all remote blocks with the matching blockIdx:
//
//   __syncthreads();
//   sync_remote_blocks<MemOpSem::AcqRel>(...);
//   __syncthreads();
//
// Pattern 2: Ensures that symm_mem buffers read by the current kernel are safe
// for writing by subsequent kernels on any device.
//
//   __syncthreads();
//   sync_remote_blocks<MemOpSem::Relaxed>(...);
template <MemOpSem Sem>
__device__ __forceinline__ void sync_remote_blocks(
    uint32_t** signal_pads,
    size_t rank,
    size_t world_size);

template <>
__device__ __forceinline__ void sync_remote_blocks<MemOpSem::Relaxed>(
    uint32_t** signal_pads,
    size_t rank,
    size_t world_size) {
  if (threadIdx.x < world_size) {
    auto target_rank = threadIdx.x;
    put_signal<MemOpSem::Relaxed>(
        signal_pads[target_rank] + blockIdx.x * world_size + rank);
    wait_signal<MemOpSem::Relaxed>(
        signal_pads[rank] + blockIdx.x * world_size + target_rank);
  }
}

template <>
__device__ __forceinline__ void sync_remote_blocks<MemOpSem::AcqRel>(
    uint32_t** signal_pads,
    size_t rank,
    size_t world_size) {
  if (threadIdx.x < world_size) {
    auto target_rank = threadIdx.x;
    put_signal<MemOpSem::Release>(
        signal_pads[target_rank] + blockIdx.x * world_size + rank);
    wait_signal<MemOpSem::Acquire>(
        signal_pads[rank] + blockIdx.x * world_size + target_rank);
  }
}

template <int Size>
union Vec;

template <>
union Vec<4> {
  uint16_t u16[2];
  uint32_t u32, as_scalar;
};

template <>
union Vec<8> {
  uint16_t u16[4];
  uint32_t u32[2];
  uint64_t u64, as_scalar;
};

template <>
union alignas(16) Vec<16> {
  uint16_t u16[8];
  uint32_t u32[4];
  uint64_t u64[2];
  uint4 u128, as_scalar;
};

template <typename T>
struct MultimemLdReduce {
  template <int Alignment>
  __device__ __inline__ Vec<Alignment> operator()(T* mc_ptr) {
    static_assert(dependent_false<T>);
  }
};

template <int Alignment, typename T>
__device__ __inline__ Vec<Alignment> multimem_ld_reduce_add(T* mc_ptr) {
  MultimemLdReduce<T> functor;
  return functor.template operator()<Alignment>(mc_ptr);
}

#if defined(USE_ROCM) || !defined(NVCC_SUPPORTS_MULTICAST)
#define SPECIALIZE_MULTIMEM_LD_REDUCE_VEC_32(type, asm_type)        \
  template <>                                                       \
  struct MultimemLdReduce<type> {                                   \
    template <int Alignment>                                        \
    __device__ __inline__ Vec<Alignment> operator()(type* mc_ptr) { \
      CUDA_KERNEL_ASSERT(false);                                    \
    }                                                               \
  };
#else
#define SPECIALIZE_MULTIMEM_LD_REDUCE_VEC_32(type, asm_type)                   \
  template <>                                                                  \
  struct MultimemLdReduce<type> {                                              \
    template <int Alignment>                                                   \
    __device__ __inline__ Vec<Alignment> operator()(type* mc_ptr) {            \
      Vec<Alignment> vec;                                                      \
      if constexpr (Alignment == 16) {                                         \
        asm("multimem.ld_reduce.relaxed.sys.global.add.v4." asm_type           \
            " {%0,%1,%2,%3}, [%4];"                                            \
            : "=r"(vec.u32[0]),                                                \
              "=r"(vec.u32[1]),                                                \
              "=r"(vec.u32[2]),                                                \
              "=r"(vec.u32[3])                                                 \
            : "l"(mc_ptr)                                                      \
            : "memory");                                                       \
      } else if constexpr (Alignment == 8) {                                   \
        asm("multimem.ld_reduce.relaxed.sys.global.add.v2." asm_type           \
            " {%0,%1}, [%2];"                                                  \
            : "=r"(vec.u32[0]), "=r"(vec.u32[1])                               \
            : "l"(mc_ptr)                                                      \
            : "memory");                                                       \
      } else if constexpr (Alignment == 4) {                                   \
        asm("multimem.ld_reduce.relaxed.sys.global.add." asm_type " %0, [%1];" \
            : "=r"(vec.u32)                                                    \
            : "l"(mc_ptr)                                                      \
            : "memory");                                                       \
      }                                                                        \
      return vec;                                                              \
    }                                                                          \
  };
#endif

SPECIALIZE_MULTIMEM_LD_REDUCE_VEC_32(at::BFloat16, "bf16x2");
SPECIALIZE_MULTIMEM_LD_REDUCE_VEC_32(float, "f32");

template <int Alignment, typename T>
__device__ __inline__ void multimem_st(T* mc_ptr, Vec<Alignment>& vec) {
#if defined(USE_ROCM) || !defined(NVCC_SUPPORTS_MULTICAST)
  CUDA_KERNEL_ASSERT(false);
#else
  if constexpr (Alignment == 16) {
    asm("multimem.st.relaxed.sys.global.v4.f32 [%0], {%1,%2,%3,%4};"
        :
        : "l"(mc_ptr),
          "r"(vec.u32[0]),
          "r"(vec.u32[1]),
          "r"(vec.u32[2]),
          "r"(vec.u32[3])
        : "memory");
  } else if constexpr (Alignment == 8) {
    asm("multimem.st.relaxed.sys.global.v2.f32 [%0], {%1,%2};"
        :
        : "l"(mc_ptr), "r"(vec.u32[0]), "r"(vec.u32[1])
        : "memory");
  } else if constexpr (Alignment == 4) {
    asm("multimem.st.relaxed.sys.global.f32 [%0], %1;"
        :
        : "l"(mc_ptr), "r"(vec.u32)
        : "memory");
  } else {
    static_assert(dependent_false<T>);
  }
#endif
}

} // namespace c10d::symmetric_memory
