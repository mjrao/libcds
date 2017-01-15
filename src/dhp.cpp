/*
    This file is a part of libcds - Concurrent Data Structures library

    (C) Copyright Maxim Khizhinsky (libcds.dev@gmail.com) 2006-2017

    Source code repo: http://github.com/khizmax/libcds/
    Download: http://sourceforge.net/projects/libcds/files/

    Redistribution and use in source and binary forms, with or without
    modification, are permitted provided that the following conditions are met:

    * Redistributions of source code must retain the above copyright notice, this
      list of conditions and the following disclaimer.

    * Redistributions in binary form must reproduce the above copyright notice,
      this list of conditions and the following disclaimer in the documentation
      and/or other materials provided with the distribution.

    THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
    AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
    IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
    DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE LIABLE
    FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
    DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR
    SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER
    CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY,
    OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
    OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
*/

#include <algorithm>
#include <vector>

#include <cds/gc/dhp_smr.h>
#include <cds/os/thread.h>

namespace cds { namespace gc { namespace dhp {

    namespace {
        void * default_alloc_memory( size_t size )
        {
            return new uintptr_t[( size + sizeof( uintptr_t ) - 1 ) / sizeof( uintptr_t )];
        }

        void default_free_memory( void* p )
        {
            delete[] reinterpret_cast<uintptr_t*>( p );
        }

        struct defaults {
            static size_t const c_extended_guard_block_size = 16;
        };

        void* ( *s_alloc_memory )( size_t size ) = default_alloc_memory;
        void( *s_free_memory )( void* p ) = default_free_memory;

        template <typename T>
        class allocator
        {
        public:
            typedef T   value_type;

            allocator() {}
            allocator( allocator const& ) {}
            template <class U>
            allocator( allocator<U> const& ) {}

            T* allocate( size_t nCount )
            {
                return reinterpret_cast<T*>( s_alloc_memory( sizeof( value_type ) * nCount ));
            }

            void deallocate( T* p, size_t /*nCount*/ )
            {
                s_free_memory( reinterpret_cast<void*>( p ));
            }
        };

    } // namespace

    /*static*/ CDS_EXPORT_API smr* smr::instance_ = nullptr;
    thread_local thread_data* tls_ = nullptr;

    CDS_EXPORT_API hp_allocator::~hp_allocator()
    {
        while ( guard_block* gp = static_cast<guard_block*>( free_list_.get())) {
            gp->~guard_block();
            s_free_memory( gp );
        }
    }

    CDS_EXPORT_API guard_block* hp_allocator::alloc()
    {
        guard_block* gb;
        auto block = free_list_.get();
        if ( block )
            gb = static_cast< guard_block* >( block );
        else {
            // allocate new block
            gb = new( s_alloc_memory( sizeof( guard_block ) + sizeof( guard ) * defaults::c_extended_guard_block_size )) guard_block;
            new ( gb->first() ) guard[defaults::c_extended_guard_block_size];
        }

        // links guards in the block
        guard* p = gb->first();
        for ( guard* last = p + defaults::c_extended_guard_block_size - 1; p != last; ++p ) {
            p->clear( atomics::memory_order_relaxed );
            p->next_ = p + 1;
        }
        p->next_ = nullptr;
        p->clear();

        return gb;
    }

    CDS_EXPORT_API retired_allocator::~retired_allocator()
    {
        while ( retired_block* rb = static_cast<retired_block*>( free_list_.get() ) ) {
            rb->~retired_block();
            s_free_memory( rb );
        }
    }

    CDS_EXPORT_API retired_block* retired_allocator::alloc()
    {
        retired_block* rb;
        auto block = free_list_.get();
        if ( block )
            rb = static_cast< retired_block* >( block );
        else {
            // allocate new block
            rb = new( s_alloc_memory( sizeof( retired_block ) + sizeof( retired_ptr ) * retired_block::c_capacity )) retired_block;
            new ( rb->first()) retired_ptr[retired_block::c_capacity];
        }

        rb->next_ = nullptr;
        return rb;
    }

    struct smr::thread_record: thread_data
    {
        atomics::atomic<thread_record*>     m_pNextNode; ///< next hazard ptr record in list
        atomics::atomic<cds::OS::ThreadId>  m_idOwner;   ///< Owner thread id; 0 - the record is free (not owned)
        atomics::atomic<bool>               m_bFree;     ///< true if record is free (not owned)

