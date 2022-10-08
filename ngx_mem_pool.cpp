#include "ngx_mem_pool.h"

// 创建指定size大小的内存池，但是每个小内存池不超过一个页面大小
bool ngx_mem_pool::ngx_create_pool(size_t size)
{
    ngx_pool_s *p;
    p = (ngx_pool_s*)malloc(size);
    if (p == nullptr)
    {
        return false;
    }
    p->d.last = (u_char*)p + sizeof(ngx_pool_s);    // p->d.last的指向该小内存块（内存池）的头部节点的下一个位置
    p->d.end = (u_char*)p + size;                   // p->d.end指向小内存块的尾部的下一个节点
    p->d.next = nullptr;
    p->d.failed = 0;

    size = size - sizeof(ngx_pool_s);               // 小内存块总大小-小内存块头部大小 = 剩余可分配的大小
    // 初始化最大可分配的空间（不超过一个页面，即NGX_MAX_ALLOC_FROM_POOL=4KB）
    p->max = (size < NGX_MAX_ALLOC_FROM_POOL) ? size : NGX_MAX_ALLOC_FROM_POOL; 

    p->current = p;                                 // curreent指向第一个可以分配内存的小内存块,可分配内存的小内存块就是自己
    p->large = nullptr;                             // 暂时不需要大内存块
    p->cleanup = nullptr;

    pool = p;
    return true;
}


// 考虑内存字节对齐，从内存池申请size大小的内存
void *ngx_mem_pool::ngx_palloc(size_t size)
{
    if (size < pool->max)                           // 如果小内存块没有足够的空间分配给用户
    {
        return ngx_palloc_small(size, 1);           // 申请小内存块并进行字节对齐
    }
    return ngx_palloc_large(size);
}


// 不考虑内存字节对齐，从内存池申请size大小的内存
void *ngx_mem_pool::ngx_pnalloc(size_t size)
{
    if (size < pool->max)                           // 如果小内存块没有足够的空间分配给用户
    {
        return ngx_palloc_small(size, 0);           // 申请小内存块不进行字节对齐
    }
    return ngx_palloc_large(size);
}


// 考虑内存字节对齐，从内存池申请size大小的内存，并且会初始化为0
void *ngx_mem_pool::ngx_pcalloc(size_t size)
{
    void *p;
    p = ngx_palloc(size);
    if (p)
    {
        ngx_memzero(p, size);
    }

    return p;
}


// 释放大内存块（内存池）
void ngx_mem_pool::ngx_pfree(void *p)
{
    ngx_pool_large_s *l;
    for (l = pool->large; l; l = l->next)
    {
        if (p == l->alloc)
        {
            free(l->alloc);
            l->alloc = nullptr;
            return;
        }
    }
}


// 内存重置函数
void ngx_mem_pool::ngx_reset_pool()
{
    ngx_pool_s *p;
    ngx_pool_large_s *l;
    // 释放大内存块
    for (l = pool->large; l; l = l->next)
    {
        if (l->alloc)
        {
            free(l->alloc);
        }
    }
    // 由于第一个小内存块和头部和其他不同，所以单独处理
    p = pool;
    p->d.last = (u_char *)p + sizeof(ngx_pool_s);   
    p->d.failed = 0;

    // 第二块到最后一块小内存的重置
    for (p = p->d.next; p; p = p->d.next)
    {
        p->d.last = (u_char*)p + sizeof(ngx_pool_data_t);
        p->d.failed = 0;
    }

    pool->current = pool;
    pool->large = nullptr;
}


// 内存池的销毁操作
void ngx_mem_pool::ngx_destroy_pool()
{
    ngx_pool_s *p, *n;
    ngx_pool_large_s *l;
    ngx_pool_cleanup_s *c;

    // 遍历存储大内存块头部信息的链表，并依次执行他们的回调函数，
    // 从而释放该大内存（即ngx_pool_large_s的alloc指向的内存）申请的外部资源
    for (c = pool->cleanup; c; c = c->next)
    {
        if (c->handler)                             // handle就是释放外部资源要执行的回调函数，由用户给出
        {
            c->handler(c->data);
        }
    }

    for (l = pool->large; l; l = l->next)
    {
        if (l->alloc)
        {
            free(l->alloc);                         // 释放大内存空间
        }
    }

    for (p = pool, n = pool->d.next; ; p = n, n = n->d.next)
    {
        free(p);
        if (n == nullptr)
        {
            break;
        }
    }
}


// 添加回调清理操作函数(所有已经分配出去的大内存块所占用的外部资源的回调函数添加到链表中)
ngx_pool_cleanup_s *ngx_mem_pool::ngx_pool_cleanup_add(size_t size)
{
    ngx_pool_cleanup_s *c;
    c = (ngx_pool_cleanup_s*)ngx_palloc(sizeof(ngx_pool_cleanup_s));
    if (c == nullptr)
    {
        return nullptr;
    }

    if (size)
    {
        c->data = ngx_palloc(size);
        if (c->data == nullptr)
        {
            return nullptr;
        }
    }
    else
    {
        c->data = nullptr;
    }

    c->handler = nullptr;
    c->next = pool->cleanup;
    pool->cleanup = c;
    return c;
}



