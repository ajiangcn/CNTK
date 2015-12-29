//
// <copyright file="GPUMatrix.cu" company="Microsoft">
//     Copyright (c) Microsoft Corporation.  All rights reserved.
// </copyright>
//

#include "stdafx.h"
#include "Basics.h"
#include "BestGpu.h"
//#include "DebugUtil.h"

#ifndef CPUONLY

#include "GPUTensor.h"
#include "GPUMatrix.h"
#include "GPUMatrixCUDAKernels.cuh"
#include "CommonMatrix.h"
#define TENSOR_OPS_DECL __device__ __host__
#include "TensorOps.h"
#include <cuda.h>
#include <cuda_runtime.h>
#include "cublas_v2.h"
#include <assert.h>

#ifndef let
#define let const auto
#endif

#pragma comment (lib, "cudart.lib")     // instruct linker to reference these libs
#pragma comment (lib, "cublas.lib")

#pragma warning (disable: 4267) // conversion from 'size_t' to 'unsigned int'; happens in CUDA <<<a,b>>> syntax if a and b are size_t
#pragma warning (disable: 4127) // conditional expression is constant; "if (sizeof(ElemType)==sizeof(float))" triggers this
#pragma warning (disable: 4702) // unreachable code; triggered for unknown reasons

extern bool do_sync;

#ifdef _WIN32
// thread local storage to access the current stream, initalize to default stream
__declspec (thread)
#endif
extern cudaStream_t t_stream;

namespace Microsoft { namespace MSR { namespace CNTK {

    // =======================================================================
    // TensorView support
    // =======================================================================

    // To save time, this makes extensive use of templates and macros.

    // -----------------------------------------------------------------------
    // simple fixed-size arrays for passing dimension information by value
    // since CUDA can't just take our std::array and std::vector
    // -----------------------------------------------------------------------

    template<typename T, size_t N>
    struct FixedArray
    {
        T m_data[N];
        __device__ __host__ size_t size() const { return N; }
        __device__ __host__ T & operator[](size_t n)       { return m_data[n]; }
        __device__ __host__ T   operator[](size_t n) const { return m_data[n]; }
        template<class VEC> FixedArray(const VEC & data)    // construct from CPU-side STL array or vector
        {
            assert(data.size() == N);
            for (size_t n = 0; n < N; n++)
            {
                m_data[n] = (T)data[n];
                if (m_data[n] != data[n])   // overflow check
                    InvalidArgument("FixedArray: Dimensions out of range, too few bits.");
            }
        }
    };
    template<typename T>        // specialized version for 0 elements
    struct FixedArray<T, 0>
    {
        __device__ __host__ size_t size() const { return 0; }
        template<class VEC> FixedArray(const VEC & data) { assert(data.size() == 0); UNUSED(data); }
    };

    template<typename T, size_t N, size_t K>    // N = which input/output; K = index depth
    struct FixedMatrix
    {
        T m_data[N][K];
        __device__ __host__ size_t getNumRows() const { return N; }
        __device__ __host__ size_t getNumCols() const { return K; }
        __device__ __host__ T & operator()(size_t n, size_t k)       { return m_data[n][k]; }
        __device__ __host__ T   operator()(size_t n, size_t k) const { return m_data[n][k]; }
        template<typename U> FixedMatrix(const array<SmallVector<U>, N> & data)  // construct from CPU-side array of vectors
        {
            assert(data.size() == N);
            for (size_t n = 0; n < N; n++)
            {
                assert(data[n].size() == K);
                for (size_t k = 0; k < K; k++)
                {
                    m_data[n][k] = (T)data[n][k];
                    if (m_data[n][k] != data[n][k])   // overflow check
                        InvalidArgument("FixedArray: Dimensions out of range, too few bits.");
                }
            }
        }
    };
    template<typename T, size_t N>        // specialized version for 0 elements
    struct FixedMatrix<T, N, 0>
    {
        __device__ __host__ size_t getNumRows() const { return N; }
        __device__ __host__ size_t getNumCols() const { return 0; }
        template<typename U> FixedMatrix(const array<SmallVector<U>, N> & data) { assert(data.size() == N); for (size_t n = 0; n < N; n++) assert(data[n].size() == 0); UNUSED(data); }
    };

    // -----------------------------------------------------------------------
    // function to actually compute a function of (N-1) inputs based on the opcode
    // -----------------------------------------------------------------------

