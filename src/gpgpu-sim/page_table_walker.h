#ifndef PAGE_TABLE_WALKER_H
#define PAGE_TABLE_WALKER_H

#include <unordered_set>
#include <unordered_map>
#include <vector>
#include <ctime>
#include <thread>
#include <thread>
#include <functional>
#include <queue>
#include <atomic>
#include <mutex>
#include <condition_variable>
#include "mem_fetch.h"

using ULL = unsigned long long;
constexpr ULL walker_cache_latency = 20;
constexpr ULL walker_cache_to_dram_latency = 100;
constexpr ULL pwc_entries = 16;
constexpr ULL walker_dram_latency = walker_cache_latency + walker_cache_to_dram_latency;

class gmmu_t;
class memory_space;

enum class WalkerState
{
    Finish,
    LongPML4,
    LongPDP,
    LongPD,
    LongPTE,
    PD,
    PTE
};

#define CASE_STR(x) case WalkerState::x : return #x; break; 

std::string to_string(WalkerState state) {
    switch (state)
    {
    CASE_STR(LongPML4);
    CASE_STR(LongPDP);
    CASE_STR(LongPD);
    CASE_STR(LongPTE);
    CASE_STR(PD);
    CASE_STR(PTE);
    default:
        break;
    }
    return "UNKNOWN_EVENT!";
}

class MemFetchWrapper
{
public:
    MemFetchWrapper(mem_fetch *mf, ULL pages) : mf_(mf), pending_pages_(pages) {}

    mem_fetch *GetMenFetch()
    {
        return mf_;
    }

    void DecrementPendingPages()
    {
        if (pending_pages_ > 0)
        {
            --pending_pages_;
        }
    }

    bool IsFinished()
    {
        return pending_pages_ == 0;
    }

private:
    mem_fetch *mf_;
    ULL pending_pages_;
};

template <bool is64Bit>
class PageTableWalker
{
public:
    PageTableWalker(gmmu_t *gmmu, MemFetchWrapper *mf, ULL vpn) : gmmu_(gmmu), mf_(mf), state_(is64Bit ? WalkerState::LongPML4 : WalkerState::PD), vpn_(vpn) {}

    virtual void Cycle(ULL current_cycle) = 0;
    virtual ~PageTableWalker() {

    }
    void WalkStep();

    WalkerState GetState()
    {
        return state_;
    }

    bool IsFinished()
    {
        return GetState() == WalkerState::Finish;
    }

    ULL GetVPN() {
        return vpn_;
    }

    MemFetchWrapper *GetMenFetchWrapper()
    {
        return mf_;
    }

private:
    WalkerState state_;
    MemFetchWrapper *mf_;
    gmmu_t *gmmu_;
    ULL vpn_;
};

template <>
void PageTableWalker<true>::WalkStep()
{
    switch (state_)
    {
    case WalkerState::LongPML4:
        state_ = WalkerState::LongPDP;
        break;
    case WalkerState::LongPDP:
        state_ = WalkerState::LongPD;
        break;
    case WalkerState::LongPD:
        state_ = WalkerState::LongPTE;
        break;
    case WalkerState::LongPTE:
        state_ = WalkerState::Finish;
        GetMenFetchWrapper()->DecrementPendingPages();
        break;
    default:
        assert(false);
        break;
    }
}

template <>
void PageTableWalker<false>::WalkStep()
{
    switch (state_)
    {
    case WalkerState::PD:
        state_ = WalkerState::PTE;
        break;
    case WalkerState::PTE:
        state_ = WalkerState::Finish;
        GetMenFetchWrapper()->DecrementPendingPages();
        break;
    default:
        assert(false);
        break;
    }
}

template <bool is64Bit>
class LatencyPageTableWalker : public PageTableWalker<is64Bit>
{
private:
    class Cache
    {
    public:
        Cache(int capacity) : capacity_(capacity), dummy_(new Node)
        {
            this->dummy_->prev = this->dummy_->next = this->dummy_;
        }

        bool Has(ULL addr)
        {
            if (!m_.count(addr))
                return false;
            remove(m_[addr]);
            Node *node = new Node(addr);
            insert(dummy_->prev, node);
            m_[addr] = node;
            return true;
        }

        void Put(ULL addr)
        {
            Node *node = new Node(addr);
            insert(dummy_->prev, node);
            m_[addr] = node;
            if (m_.size() > capacity_)
                remove(dummy_->next);
        }

    private:
        struct Node
        {
            Node *next;
            Node *prev;
            ULL key;
            Node() = default;
            Node(ULL key)
            {
                this->key = key;
            }
        };

        void remove(Node *node)
        {
            node->prev->next = node->next;
            node->next->prev = node->prev;
            m_.erase(node->key);
            delete node;
        }

        void insert(Node *prev, Node *node)
        {
            node->prev = prev;
            node->next = prev->next;
            prev->next = node;
            node->next->prev = node;
        }

        std::unordered_map<ULL, Node *> m_;
        ULL capacity_;
        Node *dummy_;
    };

public:
    LatencyPageTableWalker(gmmu_t *gmmu, MemFetchWrapper *mf, ULL vpn, ULL initial_cycle)
        : PageTableWalker(gmmu, mf, vpn), init_cycle_(initial_cycle), cycle_(initial_cycle), cache_pending_(true) {
            Stats::GetInstance().Inc("count", 1);
            logs_.push_back("vpn[" + vpn +  "] translation started at cycle " + initial_cycle + ":");
        }