// 小块内存分配
void * ngx_mem_pool::ngx_palloc_small(size_t size, ngx_uint_t align)
{
    u_char *m;
    ngx_pool_s *p;
    p = pool->current;                              // 指向当前可分配内存的内存块

    do                                              // 从当前内存块开始一直往后找，直到找可以分配下size大小的内存块
    {
        m = p->d.last;                              // m指向该内存块p可分配内存的起始位置
        if (align)                                  // 如果要进行字节对齐，就将m调整到满足字节对齐的地址上
        {
            m = ngx_align_ptr(m, NGX_ALIGNMENT);
        }
        if ((size_t)(p->d.end - m) >= size)         // 如果剩余可分配空间够用
        {
            p->d.last = m + size;
            {
                p->d.last = m + size;               // 更新d.last到可分配的起始位置
                return m;
            }
        }
        p = p->d.next;                              // 如果剩余空间不够，则看看下一个内存块够不够
        // 该内存块无法分配内存为什么failed不++? failed++在ngx_palloc_block函数中
    } while (p);
    return ngx_palloc_block(size);                  // 遍历完了所有小内存块都没找到能够分配size大小的内存块，则重新分配一个小内存块
}


// 大块内存分配
void * ngx_mem_pool::ngx_palloc_large(size_t size)
{
    void *p;
    ngx_uint_t n;                                   // 记录已经使用了的大内存块个数
    ngx_pool_large_s *large;

    p = malloc(size);
    if (p == nullptr)
    {
        return nullptr;
    }

    n = 0;
    // 从大内存块的链表的第一个节点开始遍历，直到找到large->alloc为空的large
    // large->alloc为空可能是还没有大内存池，或者是alloc之前分配过但是后来释放后置空了
    for (large = pool->large; large; large = large->next)
    {
        if (large->alloc == nullptr)
        {
            large->alloc = p;
            return p;
        }

        if (n++ > 3) break;                         // 遍历比较耗时，如果遍历3次都没找到alloc为空的大内存块节点（即这几个大内存块目前都已经被用户利用了），就干脆不找了
    }

    // 在堆中申请一个存储大内存块头部信息的节点，该节点占用的空间是小内存中剩余可分配的空间
    large = (ngx_pool_large_s *)ngx_palloc_small(sizeof(ngx_pool_large_s), 1);
    if (large == nullptr)
    {
        free(p);
        return nullptr;
    }

    large->alloc = p;                               // 让刚得到存储大内存头部信息的节点的alloc指向刚分配的大内存空间
    // 将刚得到的头部信息节点插入到之前的头部信息链表中
    large->next = pool->large;                      
    pool->large = large;
    return p;
}


// 分配小块内存池
void * ngx_mem_pool::ngx_palloc_block(size_t size)
{
    u_char *m;
    size_t psize;
    ngx_pool_s *p, *new_pool;
    psize = (size_t)(pool->d.end - (u_char *)pool); // 新申请的页面大小 = end - 该内存块的起始位置pool，即保持新的内存块和之前的内存块大小一样
    m = (u_char*)malloc(psize);                     // m记录刚申请的内存空间的首地址
    if (m == nullptr)
    {
        return nullptr;
    }
    new_pool = (ngx_pool_s *)m;
    new_pool->d.end = m + size;
    new_pool->d.next = nullptr;
    new_pool->d.failed = 0;
    m += sizeof(ngx_pool_data_t);                   // 在该内存块中找到可分配部分的首地址（此时，m还不一定处于对齐字节位置；新的小内存块头部信息只有ngx_pool_data_t，比第一个内存块要少很多信息）
    m = ngx_align_ptr(m, NGX_ALIGNMENT);            // 将m调整到字节对齐的位置, 此时m是可以供用户使用的首地址
    new_pool->d.last = m + size;                    // 从m开始的size个字节都被使用了，因此，下一次用户从该内存块拿内存就要从m + size开始

    for (p = pool->current; p->d.next; p = p->d.next)
    {
        // -----------如果形参pool传进来的是类的私有变量，那这里的循环不相当于多遍历了一遍吗
        // -----------因为调用本函数的ngx_palloc_small已经遍历过一遍了，这里再次遍历不是多次一举吗，而且ngx_palloc_small遍历还没有给p->d.failed自增
        if (p->d.failed++ > 4)                      // 如果某个内存块4次分配内存都失败，说明这个内存块已经几乎没有剩余空间可以分配了
        {
            pool->current = p->d.next;              // 指向第一个可以分配内存的小内存块，由于当前内存块内存不够，只能将分配内存的希望寄托于下一个小内存块
        }
    }
    
    p->d.next = new_pool;
    return m;
}