    template<class ElemType>
    struct TensorOps
    {
        static __device__ ElemType Compute(const FixedArray<ElemType*, 1> & pointers, ElementWiseOperator op)
        {
#define CaseNullaryTensorOp(oper) case ElementWiseOperator::op ## oper: return Op ## oper<ElemType>()
            switch (op)
            {
            ForAllNullaryOps(CaseNullaryTensorOp);
            default: return OpConstOne<ElemType>();   // (failure--we only have one nullary op, so use the same, maybe it will eliminate the switch altogether)
            }
        }
        static __device__ ElemType Compute(const FixedArray<ElemType*, 2> & pointers, ElementWiseOperator op)
        {
            ElemType a = *(pointers[0]);
#define CaseUnaryTensorOp(oper) case ElementWiseOperator::op ## oper: return Op ## oper(a)
            switch (op)
            {
            ForAllUnaryOps(CaseUnaryTensorOp);
            default: return 0;  // (failure)
            }
        }
        static __device__ ElemType Compute(const FixedArray<ElemType*, 3> & pointers, ElementWiseOperator op)
        {
            ElemType a = *(pointers[0]);
            ElemType b = *(pointers[1]);
#define CaseBinaryTensorOp(oper) case ElementWiseOperator::op ## oper: return Op ## oper(a,b)
            switch (op)
            {
            ForAllBinaryOps(CaseBinaryTensorOp);    // note: this costs about 6% compared to having only a single case
            default: return 0;  // (failure)
            }
        }
        static __device__ ElemType Compute(const FixedArray<ElemType*, 4> & pointers, ElementWiseOperator op)
        {
            ElemType a = *(pointers[0]);
            ElemType b = *(pointers[1]);
            ElemType c = *(pointers[2]);
#define CaseTernaryTensorOp(oper) case ElementWiseOperator::op ## oper: return Op ## oper(a,b,c)
            switch (op)
            {
            ForAllTernaryOps(CaseTernaryTensorOp);
            default: return 0;  // (failure)
            }
        }
    };

    // -----------------------------------------------------------------------
    // function to compute the value for a given output location (this version performs reduction if needed)
    // -----------------------------------------------------------------------

    template<class ElemType, C_size_t N, C_int M, C_int m>
    struct TensorOpReduce
    {
        // this version for m >= 0
        static __device__ ElemType Compute(FixedArray<ElemType*, N> pointers, ElementWiseOperator op,
                                           const FixedArray<C_unsigned_int, M> & reducingOpDims, const FixedMatrix<C_int, N, M> & reducingStrides)
        {
            // start with index 0
            // Using 'double' since we are memory-bound anyway.
            double/*ElemType*/ aggregate = TensorOpReduce<ElemType, N, M, m - 1>::Compute(pointers, op, reducingOpDims, reducingStrides);
            // apply this index to the pointers
            C_size_t dim = reducingOpDims[m];
            for (C_size_t k = 1/*done with k=0 already*/; k < dim; k++)
            {
                // bump the pointers
                for (C_size_t i = 0; i < N - 1; i++)    // N-1 because output is not used here
                    pointers[i] += reducingStrides(i,(C_size_t)m);
                ElemType val = TensorOpReduce<ElemType, N, M, m - 1>::Compute(pointers, op, reducingOpDims, reducingStrides);
                aggregate += val;
            }
            return (ElemType)aggregate;
        }
    };

    // this one terminates the template recursion over reduction dimensions
    // The pointers are pointing to the input element.
    template<class ElemType, C_size_t N, C_int M>
    struct TensorOpReduce<ElemType, N, M, /*m=*/-1>
    {
        // this version for m = -1
        // the pointers are pointing to the right location(s) to take the operation over
        static __device__ ElemType Compute(FixedArray<ElemType*, N> pointers, ElementWiseOperator op,
                                           const FixedArray<C_unsigned_int, M> & /*reducingOpDims*/, const FixedMatrix<C_int, N, M> & /*reducingStrides*/)
        {
            return TensorOps<ElemType>::Compute(pointers, op);   // finally computing something!
        }
    };

    // -----------------------------------------------------------------------
    // function to compute one constituent of the value for a given output location (this version has reduction done outside)
    // -----------------------------------------------------------------------

    template<class ElemType, C_size_t N, C_int M, C_int m>
    struct TensorOpParallelReduce
    {
        // this version for m >= 0
        static __device__ ElemType Compute(CUDA_LONG id, FixedArray<ElemType*, N> pointers, ElementWiseOperator op,
                                           const FixedArray<C_unsigned_int, M> & reducingOpDims, const FixedMatrix<C_int, N, M> & reducingStrides)
        {
            // map id (location on grid) to index[k]
            C_size_t stride = 1;                    // compute the stride. This seems expensive, but since we we only currently support M <= 2, this is just compile-time selection between 1 and reducingOpDims[0].
            for (int i = 0; i < m; i++)
                stride *= reducingOpDims[(C_size_t)i];
            C_size_t index = id / stride;           // this dimension. For m=0, the stride is 1 and hence the division will be removed at compile time.
            id = id % stride;                       // remaining dimensions inside this. For m=0 this value is ignored and hence not even computed.
            // apply this index to the pointers
            for (C_size_t i = 0; i < N; i++)
                pointers[i] += index * reducingStrides(i, (C_size_t)m);    // now this dimension is taken care of
            return TensorOpParallelReduce<ElemType, N, M, m - 1>::Compute(id, pointers, op, reducingOpDims, reducingStrides);
        }
    };