    void Cycle(ULL current_cycle) override
    {
        if (IsFinished()) {
            return
        }

        ULL delta = current_cycle - cycle_;
        if (cache_pending_)
        {
            if (delta < walker_cache_latency) {
                return;
            }
                
            bool hit = LatencyPageTableWalker::GetCache().Has(GetMappedIndex());
            if (hit)
            {
                logs_.push_back(GetState() + "[" + GetMappedIndex() +  "]: cache hit, " + delta + " cycles");
                this->WalkStep();
                cycle_ = current_cycle;
            }

            // if cache hit, prepare next walk step, reset cache_pending_ to true
            // if cache miss, set cache_pending_ to false
            cache_pending_ = hit;
        }
        else
        {
            if (delta < walker_dram_latency) {
                return;
            }
            
            LatencyPageTableWalker::GetCache().Put(GetMappedIndex());
            logs_.push_back(GetState() + "[" + GetMappedIndex() +  "]: cache miss, " + delta + " cycles")
            this->WalkStep();
            cache_pending_ = true;
            cycle_ = current_cycle;
        }
    }

    ~LatencyPageTableWalker() override {
        Stats::GetInstance().Inc("cycle", cycle_ - init_cycle_);
        logs_.push_back("vpn[" + vpn +  "] translation finished at cycle " + cycle_ + ".");


        std::cout << "-----------------------------" << endl;
        for (auto &log: logs_) {
            std::cout << log << endl;
        }
    }

    ULL GetMappedIndex();
    
    static LatencyPageTableWalker::Cache &GetCache()
    {
        static LatencyPageTableWalker::Cache cache(pwc_entries);
        return cache;
    }

private:
    bool cache_pending_;
    ULL cycle_;
    ULL init_cycle_;
    std::vector<std::string> logs_;
};


template <>
ULL LatencyPageTableWalker<true>::GetMappedIndex()
{
    switch (GetState())
    {
    case WalkerState::LongPML4:
        return GetVPN() & (0b111111000ull << 27);
    case WalkerState::LongPDP:
        return GetVPN() & (0b111111000ull << 18);
    case WalkerState::LongPD:
        return GetVPN() & (0b111111000ull << 9);
    case WalkerState::LongPTE:
        return GetVPN() & (0b111111000ull);
    default:
        assert(false);
        return 0;
    }
}

template <>
ULL LatencyPageTableWalker<false>::GetMappedIndex()
{
    switch (GetState())
    {
    case WalkerState::PD:
        return GetVPN() & (0b1111110000ull << 10);
    case WalkerState::PTE:
        return GetVPN() & (0b1111110000ull);
    default:
        assert(false);
        return 0;
    }
}

// inline ULL get_block_offset(ULL addr)
// {
//     return addr >> 6;
// }

template <bool is64Bit>
class PageWalkerManager
{
public:
    using PageTableWalker_t = std::vector<PageTableWalker<is64Bit>>;

    PageWalkerManager(gmmu_t *gmmu) : gmmu_(gmmu) {        
    }

    ~PageWalkerManager() {
        std::cout << "-----------------------------" << endl;
        std::cout << "PW access count: " << Stats::GetInstance().Get("count") << endl;
        std::cout << "PW total cycles: " << Stats::GetInstance().Get("cycle") << endl;
        std::cout << "Avg PW cycles: " << (Stats::GetInstance().Get("cycle") / Stats::GetInstance().Get("count")) << endl;
        std::cout << "PTE access count: " << Stats::GetInstance().Get("pte_access_count") << endl;
        std::cout << "PTE access cache hit count: " << Stats::GetInstance().Get("pte_access_cache_hit_count") << endl;
        std::cout << "PTE access cache hit rate: " << (static_cast<double>(Stats::GetInstance().Get("pte_access_cache_hit_count")) / Stats::GetInstance().Get("pte_access_count"))<< endl;
        std::cout << "-----------------------------" << endl;
    }

    void Cycle(ULL current_cycle)
    {
        for (auto &walker : walkers_)
        {
            walker.Cycle(current_cycle);
        }
    }

    void StartWalk(mem_fetch *mf, ULL current_cycle)
    {
        ULL start_page = gmmu_->get_global_memory()->get_page_num(mf->get_addr());
        ULL end_page = gmmu_->get_global_memory()->get_page_num(mf->get_addr() + mf->get_access_size() - 1);
        MemFetchWrapper *mf_wrapper = new MemFetchWrapper(mf, end_page - start_page + 1);
        for (ULL vpn = start_page; vpn <= end_page; ++vpn)
        {
            walkers_.push_back(LatencyPageTableWalker<is64Bit>(gmmu_, mf_wrapper, vpn, current_cycle));
        }
    }

    std::vector<mem_fetch *> PopFinishedFetchs()
    {
        std::vector<mem_fetch *> fetchs;
        size_t i = 0;
        while (i < walkers_.size())
        {
            if (walkers_[i].GetState() == WalkerState::Finish)
            {
                std::swap(walkers_[i], walkers_.back());
                if (walkers_.back().GetMenFetchWrapper().IsFinished())
                {
                    fetchs.push_back(walkers_.back().GetMenFetchWrapper()->GetMenFetch());
                    delete walkers_.back().GetMenFetchWrapper();
                }
                walkers_.pop_back();
            }
            else
                ++i;
        }
        return fetchs;
    }

private:
    PageTableWalker_t walkers_;
    gmmu_t *gmmu_;
};

class Stats {
public:
    static Stats& GetInstance() {
        static Stats instance;
        return instance;
    }

    void Inc(const std::string &key, ULL val) {
        entries_[key] += val;
    }

    ULL Get(const std::string &key) {
        return entries_[key];
    }

private:
    Stats() {}
    std::unordered_map<std::string, ULL> entries_;
};

#endif