        thread_record( guard* guards, size_t guard_count )
            : thread_data( guards, guard_count )
            , m_bFree( false )
        {}
    };

    /*static*/ CDS_EXPORT_API thread_data* smr::tls()
    {
        assert( tls_ != nullptr );
        return tls_;
    }

    /*static*/ CDS_EXPORT_API void smr::set_memory_allocator(
        void* ( *alloc_func )( size_t size ),
        void( *free_func )( void * p )
    )
    {
        // The memory allocation functions may be set BEFORE initializing DHP SMR!!!
        assert( instance_ == nullptr );

        s_alloc_memory = alloc_func;
        s_free_memory = free_func;
    }

    /*static*/ CDS_EXPORT_API void smr::construct( size_t nInitialHazardPtrCount )
    {
        if ( !instance_ ) {
            instance_ = new( s_alloc_memory( sizeof( smr ))) smr( nInitialHazardPtrCount );
        }
    }

    /*static*/ CDS_EXPORT_API void smr::destruct( bool bDetachAll )
    {
        if ( instance_ ) {
            if ( bDetachAll )
                instance_->detach_all_thread();

            instance_->~smr();
            s_free_memory( instance_ );
            instance_ = nullptr;
        }
    }

    CDS_EXPORT_API smr::smr( size_t nInitialHazardPtrCount )
        : thread_list_( nullptr )
        , initial_hazard_count_( nInitialHazardPtrCount < 4 ? 16 : nInitialHazardPtrCount )
        , last_plist_size_( initial_hazard_count_ * 64 )
    {}

    CDS_EXPORT_API smr::~smr()
    {
        CDS_DEBUG_ONLY( const cds::OS::ThreadId nullThreadId = cds::OS::c_NullThreadId; )
        CDS_DEBUG_ONLY( const cds::OS::ThreadId mainThreadId = cds::OS::get_current_thread_id(); )

        thread_record* pHead = thread_list_.load( atomics::memory_order_relaxed );
        thread_list_.store( nullptr, atomics::memory_order_relaxed );

        thread_record* pNext = nullptr;
        for ( thread_record* hprec = pHead; hprec; hprec = pNext )
        {
            assert( hprec->m_idOwner.load( atomics::memory_order_relaxed ) == nullThreadId
                || hprec->m_idOwner.load( atomics::memory_order_relaxed ) == mainThreadId
                || !cds::OS::is_thread_alive( hprec->m_idOwner.load( atomics::memory_order_relaxed ) )
            );

            retired_array& retired = hprec->retired_;

            // delete retired data
            for ( retired_block* block = retired.list_head_; block && block != retired.current_block_; block = block->next_ ) {
                for ( retired_ptr* p = block->first(); p != block->last(); ++p )
                    p->free();
            }
            if ( retired.current_block_ ) {
                for ( retired_ptr* p = retired.current_block_->first(); p != retired.current_cell_; ++p )
                    p->free();
            }
            hprec->retired_.fini();
            hprec->hazards_.clear();

            pNext = hprec->m_pNextNode.load( atomics::memory_order_relaxed );
            hprec->m_bFree.store( true, atomics::memory_order_relaxed );
            destroy_thread_data( hprec );
        }
    }

    /*static*/ CDS_EXPORT_API void smr::attach_thread()
    {
        if ( !tls_ )
            tls_ = instance().alloc_thread_data();
    }

    /*static*/ CDS_EXPORT_API void smr::detach_thread()
    {
        thread_data* rec = tls_;
        if ( rec ) {
            tls_ = nullptr;
            instance().free_thread_data( static_cast<thread_record*>( rec ) );
        }
    }

    CDS_EXPORT_API void smr::detach_all_thread()
    {
        thread_record * pNext = nullptr;
        const cds::OS::ThreadId nullThreadId = cds::OS::c_NullThreadId;

        for ( thread_record * hprec = thread_list_.load( atomics::memory_order_relaxed ); hprec; hprec = pNext ) {
            pNext = hprec->m_pNextNode.load( atomics::memory_order_relaxed );
            if ( hprec->m_idOwner.load( atomics::memory_order_relaxed ) != nullThreadId ) {
                free_thread_data( hprec );
            }
        }
    }