    // this one terminates the template recursion over reduction dimensions
    // The pointers are pointing to the input element.
    template<class ElemType, C_size_t N, C_int M>
    struct TensorOpParallelReduce<ElemType, N, M, /*m=*/-1>
    {
        // this version for m = -1
        // the pointers are pointing to the right location(s) to take the operation over
        static __device__ ElemType Compute(CUDA_LONG /*id*/, FixedArray<ElemType*, N> pointers, ElementWiseOperator op,
                                           const FixedArray<C_unsigned_int, M> & /*reducingOpDims*/, const FixedMatrix<C_int, N, M> & /*reducingStrides*/)
        {
            return TensorOps<ElemType>::Compute(pointers, op);   // finally computing something!
        }
    };

    // -----------------------------------------------------------------------
    // perform loop over regular index k for N-nary operations (N counting the output)
    // -----------------------------------------------------------------------

    // The canonical case, vector op without reduction, is this PTX function:
    // _ZN9Microsoft3MSR4CNTK15_launchTensorOpIfLi3ELi0ELi1EEEvT_NS1_10FixedArrayIPS3_XT0_EEES3_NS1_19ElementWiseOperatorENS4_IiXT2_EEENS1_11FixedMatrixIiXT0_EXT2_EEENS4_IiXT1_EEENS9_IiXT0_EXT1_EEEi
    //                                   float ^      ^ aggregate loop
    //                                      args? ^       ^ input dims
    // _ZN9Microsoft3MSR4CNTK15_launchTensorOpIfLi2ELi0ELi1EEEvT_NS1_10FixedArrayIPS3_XT0_EEES3_NS1_19ElementWiseOperatorENS4_IiXT2_EEENS1_11FixedMatrixIiXT0_EXT2_EEENS4_IiXT1_EEENS9_IiXT0_EXT1_EEEi

    // The 'pointers' only refer to a single element, so we will bump them in-place to perform indexing.
    template<class ElemType, C_size_t N, C_int M, C_int K, bool parallelReduce, C_int k>
    struct TensorOpElement
    {
        // template-recursive version loops over indices
        static __device__ void Compute(CUDA_LONG id, ElemType beta, FixedArray<ElemType*, N> & pointers, ElemType alpha, ElementWiseOperator op,
                                       const FixedArray<C_unsigned_int, K> & regularOpStrides,  const FixedMatrix<C_int, N, K> & regularStrides,
                                       const FixedArray<C_unsigned_int, M> & reducingOpDims, const FixedMatrix<C_int, N, M> & reducingStrides, CUDA_LONG reductionDim)
        {
            // map id (location on grid) to index[k]
            C_size_t stride = regularOpStrides[(C_size_t)k];
            C_size_t index = id / stride;             // this dimension
            id = id % stride;                       // remaining dimensions inside this
            // apply this index to the pointers
            for (C_size_t i = 0; i < N; i++)
                pointers[i] += index * regularStrides(i,(C_size_t)k);    // now this dimension is taken care of
            // process the previous index
            TensorOpElement<ElemType, N, M, K, parallelReduce, k - 1>::Compute(id, beta, pointers, alpha, op, regularOpStrides, regularStrides, reducingOpDims, reducingStrides, reductionDim);
        }
    };

    // specialization for k=0 where op stride is guaranteed to be 1
    template<class ElemType, C_size_t N, C_int M, C_int K, bool parallelReduce>
    struct TensorOpElement<ElemType, N, M, K, parallelReduce, /*k=*/0>
    {
        // template-recursive version loops over indices
        static __device__ void Compute(CUDA_LONG id, ElemType beta, FixedArray<ElemType*, N> & pointers, ElemType alpha, ElementWiseOperator op,
                                       const FixedArray<C_unsigned_int, K> & regularOpStrides,  const FixedMatrix<C_int, N, K> & regularStrides,
                                       const FixedArray<C_unsigned_int, M> & reducingOpDims,    const FixedMatrix<C_int, N, M> & reducingStrides, CUDA_LONG reductionDim)
        {
            // map id (location on grid) to index[k]
            C_size_t index = id;                      // this dimension
            // apply this index to the pointers
            for (C_size_t i = 0; i < N; i++)
                pointers[i] += index * regularStrides(i,0);    // now this dimension is taken care of
            // process the previous index
            TensorOpElement<ElemType, N, M, K, parallelReduce, -1>::Compute(/*id*/0, beta, pointers, alpha, op, regularOpStrides, regularStrides, reducingOpDims, reducingStrides, reductionDim);
        }
    };

