#ifndef PAGE_TABLE_WALKER_H
#define PAGE_TABLE_WALKER_H

#include <unordered_set>
// #include <unordered_map>
#include <vector>
#include "mem_fetch.h"

using ULL = unsigned long long;
constexpr ULL walker_cache_latency = 0;
constexpr ULL walker_cache_to_dram_latency = 0;
// constexpr ULL walker_dram_latency = walker_cache_latency + walker_cache_to_dram_latency;

class gmmu_t;
class memory_space;

class PageTable
{
public:
    PageTable(gmmu_t *gmmu) : gmmu_(gmmu), ptes() {}
private:
    std::unordered_map<ULL, memory_space*> ptes;
    gmmu_t *gmmu_;
};

class PageTableWalker
{
public:
    enum class State
    {
        Ready,
        Finish,
        LongPML4,
        LongPDP,
        LongPD,
        LongPTE,
        PD,
        PTE
    };

    PageTableWalker(gmmu_t *gmmu, mem_fetch *mf) : gmmu_(gmmu), mf_(mf), state_(State::Ready) {}

    virtual void Cycle(ULL current_cycle) = 0;
    virtual void WalkStep() {
        switch (state_)
        {
        case State::Ready:
            /* code */
            break;
        case State::LongPML4:
            /* code */
            break;
        case State::LongPDP:
            /* code */
            break;
        case State::LongPD:
            /* code */
            break;
        case State::LongPTE:
            /* code */
            break;
        case State::PD:
            /* code */
            break;
        case State::PTE:
            /* code */
            break;
        default:
            assert(false);
            break;
        }
    };

    State GetState()
    {
        return state_;
    }

    mem_fetch *GetMenFetch()
    {
        return mf_;
    }

private:
    State state_;
    mem_fetch *mf_;
    gmmu_t *gmmu_;
};

class LatencyPageTableWalker : public PageTableWalker
{
public:
    LatencyPageTableWalker(gmmu_t *gmmu, mem_fetch *mf, ULL initial_cycle)
        : PageTableWalker(gmmu, mf), cycle_(initial_cycle), cache_pending_(true) {}


    void Cycle(ULL current_cycle) override
    {
        ULL delta = current_cycle - cycle_;
        if (cache_pending_) {
            if (delta < walker_cache_latency) return;

            bool hit = checkCache();
            if (hit) this->WalkStep();
            // if cache hit, prepare next walk step, reset cache_pending_ to true
            // if cache miss, set cache_pending_ to false
            cache_pending_ = hit;
        } else {
            if (delta < walker_cache_to_dram_latency) return;

            this->WalkStep();
            cache_pending_ = true;
        }
        cycle_ = current_cycle;
    }

    bool checkCache() {
        // TODO do check
        return false;
    }

    static std::unordered_set<ULL>& GetCache() {
        static std::unordered_set<ULL> cache;
        return cache;
    }
private:
    bool cache_pending_;
    ULL cycle_;
};

class PageWalkerManager
{
public:
    PageWalkerManager(gmmu_t *gmmu) : gmmu_(gmmu) {}

    void Cycle(ULL current_cycle)
    {
        for (PageTableWalker *walker : walkers_)
        {
            walker->Cycle(current_cycle);
        }
    }

    void StartWalk(mem_fetch *mf, ULL current_cycle) {
        walkers_.push_back(new LatencyPageTableWalker(gmmu_, mf, current_cycle));
    }

    std::vector<PageTableWalker *> PopFinishedWalkers()
    {
        std::vector<PageTableWalker *> finishedWalkers;
        size_t i = 0;
        while (i < walkers_.size())
        {
            if (walkers_[i]->GetState() == PageTableWalker::State::Finish)
            {
                std::swap(walkers_[i], walkers_.back());
                finishedWalkers.push_back(walkers_.back());
                walkers_.pop_back();
            }
            else
                ++i;
        }
        return finishedWalkers;
    }

private:
    std::vector<PageTableWalker *> walkers_;
    gmmu_t *gmmu_;
};

#endif