    CDS_EXPORT_API smr::thread_record* smr::create_thread_data()
    {
        size_t const guard_array_size = sizeof( guard ) * initial_hazard_count_;

        /*
            The memory is allocated by contnuous block
            Memory layout:
            +--------------------------+
            |                          |
            | thread_record            |
            |         hazards_         +---+
            |         retired_         |   |
            |                          |   |
            |--------------------------|   |
            | hazard_ptr[]             |<--+
            |  initial HP array        |
            |                          |
            +--------------------------+
        */

        char* mem = reinterpret_cast<char*>( s_alloc_memory( sizeof( thread_record ) + guard_array_size ));
        return new( mem ) thread_record(
            reinterpret_cast<guard*>( mem + sizeof( thread_record ) ), initial_hazard_count_
        );
    }

    CDS_EXPORT_API void smr::destroy_thread_data( thread_record* pRec )
    {
        // all retired pointers must be freed
        pRec->~thread_record();
        s_free_memory( pRec );
    }

    CDS_EXPORT_API smr::thread_record* smr::alloc_thread_data()
    {
        thread_record * hprec = nullptr;
        const cds::OS::ThreadId nullThreadId = cds::OS::c_NullThreadId;
        const cds::OS::ThreadId curThreadId = cds::OS::get_current_thread_id();

        // First try to reuse a free (non-active) DHP record
        for ( hprec = thread_list_.load( atomics::memory_order_acquire ); hprec; hprec = hprec->m_pNextNode.load( atomics::memory_order_relaxed ) ) {
            cds::OS::ThreadId thId = nullThreadId;
            if ( !hprec->m_idOwner.compare_exchange_strong( thId, curThreadId, atomics::memory_order_relaxed, atomics::memory_order_relaxed ) )
                continue;
            hprec->m_bFree.store( false, atomics::memory_order_release );
            break;
        }
        
        if ( !hprec ) {
            // No HP records available for reuse
            // Allocate and push a new HP record
            hprec = create_thread_data();
            hprec->m_idOwner.store( curThreadId, atomics::memory_order_relaxed );

            thread_record* pOldHead = thread_list_.load( atomics::memory_order_relaxed );
            do {
                hprec->m_pNextNode.store( pOldHead, atomics::memory_order_relaxed );
            } while ( !thread_list_.compare_exchange_weak( pOldHead, hprec, atomics::memory_order_release, atomics::memory_order_acquire ) );
        }

        hprec->hazards_.init();
        hprec->retired_.init();

        return hprec;
    }

    CDS_EXPORT_API void smr::free_thread_data( thread_record* pRec )
    {
        assert( pRec != nullptr );
        //CDS_HAZARDPTR_STATISTIC( ++m_Stat.m_RetireHPRec )

        pRec->hazards_.clear();
        scan( pRec );
        help_scan( pRec );

        if ( pRec->retired_.empty() ) {
            pRec->retired_.fini();
            pRec->m_bFree.store( true, std::memory_order_release );
        }
        else {
            // Free all empty blocks
            retired_block* free_block = pRec->retired_.current_block_->next_;
            if ( free_block ) {
                pRec->retired_.current_block_->next_ = nullptr;
                while ( free_block ) {
                    retired_block* next = free_block->next_;
                    retired_allocator_.free( free_block );
                    free_block = next;
                    --pRec->retired_.block_count_;
                }
            }
        }

        pRec->m_idOwner.store( cds::OS::c_NullThreadId, atomics::memory_order_release );
    }

    namespace {
        typedef std::vector<void*, allocator<void*>> hp_vector;

        inline void copy_hazards( hp_vector& vect, guard const* arr, size_t size )
        {
            for ( guard const* end = arr + size; arr != end; ++arr ) {
                void* hp = arr->get();
                if ( hp )
                    vect.push_back( hp );
            }
        }

        inline size_t retire_data( hp_vector const& plist, retired_array& stg, retired_block* block, size_t block_size )
        {
            auto hp_begin = plist.begin();
            auto hp_end = plist.end();
            size_t count = 0;

            for ( retired_ptr* p = block->first(), *end = p + block_size; p != end; ++p ) {
                if ( cds_unlikely( std::binary_search( hp_begin, hp_end, p->m_p )))
                    stg.safe_push( p );
                else {
                    p->free();
                    ++count;
                }
            }

            return count;
        }

    } // namespace