    // apply beta and alpha and save
    template<class ElemType, class PointersType>
    static __device__ void SetFinalValue(ElemType val, ElemType beta, const PointersType & pointers, ElemType alpha)
    {
        // scale
        val *= alpha;
        // combine with previous value in target matrix, then write it out
        auto * pout = pointers[pointers.size() - 1];
        if (beta != 0)
            val += beta * *pout;
        // save
        *pout = val;
    }

    // specialization for k = -1 terminates the template recursion, and computes reductions in a for loop
    template<class ElemType, C_size_t N, C_int M, C_int K>
    struct TensorOpElement<ElemType, N, M, K, /*parallelReduce=*/false, /*k=*/-1>
    {
        // template-recursion-teminating version computes the actual value for this output location
        // now the output pointers point to the right element (input pointers may still iterate for reduction)
        static __device__ void Compute(CUDA_LONG /*id*/, ElemType beta, FixedArray<ElemType*, N> & pointers, ElemType alpha, ElementWiseOperator op,
                                       const FixedArray<C_unsigned_int, K> & /*regularOpStrides*/, const FixedMatrix<C_int, N, K> & /*regularStrides*/,
                                       const FixedArray<C_unsigned_int, M> & reducingOpDims,       const FixedMatrix<C_int, N, M> & reducingStrides, CUDA_LONG /*reductionDim*/)
        {
            // compute the operation for this output coordinate
            // This may still involve a reduction over inverse-broadcasting dimensions.
            ElemType val = TensorOpReduce<ElemType, N, M, M - 1>::Compute(pointers, op, reducingOpDims, reducingStrides);
            // and save the final value
            SetFinalValue(val, beta, pointers, alpha);
        }
    };

    // specialization for k = -1 terminates the template recursion, and computes reductions in parallel
    template<class ElemType, C_size_t N, C_int M, C_int K>
    struct TensorOpElement<ElemType, N, M, K, /*parallelReduce=*/true, /*k=*/-1>
    {
        // template-recursion-teminating version computes the actual value for this output location
        // now the output pointers point to the right element (input pointers may still iterate for reduction)
        static __device__ void Compute(CUDA_LONG /*id*/, ElemType beta, FixedArray<ElemType*, N> & pointers, ElemType alpha, ElementWiseOperator op,
                                       const FixedArray<C_unsigned_int, K> & /*regularOpStrides*/, const FixedMatrix<C_int, N, K> & /*regularStrides*/,
                                       const FixedArray<C_unsigned_int, M> & reducingOpDims,       const FixedMatrix<C_int, N, M> & reducingStrides, CUDA_LONG reductionDim)
        {
            CUDA_LONG tid = threadIdx.x;                    // note: blockDim.x might be out of bounds w.r.t. tid in case we split reduction
            let reductionBlock = blockIdx.z;                // larger reductions are split into blocks
            let redId0 = reductionBlock * blockDim.x;       // first redId for this block

            let tidEnd = min(blockDim.x, reductionDim - redId0);    // last block is partial

            // accumulator
            __shared__ double accumulators[GridDim::maxThreadsPerBlock];

            // compute the operation for this input coordinate
            if (tid < tidEnd)
            {
                let redId = redId0 + tid;                   // redId: linearized thread index which gets mapped to actual coordinates
                accumulators[tid] = TensorOpParallelReduce<ElemType, N, M, M - 1>::Compute(redId, pointers, op, reducingOpDims, reducingStrides);
            }

            // reduce    --cf https://docs.nvidia.com/cuda/samples/6_Advanced/reduction/doc/reduction.pdf
            __syncthreads();
            static_assert(GridDim::maxThreadsPerBlock <= 512, "GridDim::maxThreadsPerBlock too large, need to add manually unrolled steps");
            if (tid < 256 && tid + 256 < tidEnd) accumulators[tid] += accumulators[tid + 256]; if (0 + 256 < tidEnd) __syncthreads();   // sync if condition true for at least one thread
            if (tid < 128 && tid + 128 < tidEnd) accumulators[tid] += accumulators[tid + 128]; if (0 + 128 < tidEnd) __syncthreads();
            if (tid < 64  && tid + 64  < tidEnd) accumulators[tid] += accumulators[tid + 64];  if (0 + 64  < tidEnd) __syncthreads();
            if (tid < 32  && tid + 32  < tidEnd) accumulators[tid] += accumulators[tid + 32];  if (0 + 32  < tidEnd) __syncthreads();   // somehow I still need to sync, contradicting the PDF
            if (tid < 16  && tid + 16  < tidEnd) accumulators[tid] += accumulators[tid + 16];  if (0 + 16  < tidEnd) __syncthreads();   // TODO: Understand this!
            if (tid < 8   && tid + 8   < tidEnd) accumulators[tid] += accumulators[tid + 8];                         __syncthreads();
            if (tid < 4   && tid + 4   < tidEnd) accumulators[tid] += accumulators[tid + 4];                         __syncthreads();
            if (tid < 2   && tid + 2   < tidEnd) accumulators[tid] += accumulators[tid + 2];                         __syncthreads();
            if (tid < 1   && tid + 1   < tidEnd) accumulators[tid] += accumulators[tid + 1];

            // now set final value to output coordinate
            if (tid == 0)
            {
                ElemType val = (ElemType)accumulators[0];
                pointers[pointers.size() - 1] += reductionBlock * reductionDim;     // if we are using temp memory then each block has its own memory to write to
                // BUGBUG: No, this should be numElements            ^^
                SetFinalValue(val, beta, pointers, alpha);
            }
        }
    };

