#pragma once

#include <stdlib.h>
#include<memory.h>
#include <stdint.h>

using ngx_uint_t = unsigned int;

struct ngx_pool_s;
// 清理函数（回调函数）的类型
typedef void(*ngx_pool_cleanup_pt) (void *data);
struct ngx_pool_cleanup_s
{
    ngx_pool_cleanup_pt handler;    // 外部资源回收函数指针
    void                *data;      // 指向外部需要释放的资源（比如文件描述符、堆上其他位置的资源），是传递给回调函数的参数
    ngx_pool_cleanup_s  *next;      // 将所有的要清理的操作串在一起
};

// 大内存的头部信息
struct ngx_pool_large_s
{
    ngx_pool_large_s    *next;      // 下一个大内存块地址
    void                *alloc;     // 保存该大块内存的起始地址
};


// 内存池分配小块内存的头部信息
struct ngx_pool_data_t
{
    u_char              *last;      // 该内存块空闲区域的起始地址
    u_char              *end;       // 该内存块的尾部地址
    ngx_pool_s          *next;      // 指向下一个内存块（或称内存池）
    ngx_uint_t          failed;     // 记录当前内存块内存分配失败次数（可能该内存块剩余可分配内存特小，导致分配失败）
};

// 小内存池的头部信息+管理成员信息
struct ngx_pool_s
{
    ngx_pool_data_t     d;          // 存储当前小内存块的使用情况
    size_t              max;        // 记录小块内存允许一次性分配给用户的最大的空间大小，如果需要分配的内存大于max，就需要使用大内存块
    ngx_pool_s          *current;   // 指向第一个可以分配内存的小内存块，可能前面的几个内存块都满了，所以这个就需要指向当前还有空间可分配的内存块
    ngx_pool_large_s    *large;     // 指向大块内存链表的入口
    ngx_pool_cleanup_s  *cleanup;   // 指向所有预置的清理操作（回调函数）的入口
};

/*
 * 疑问1：ngx_pagesize和NGX_DEFAULT_POOL_SIZE含义是什么，有什么区别
 * 疑问2：为什么ngx_align函数的参数中要2倍的sizeof(ngx_pool_large_s)而不是1倍
 * 疑问3：（ngx_reset_pool是否同时重置大内存块和小内存块）
 * 疑问4：NGX_ALIGNMENT和NGX_POOL_ALIGNMENT作用区别
 */
#define ngx_memzero(buf, n) (void) memset(buf, 0, n)        // buffer缓冲区清0
#define NGX_ALIGNMENT sizeof(unsigned long)                 // 小块内存分配时考虑内存分配字节对齐单位，32位和64位对齐字节不同
#define ngx_align(d, a) (((d) + (a - 1)) & ~(a - 1))        // 把数字d调整到最接近a的整数倍
// 把指针p调整到最接近a的整数倍
#define ngx_align_ptr(p, a)                           \    
    (u_char *) (((uintptr_t) (p) + ((uintptr_t) a - 1)) & ~((uintptr_t) a - 1))
const int ngx_pagesize = 4096;                              // 默认页面大小为4KB
const int NGX_MAX_ALLOC_FROM_POOL = ngx_pagesize - 1;       // 小内存池的最大可分配空间(注意是可分配给用户的空间而不是整个小内存块空间)
const int NGX_DEFAULT_POOL_SIZE = 16 * 1024;                // 默认内存池开辟的大小：16KB
const int NGX_POOL_ALIGNMENT = 16;                          // 内存池按照16字节对齐
// ngx小内存池最小的size调整成NGX_POOL_ALIGNMENT的整数倍
// （是小内存池的大小是NGX_POOL_ALIGNMENT的整数倍而不是小内存池中可分配内存是NGX_POOL_ALIGNMENT的整数倍）
// 小内存池的大小至少要装得下小内存池的头部sizeof(ngx_pool_s)和2倍大内存头部：2 * sizeof(ngx_pool_large_s)
const int NGX_MIN_POOL_SIZE = ngx_align((sizeof(ngx_pool_s) + 2 * sizeof(ngx_pool_large_s)), NGX_POOL_ALIGNMENT);

// 采用面向对象3来实现nginx内存池
class ngx_mem_pool
{
public:
    // 创建指定size大小的内存池，但是每个小内存池不超过一个页面大小
    bool ngx_create_pool(size_t size = NGX_DEFAULT_POOL_SIZE);
    // 考虑内存字节对齐，从内存池申请size大小的内存
    void *ngx_palloc(size_t size);
    // 不考虑内存字节对齐，从内存池申请size大小的内存
    void *ngx_pnalloc(size_t size);
    // 考虑内存字节对齐，从内存池申请size大小的内存，并且会初始化为0
    void *ngx_pcalloc(size_t size);
    // 释放大内存块（内存池）
    void ngx_pfree(void *p);
    // 内存重置函数
    void ngx_reset_pool();
    // 内存池的销毁操作
    void ngx_destroy_pool();
    // 添加回调清理操作函数(所有已经分配出去的大内存块所占用的外部资源的回调函数添加到链表中)
    ngx_pool_cleanup_s *ngx_pool_cleanup_add(size_t size);

private:

    ngx_pool_s *pool;               // 指向ngx内存池的入口指针

    // 小块内存分配
    void *ngx_palloc_small(size_t size, ngx_uint_t align);
    // 大块内存分配
    void *ngx_palloc_large(size_t size);
    // 分配小块内存池
    void *ngx_palloc_block(size_t size);
};

