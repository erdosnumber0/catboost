#include "stack_guards.h"
#include "stack_pool.h"
#include "stack_utils.h"

#include <util/generic/hash.h>

#ifdef _linux_
#include <unistd.h>
#endif


namespace NCoro::NStack {

    template<typename TGuard>
    class TPoolAllocator final : public IAllocator {
    public:
        explicit TPoolAllocator(const TPoolAllocatorSettings& settings);

        TArrayRef<char> GetStackWorkspace(void* stack, uint64_t size) noexcept override {
            return Guard_.GetWorkspace(stack, size);
        }
        bool CheckStackOverflow(void* stack) const noexcept override {
            return Guard_.CheckOverflow(stack);
        }
        bool CheckStackOverride(void* stack, uint64_t size) const noexcept override {
            return Guard_.CheckOverride(stack, size);
        }

    private: // methods
        NDetails::TStack DoAllocStack(uint64_t size, const char* name) override;
        void DoFreeStack(NDetails::TStack& stack) noexcept override;

    private: // data
        const TPoolAllocatorSettings PoolSettings_;
        const TGuard& Guard_;
        THashMap<uint64_t, TPool<TGuard>> Pools_; // key - stack size
    };

    template<typename TGuard>
    TPoolAllocator<TGuard>::TPoolAllocator(const TPoolAllocatorSettings& settings)
        : PoolSettings_(settings)
        , Guard_(GetGuard<TGuard>())
    {
#ifdef _linux_
        Y_VERIFY(sysconf(_SC_PAGESIZE) == PageSize);
#endif
    }

    template<typename TGuard>
    NDetails::TStack TPoolAllocator<TGuard>::DoAllocStack(uint64_t alignedSize, const char* name) {
        Y_ASSERT(alignedSize > Guard_.GetSize());

        auto pool = Pools_.find(alignedSize);
        if (pool == Pools_.end()) {
            Y_ASSERT(Pools_.size() < 1000); // too many different sizes for coroutine stacks
            auto [newPool, success] = Pools_.emplace(alignedSize, TPool<TGuard>{alignedSize, PoolSettings_, Guard_});
            Y_VERIFY(success, "Failed to add new coroutine pool");
            pool = newPool;
        }
        return pool->second.AllocStack(name);
    }

    template<typename TGuard>
    void TPoolAllocator<TGuard>::DoFreeStack(NDetails::TStack& stack) noexcept {
        auto pool = Pools_.find(stack.GetSize());
        Y_VERIFY(pool != Pools_.end(), "Attempt to free stack from another allocator");
        pool->second.FreeStack(stack);
    }

    // ------------------------------------------------------------------------
    //
    template<typename TGuard>
    class TSimpleAllocator : public IAllocator {
    public:
        explicit TSimpleAllocator();

        TArrayRef<char> GetStackWorkspace(void* stack, uint64_t size) noexcept override {
            return Guard_.GetWorkspace(stack, size);
        }
        bool CheckStackOverflow(void* stack) const noexcept override {
            return Guard_.CheckOverflow(stack);
        }
        bool CheckStackOverride(void* stack, uint64_t size) const noexcept override {
            return Guard_.CheckOverride(stack, size);
        }

    private: // methods
        NDetails::TStack DoAllocStack(uint64_t size, const char* name) override;
        void DoFreeStack(NDetails::TStack& stack) noexcept override;

    private: // data
        const TGuard& Guard_;
    };


    template<typename TGuard>
    TSimpleAllocator<TGuard>::TSimpleAllocator()
        : Guard_(GetGuard<TGuard>())
    {}

    template<typename TGuard>
    NDetails::TStack TSimpleAllocator<TGuard>::DoAllocStack(uint64_t alignedSize, const char* name) {
        Y_ASSERT(alignedSize > Guard_.GetSize());

        char* rawPtr = nullptr;
        char* alignedPtr = nullptr; // with extra space for previous guard in this type of allocator

        Y_VERIFY(GetAlignedMemory((alignedSize + Guard_.GetPageAlignedSize()) / PageSize, rawPtr, alignedPtr)); // + memory for previous guard
        char* alignedStackMemory = alignedPtr + Guard_.GetPageAlignedSize(); // after previous guard

        // Default allocator sets both guards, because it doesn't have memory chunk with previous stack and guard on it
        Guard_.Protect((void*)alignedPtr, Guard_.GetPageAlignedSize(), false); // first guard should be before stack memory
        Guard_.Protect(alignedStackMemory, alignedSize, true); // second guard is placed on stack memory

        return NDetails::TStack{rawPtr, alignedStackMemory, alignedSize, name};
    }

    template<typename TGuard>
    void TSimpleAllocator<TGuard>::DoFreeStack(NDetails::TStack& stack) noexcept {
        Guard_.RemoveProtection(stack.GetAlignedMemory() - Guard_.GetPageAlignedSize(), Guard_.GetPageAlignedSize());
        Guard_.RemoveProtection(stack.GetAlignedMemory(), stack.GetSize());

        free(stack.GetRawMemory());
        stack.Reset();
    }
}