    // -----------------------------------------------------------------------
    // kernel and launch
    // -----------------------------------------------------------------------

    // the top-level kernel
    template<class ElemType, C_size_t N, C_int M, C_int K>
    __global__ void _launchTensorOp(ElemType beta, FixedArray<ElemType*, N> pointers, ElemType alpha, ElementWiseOperator op,
                                    FixedArray<C_unsigned_int, K> regularOpStrides, FixedMatrix<C_int, N, K> regularStrides,  CUDA_LONG numElements,
                                    FixedArray<C_unsigned_int, M> reducingOpDims,   FixedMatrix<C_int, N, M> reducingStrides, CUDA_LONG reductionDim)
    {
        CUDA_LONG id = GridDim::GetLinearThreadId();
        if (id < numElements)       // note: there are no __syncthread() calls inside
            TensorOpElement<ElemType, N, M, K, false, K - 1>::Compute(id, beta, pointers, alpha, op, regularOpStrides, regularStrides, reducingOpDims, reducingStrides, reductionDim);
    }
    template<class ElemType, C_size_t N, C_int M, C_int K>
    __global__ void _launchTensorOpParallelReduction(ElemType beta, FixedArray<ElemType*, N> pointers, ElemType alpha, ElementWiseOperator op,
                                                     FixedArray<C_unsigned_int, K> regularOpStrides, FixedMatrix<C_int, N, K> regularStrides,  CUDA_LONG numElements,
                                                     FixedArray<C_unsigned_int, M> reducingOpDims,   FixedMatrix<C_int, N, M> reducingStrides, CUDA_LONG reductionDim)
    {
        CUDA_LONG id = gridDim.y * blockIdx.x + blockIdx.y;   // input dimensions are Y dimension of blocks in this case, so we can use thread dim for shared-memory/parallelization
        if (id < numElements)       // note: we have __syncthread() calls but only entire blocks in sync, so this is OK
            TensorOpElement<ElemType, N, M, K, true, K - 1>::Compute(id, beta, pointers, alpha, op, regularOpStrides, regularStrides, reducingOpDims, reducingStrides, reductionDim);
    }