    CDS_EXPORT_API void smr::scan( thread_data* pThreadRec )
    {
        thread_record* pRec = static_cast<thread_record*>( pThreadRec );

        hp_vector plist;
        size_t plist_size = last_plist_size_.load( std::memory_order_relaxed );
        plist.reserve( plist_size );

        // Stage 1: Scan HP list and insert non-null values in plist
        thread_record* pNode = thread_list_.load( atomics::memory_order_acquire );
        while ( pNode ) {
            if ( pNode->m_idOwner.load( std::memory_order_relaxed ) != cds::OS::c_NullThreadId ) {
                copy_hazards( plist, pNode->hazards_.array_, pNode->hazards_.initial_capacity_ );

                for ( guard_block* block = pNode->hazards_.extended_list_; block; block = block->next_ )
                    copy_hazards( plist, block->first(), defaults::c_extended_guard_block_size );
            }

            pNode = pNode->m_pNextNode.load( atomics::memory_order_relaxed );
        }

        // Store plist size for next scan() call (vector reallocation optimization)
        if ( plist.size() > plist_size )
            last_plist_size_.compare_exchange_weak( plist_size, plist.size(), std::memory_order_relaxed, std::memory_order_relaxed );

        // Sort plist to simplify search in
        std::sort( plist.begin(), plist.end() );

        // Stage 2: Search plist
        size_t free_count = 0;
        retired_block* last_block = pRec->retired_.current_block_;
        retired_ptr*   last_block_cell = pRec->retired_.current_cell_;

        pRec->retired_.current_block_ = pRec->retired_.list_head_;
        pRec->retired_.current_cell_ = pRec->retired_.current_block_->first();

        for ( retired_block* block = pRec->retired_.list_head_; block; block = block->next_ ) {
            bool const end_block = block == last_block;
            size_t const size = end_block ? last_block_cell - block->first() : retired_block::c_capacity;

            free_count += retire_data( plist, pRec->retired_, block, size );

            if ( end_block )
                break;
        }

        // If the count of freed elements is too small, increase retired array
        if ( free_count == 0 && last_block == pRec->retired_.list_tail_ && last_block_cell == last_block->last() )
            pRec->retired_.extend();
    }

    CDS_EXPORT_API void smr::help_scan( thread_data* pThis )
    {
        assert( static_cast<thread_record*>( pThis )->m_idOwner.load( atomics::memory_order_relaxed ) == cds::OS::get_current_thread_id() );

        const cds::OS::ThreadId nullThreadId = cds::OS::c_NullThreadId;
        const cds::OS::ThreadId curThreadId = cds::OS::get_current_thread_id();
        for ( thread_record* hprec = thread_list_.load( atomics::memory_order_acquire ); hprec; hprec = hprec->m_pNextNode.load( atomics::memory_order_relaxed ) )
        {
            // If m_bFree == true then hprec->retired_ is empty - we don't need to see it
            if ( hprec->m_bFree.load( atomics::memory_order_acquire ) ) {
                assert( hprec->retired_.empty() );
                continue;
            }

            // Owns hprec
            // Several threads may work concurrently so we use atomic technique
            {
                cds::OS::ThreadId curOwner = hprec->m_idOwner.load( atomics::memory_order_relaxed );
                if ( curOwner == nullThreadId || !cds::OS::is_thread_alive( curOwner ) ) {
                    if ( !hprec->m_idOwner.compare_exchange_strong( curOwner, curThreadId, atomics::memory_order_acquire, atomics::memory_order_relaxed ) )
                        continue;
                }
                else
                    continue;
            }

            // We own the thread record successfully. Now, we can see whether it has retired pointers.
            // If it has ones then we move to pThis that is private for current thread.
            retired_array& src = hprec->retired_;
            retired_array& dest = pThis->retired_;

            for ( retired_block* block = src.list_head_; block; block = block->next_ ) {
                retired_ptr* last = block == src.current_block_ ? src.current_cell_ : block->last();
                for ( retired_ptr* p = block->first(); p != last; ++p ) {
                    if ( !dest.push( *p ) )
                        scan( pThis );
                }

                if ( block == src.current_block_ )
                    break;
            }

            src.fini();
            hprec->m_bFree.store( true, atomics::memory_order_relaxed );
            hprec->m_idOwner.store( nullThreadId, atomics::memory_order_release );
        }

        scan( pThis );
    }

}}} // namespace cds::gc::dhp