    // launch tensor op with CUDA
    // All dimensions (N-ariness, number of input dimensions K and number of reduction dimensions M) are bound to template parameters now.
    template<class ElemType, C_size_t N, C_int M, C_int K>
    static void LaunchTensorOp(ElemType beta, array<ElemType*, N> pointerVector, ElemType alpha, ElementWiseOperator op,
                               const SmallVector<size_t> & regularOpDims,       const array<SmallVector<ptrdiff_t>, N> & regularStrideVectors,
                               const SmallVector<size_t> & reducingOpDimVector, const array<SmallVector<ptrdiff_t>, N> & reducingStrideVectors)
    {
        // copy all parameters to CUDA-compatible data structures
        FixedArray<ElemType*, N> pointers(pointerVector);
        SmallVector<C_size_t> regularOpStrideVector;    // kernel needs the strides for converting thread index back to multi-dimensional tensor index
        C_size_t numElements = 1;
        for (C_size_t k = 0; k < regularOpDims.size(); k++)
        {
            regularOpStrideVector.push_back(numElements);
            numElements *= (C_size_t)regularOpDims[k];
        }
        FixedArray<C_unsigned_int, K> regularOpStrides(regularOpStrideVector);
        FixedMatrix<C_int, N, K> regularStrides(regularStrideVectors);
        FixedArray<C_unsigned_int, M> reducingOpDims(reducingOpDimVector);
        FixedMatrix<C_int, N, M> reducingStrides(reducingStrideVectors);

        // launch the kernel
        CUDA_LONG NN = (CUDA_LONG)numElements;      // linear space identifying each individual input element
        cudaEvent_t done = nullptr;
        if (do_sync)    CUDA_CALL(cudaEventCreate(&done));

        // do some optimization for reductions
        // Cases:
        //  - input elements >> GPU procs  -->  do reduction in inner loop
        //  - reduction dimension fits into a single kernel  -->  launch it that way
        //  - reduction dimension requires multiple kernels  -->  use atomic add, to avoid temp mem alloc  --is this any good?
        //     - PlusNode: reducing to a bias for small matrices
        //     - ScaleNode: big elementwise product reduced to a scalar (dot product)
#if 1
        C_size_t reductionDim = 1;  // number of elements to reduce over
        for (C_size_t k = 0; k < reducingOpDimVector.size(); k++)
            reductionDim *= (C_size_t)reducingOpDimVector[k];
        let & props = GridDim::GetDeviceProps();
        GridDim grid(NN);
        if (reductionDim > 1 && NN < props.multiProcessorCount * props.warpSize      && reductionDim <= GridDim::maxThreadsPerBlock)
        {
            let numInputBlocks = (NN + grid.m_blocksPerGrid - 1) / grid.m_blocksPerGrid;    // undo rounding-up to multiples of 32, since this is no longer used as a warp
            // TODO: We can now freely redistribute between .x and .y dimension.
            if (reductionDim <= GridDim::maxThreadsPerBlock)
            {
                // one thread block per reduction is sufficient
                // TODO: In the special case where reduction dim <= 16 (half warp size), we could fit more than one reduction.
                let blocksPerGrid   = dim3(grid.m_blocksPerGrid, numInputBlocks); // block Y is element dimension
                let threadsPerBlock = dim3(reductionDim);       // X dimension is reduction dimension
                _launchTensorOpParallelReduction<ElemType, N, M, K> << <blocksPerGrid, threadsPerBlock, reductionDim * sizeof(double), t_stream >> >(beta, pointers, alpha, op, regularOpStrides, regularStrides, grid.m_N, reducingOpDims, reducingStrides, reductionDim);
            }
            else
            {
                fprintf(stderr, "%d %d\n", (int)reductionDim, (int)props.multiProcessorCount * props.warpSize);
                // we need more than one block for each reduction. Temporary memory is required.
                auto numReductionBlocks = (reductionDim + GridDim::maxThreadsPerBlock - 1) / GridDim::maxThreadsPerBlock;   // #blocks used to compute each result element
                auto reductionDimPerBlock = (reductionDim + numReductionBlocks - 1) / numReductionBlocks;                    // #reduction dimensions computed in each block
                if (reducingStrideVectors[0] == 1)      // first stride is memory-consecutive: use warps of 32 for memory coalescing
                {
                    reductionDimPerBlock = (reductionDimPerBlock + 31) / 32 * 32;
                    numReductionBlocks = (reductionDim + reductionDimPerBlock - 1) / reductionDimPerBlock;  // (should this be the same?)
                }
                // TODO: limit the reduction blocks to #procs, or maybe 2 x, and then do an inner loop that just accumulates into the __shared__ memory
                let blocksPerGrid   = dim3(grid.m_blocksPerGrid, numInputBlocks, numReductionBlocks);       // block Y is element dimension, block Z is reduction block index
                let threadsPerBlock = dim3(reductionDimPerBlock);                                           // X dimension is reduction dimension
                // we use temporary memory: one tensor [numElements] per reduction block
                // Remember numElements is the number of elements excluding the reducing dimensions, i.e. the actual number of result elements to be produced.
                let numTempElems = numElements * numReductionBlocks;
                ElemType* tempBufferPtr = nullptr;
                CUDA_CALL(cudaMalloc((void**)&tempBufferPtr, sizeof(ElemType) * numTempElems));
                // redirect reduced values into the temp tensor
                //UNUSED(blocksPerGrid); UNUSED(threadsPerBlock);
#if 0
                auto tempStrideVectors = regularStrideVectors;  // we update the output strides to be flat and match our temp storage
                tempStrideVectors[N-1] = regularOpStrides;      // we condense all output into a dense flat matrix
                FixedMatrix<C_int, N, M> tempStrides(tempStrideVectors);
                auto tempPointerVector = pointerVector;
                tempPointerVector.back() = tempBufferPtr;
                FixedArray<ElemType*, N> tempPointers(tempPointerVector);
                // reduce into the temporary memory. Each reduction block's result gets redirected into a different matrix of dim [reducingOpDims]
                // too tired...
                _launchTensorOpParallelReduction<ElemType, N, M, K> << <blocksPerGrid, threadsPerBlock, reductionDimPerBlock * sizeof(double), t_stream >> >(beta, tempPointers, alpha, op, regularOpStrides, tempStrides, grid.m_N, reducingOpDims, reducingStrides, reductionDim);
#endif
                // reduce all those matrices into one
                // and done with the temp memory
                CUDA_CALL(cudaFree(tempBufferPtr));
            }
        }
        else
#endif
        {
            _launchTensorOp<ElemType, N, M, K> << <grid.m_blocksPerGrid, grid.m_threadsPerBlock, 0, t_stream >> >(beta, pointers, alpha, op, regularOpStrides, regularStrides, grid.m_N, reducingOpDims, reducingStrides, 1);
        }
        if (do_sync)    CUDA_CALL(cudaEventRecord(done));
        if (do_sync)    CUDA_CALL(cudaEventSynchronize(done));
        if (do_sync)    CUDA_CALL(cudaEventDestroy(done));
    }

    // for linear unary ops, we need to define a functor for every function for use as a template parameter (lambda syntax doesn't work in CUDA 7)
    #define DefineUnaryTensorFunctor(oper) \
        struct Functor ## oper { template<class ElemType> static __device__ ElemType f(ElemType a) { return Op ## oper(a); } };
    ForAllUnaryOps(DefineUnaryTensorFunctor);

    // the top-level kernel for linear unary ops
    // Note: If we have a beta, we have 2 memory accesses, so this optimization may no longer be needed as we are memory-bound.
    template<class ElemType, class FN>
    __global__ void _launchUnaryTensorOp(ElemType beta, const ElemType * pa, ElemType * pb, ElemType alpha, CUDA_LONG numElements)
    {
        CUDA_LONG id = GridDim::GetLinearThreadId();
        if (id >= numElements)
            return;
        ElemType a = pa[id];
        ElemType val = FN::f(a);
        val *= alpha;
        if (beta != 0)
            val += beta * pb[id];
        pb[id] = val;
    }
    // version without beta and alpha
    template<class ElemType, class FN>
    __global__ void _launchUnaryTensorOp(const ElemType * pa, ElemType * pb, CUDA_LONG numElements)
    {
        CUDA_LONG id = GridDim::GetLinearThreadId();
        if (id >= numElements)
            return;
        ElemType a = pa[id];
        ElemType val = FN::f(a);
        pb[id] = val;
    }

    // special case of linear unary operation
    template<class ElemType>
    void LaunchUnaryTensorOp(ElemType beta, const ElemType * pa, ElemType * pb, ElemType alpha, ElementWiseOperator op, size_t regularOpDim)
    {
        CUDA_LONG NN = (CUDA_LONG)regularOpDim;

        #define CaseLaunchUnaryTensorOp(oper) case ElementWiseOperator::op ## oper: \
            if (beta == 0 && alpha == 1) \
                return _launchUnaryTensorOp<ElemType,Functor ## oper> << <grid.m_blocksPerGrid, grid.m_threadsPerBlock, 0, t_stream >> >(pa, pb, NN); \
            else \
                return _launchUnaryTensorOp<ElemType,Functor ## oper> << <grid.m_blocksPerGrid, grid.m_threadsPerBlock, 0, t_stream >> >(beta, pa, pb, alpha, NN);

        cudaEvent_t done = nullptr;
        if (do_sync)    CUDA_CALL(cudaEventCreate(&done));
        GridDim grid(NN);
        switch (op)
        {
        ForAllUnaryOps(CaseLaunchUnaryTensorOp);
        default: LogicError("LaunchTensorOp1: Unknown op code %d.", (int)op);
        }
        if (do_sync)    CUDA_CALL(cudaEventRecord(done));
        if (do_sync)    CUDA_CALL(cudaEventSynchronize(done));
        if (do_sync)    CUDA_CALL(cudaEventDestroy(done));
    }

    // -----------------------------------------------------------------------
    // map runtime parameters N to template parameters
    // -----------------------------------------------------------------------

    // tensor operation with k+1 dimensions (-1 means scalar)
    template<class ElemType, C_size_t N, C_int K>
    static void TensorOpWithRegularLoop(ElemType beta, const array<ElemType*, N> & pointers, ElemType alpha, ElementWiseOperator op,
                                        const SmallVector<size_t> & regularOpDims,  const array<SmallVector<ptrdiff_t>, N> & regularStrides,
                                        const SmallVector<size_t> & reducingOpDims, const array<SmallVector<ptrdiff_t>, N> & reducingStrides)
    {
        size_t dims = reducingOpDims.size();
        switch (dims)
        {
        case 2: return LaunchTensorOp<ElemType, N, 2, K>(beta, pointers, alpha, op, regularOpDims, regularStrides, reducingOpDims, reducingStrides);
        case 1: return LaunchTensorOp<ElemType, N, 1, K>(beta, pointers, alpha, op, regularOpDims, regularStrides, reducingOpDims, reducingStrides);
        case 0: return LaunchTensorOp<ElemType, N, 0, K>(beta, pointers, alpha, op, regularOpDims, regularStrides, reducingOpDims, reducingStrides);
        default: LogicError("TensorOp: %d non-flattened reduction dimensions are not supported.", (C_int)dims);
        }
    }

    // tensor operation, generalized in number of arguments
    // This function now expands into different k. It also eliminates the offsets by adding them to the pointers.
    template<class ElemType, C_size_t N>
    void TensorOpN(ElemType beta, array<ElemType*, N> pointers, ElemType alpha, ElementWiseOperator op,
                        const array<size_t, N> & offsets,
                        const SmallVector<size_t> & regularOpDims,  const array<SmallVector<ptrdiff_t>, N> & regularStrides,
                        const SmallVector<size_t> & reducingOpDims, const array<SmallVector<ptrdiff_t>, N> & reducingStrides)
    {
        for (C_size_t i = 0; i < N; i++)  // N = a small constant, this will be unrolled
            pointers[i] += offsets[i];
        size_t dims = regularOpDims.size();
        switch (dims)
        {
        case 4: return TensorOpWithRegularLoop<ElemType, N, 4>(beta, pointers, alpha, op, regularOpDims, regularStrides, reducingOpDims, reducingStrides);
        case 3: return TensorOpWithRegularLoop<ElemType, N, 3>(beta, pointers, alpha, op, regularOpDims, regularStrides, reducingOpDims, reducingStrides);
        case 2: return TensorOpWithRegularLoop<ElemType, N, 2>(beta, pointers, alpha, op, regularOpDims, regularStrides, reducingOpDims, reducingStrides);
        case 1: return TensorOpWithRegularLoop<ElemType, N, 1>(beta, pointers, alpha, op, regularOpDims, regularStrides, reducingOpDims, reducingStrides);
        case 0: return TensorOpWithRegularLoop<ElemType, N, 0>(beta, pointers, alpha, op, regularOpDims, regularStrides, reducingOpDims, reducingStrides);
        default: LogicError("TensorOp: %d non-flattened input dimensions are not supported.", (C_int)dims);
        }
    }

    //------------------------------------------------------------------------
    // explicit instantiations--these are being called from GPUMatrix.cu
    //------------------------------------------------------------------------

    template void TensorOpN<float,  2>(float beta, array<float*, 2> pointers, float alpha, ElementWiseOperator op,
                                       const array<size_t, 2> & offsets,
                                       const SmallVector<size_t> & regularOpDims,  const array<SmallVector<ptrdiff_t>, 2> & regularStrides,
                                       const SmallVector<size_t> & reducingOpDims, const array<SmallVector<ptrdiff_t>, 2> & reducingStrides);
    template void TensorOpN<float,  3>(float beta, array<float*, 3> pointers, float alpha, ElementWiseOperator op,
                                       const array<size_t, 3> & offsets,
                                       const SmallVector<size_t> & regularOpDims,  const array<SmallVector<ptrdiff_t>, 3> & regularStrides,
                                       const SmallVector<size_t> & reducingOpDims, const array<SmallVector<ptrdiff_t>, 3> & reducingStrides);
    template void TensorOpN<float,  4>(float beta, array<float*, 4> pointers, float alpha, ElementWiseOperator op,
                                       const array<size_t, 4> & offsets,
                                       const SmallVector<size_t> & regularOpDims,  const array<SmallVector<ptrdiff_t>, 4> & regularStrides,
                                       const SmallVector<size_t> & reducingOpDims, const array<SmallVector<ptrdiff_t>, 4> & reducingStrides);
    template void TensorOpN<double, 2>(double beta, array<double*, 2> pointers, double alpha, ElementWiseOperator op,
                                       const array<size_t, 2> & offsets,
                                       const SmallVector<size_t> & regularOpDims,  const array<SmallVector<ptrdiff_t>, 2> & regularStrides,
                                       const SmallVector<size_t> & reducingOpDims, const array<SmallVector<ptrdiff_t>, 2> & reducingStrides);
    template void TensorOpN<double, 3>(double beta, array<double*, 3> pointers, double alpha, ElementWiseOperator op,
                                       const array<size_t, 3> & offsets,
                                       const SmallVector<size_t> & regularOpDims,  const array<SmallVector<ptrdiff_t>, 3> & regularStrides,
                                       const SmallVector<size_t> & reducingOpDims, const array<SmallVector<ptrdiff_t>, 3> & reducingStrides);
    template void TensorOpN<double, 4>(double beta, array<double*, 4> pointers, double alpha, ElementWiseOperator op,
                                       const array<size_t, 4> & offsets,
                                       const SmallVector<size_t> & regularOpDims,  const array<SmallVector<ptrdiff_t>, 4> & regularStrides,
                                       const SmallVector<size_t> & reducingOpDims, const array<SmallVector<ptrdiff_t>, 4> & reducingStrides);

    template void LaunchUnaryTensorOp(float beta, const float * pa, float * pb, float alpha, ElementWiseOperator op, size_t regularOpDim);
    template void LaunchUnaryTensorOp(double beta, const double * pa, double * pb, double alpha, ElementWiseOperator op, size_t regularOpDim);

}}}

#endif // CPUONLY