# MemoryPool
# Nginx内存池基本思想
nginx内存池分为小内存池和大内存池，各个内存块都是通过链表的形式链接起来，其具体结构图和字段含义如下（注意，下图每一个内存块就是一个内存池，也就是下面画了3个小内存池和3个大内存池）。在程序最开始时是没有没有大内存池的，只有一个小内存池/块。小内存块默认有64KB，但是用户一次性申请的空间不得超过一个页面，即4KB，这样做的好处是让每个小内存块可以多分配几个小内存空间给用户，可以尽可能的避免大的内存碎片。如果用户申请的大小超过了一个页面大小，就需要开辟大内存块给用户。
![image.png](https://cdn.nlark.com/yuque/0/2022/png/27222704/1665213598426-dafb5fda-0481-469d-abac-cf5d1e388a22.png#clientId=u16f8da3c-e434-4&crop=0&crop=0&crop=1&crop=1&from=paste&height=945&id=u282e8892&margin=%5Bobject%20Object%5D&name=image.png&originHeight=945&originWidth=1317&originalType=binary&ratio=1&rotation=0&showTitle=false&size=100471&status=done&style=none&taskId=u0719b382-d1d0-488c-9f70-1c936e84f1b&title=&width=1317)
```cpp
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

// 大内存的头部信息
struct ngx_pool_large_s
{
    ngx_pool_large_s    *next;      // 下一个大内存块地址
    void                *alloc;     // 保存该大块内存的起始地址
};
```
在用户需要申请内存空间时，需要经过以下几个步骤：

1. 根据申请大小，选择使用从大内存块还是小内存块为用户分配空间
2. 如果是小内存块，则从小内存块current指针指向的小内存块开始给用户分配空间。在小内存池刚初始化时，current就指向自身
3. 如果从current指向的小内存块节点开始，一直到链表结尾，都没有找到一个能为用户提供足够内存空间的节点，那就再创建一个小内存块节点，并将该新开辟的节点加入到链表中
4. 如果是大内存块，则申请一块大内存块空间返回给用户，并将该大内存的头部节点插入到大内存块头部信息链表中
# 几种情况分析
## 初始化状态
初始状态，为小内存块开辟指定大小的内存空间p，默认64KB，注意，小内存块的头部也要占用一部分空间，所以能够分配给用户使用的空间如下图所示的“未使用部分”
![image.png](https://cdn.nlark.com/yuque/0/2022/png/27222704/1665215798631-6aec442d-783c-43a3-8306-05af5990fbf7.png#clientId=u16f8da3c-e434-4&crop=0&crop=0&crop=1&crop=1&from=paste&height=630&id=u3ed7cfab&margin=%5Bobject%20Object%5D&name=image.png&originHeight=630&originWidth=1090&originalType=binary&ratio=1&rotation=0&showTitle=false&size=37996&status=done&style=none&taskId=u2e0afad5-d270-47cb-9589-0d0229cca75&title=&width=1090)
代码如下
```cpp
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
```
## 用户申请一块小于max的空间
下图展示了该内存块未使用空间足够，并且申请的空间小于max的情况时，内存池的变化
![image.png](https://cdn.nlark.com/yuque/0/2022/png/27222704/1665216350532-e828f200-3351-44e0-8e08-71bda9d06ad3.png#clientId=u16f8da3c-e434-4&crop=0&crop=0&crop=1&crop=1&from=paste&height=631&id=uc8c3559b&margin=%5Bobject%20Object%5D&name=image.png&originHeight=631&originWidth=1088&originalType=binary&ratio=1&rotation=0&showTitle=false&size=55673&status=done&style=none&taskId=uf3faa279-b1db-4b13-8002-a2568490f40&title=&width=1088)
代码展示如下，此时运行到第21行结束
```cpp
// 小块内存分配
void * ngx_mem_pool::ngx_palloc_small(size_t size, ngx_uint_t align)
{
    // 第一个参数是用户要申请的大小，第二个参数表示是否字节对齐
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
```

如果该内存块剩余空间不够了，那就进入下面的情况：
① 在开辟一个小内存池块以便为用户分配空间。
② 修改failed。failed用于记录该节点分配内存失败的次数（没有足够的空间可以分配给用户就当作一次失败），由于第一个小内存块无法分配足够空间给用户，所以该节点的failed次数加一，如果failed达到了4，表示这个节点剩余的内存空间可能不够了，所以就可以把current指针指向第二个内存池节点（由于下图failed还没达到4因此current指针仍然指向自己）。
③ 将新开辟的小内存块插入到链表中。
④ 将①申请的空间（即右图灰色“已使用”部分的内存空间）返回给用户，供用户使用。
![image.png](https://cdn.nlark.com/yuque/0/2022/png/27222704/1665217915247-4b4e28e0-7e21-4f20-82a1-62fdc7330ead.png#clientId=u16f8da3c-e434-4&crop=0&crop=0&crop=1&crop=1&from=paste&height=865&id=u94b36142&margin=%5Bobject%20Object%5D&name=image.png&originHeight=865&originWidth=1263&originalType=binary&ratio=1&rotation=0&showTitle=false&size=85500&status=done&style=none&taskId=u9f8d5428-9eca-4c55-b70e-bdab03978f8&title=&width=1263)
代码如下：
```cpp
// 开辟新的小块内存池
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

```
## 用户申请一块大于max的空间
此时，需要开辟大的内存空间，大内存块和小内存块不同，大内存块的头部和可分配空间不在一起，而小内存块的头部信息和能给用户使用的内存都在一起。大内存块的头部信息存储在小内存块中，并将头部信息的alloc指针指向可以分配给用户的大内存空间，如下图左边所示：假设第一个小内存块未使用空间已经不够了，而且分配失败次数已经达到了4（即第一个小内存块的节点的current指针指向了下一个小内存块节点），这意味这接下来所有为用户分配内存的事情都从第二个（current指针指向的节点）开始。
① 申请一块大内存空间
② 如下图右边所示在小内存池中申请一块空间用于存储大内存块的头部信息
③ 将大内存块的头部信息的alloc指向①申请的内存空间
④ 将large指针指向大内存空间的头部
![image.png](https://cdn.nlark.com/yuque/0/2022/png/27222704/1665220777155-3c8dccf6-c08c-4a9c-bc05-e2a41c2d926f.png#clientId=u16f8da3c-e434-4&crop=0&crop=0&crop=1&crop=1&from=paste&height=734&id=uc419153d&margin=%5Bobject%20Object%5D&name=image.png&originHeight=734&originWidth=1591&originalType=binary&ratio=1&rotation=0&showTitle=false&size=100125&status=done&style=none&taskId=u9a4eaef5-43a4-4fb9-81f8-cbd3f42c8d5&title=&width=1591)
代码如下：
```cpp
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
```
## 内存池重置
nginx仅仅提供了大内存的释放，而没有提供小内存的释放，因为小内存的每一节点都有太多碎片不好管理，所以只有等http连接关闭后才可能重置小内存池
**重置含义：**内存池重置也就是将已经分配给用户的内存回收到内存池中，并不是将内存池的内存释放掉，注意nginx仅仅回收小内存池中的内存，大内存块直接释放掉。
**重置顺序**：由于大内存块的头部存储在小内存块中，因此必须先释放大内存空间在回收小内存空间，否则，释放掉大内存的头部信息后，就会导致大内存块的内存无法释放，造成内存泄漏
**注意事项**：

1. 大内存块（即alloc指向的位置）可能申请了其他外部资源，比如打开了文件描述符，或者在堆上新开辟了其他空间（不属于内存池管理的空间），那么释放大内存块前，就需要先释放大内存块所占有的外部资源，防止内存泄漏（不知道为什么nginx没有这样做，而是在销毁的时候才这样做）
2. 小内存块链表的第一个节点和其他节点头部信息长度不一致，所以分开进行回收（nginx是统一回收的，这使得其他小内存节点重置后，下次就没有刚开辟时那么多的内存空间可用了，具体见下面代码）

如下图：重置后并每一个小内存块不会释放掉，只会重置，而大内存块则一一释放掉，large指针指控，current指针指向第一个内存块自己，failed清零。
![image.png](https://cdn.nlark.com/yuque/0/2022/png/27222704/1665227173311-21357345-7fb0-42d3-94bd-72e4ef4a245c.png#clientId=u4e30fc23-fe1d-4&crop=0&crop=0&crop=1&crop=1&from=paste&height=582&id=u654c35c7&margin=%5Bobject%20Object%5D&name=image.png&originHeight=728&originWidth=1749&originalType=binary&ratio=1&rotation=0&showTitle=false&size=90694&status=done&style=none&taskId=u3f4fcb42-7ea9-448a-b7e3-1c987cc7d8e&title=&width=1399.2)
```cpp
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
    p = pool;
    // 由于第一个小内存块和头部和其他不同，所以单独处理（nginx没有单独处理）
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
```
## 销毁/释放内存池

1. 先释放大内存块占用的外部资源
2. 再释所有放大内存块
3. 最后释放所有小内存块

其中释放外部资源需要注册回调函数，即在销毁大内存块之前，要告诉内存池应该调用哪个函数来进行外部资源释放，比如第一个内存块打开了文件描述符，那就需要告诉内存池应该调用哪个函数来关闭文件描述符（见testnginxpool.cpp的48到54行），注册回调函数是`ngx_pool_cleanup_add`函数实现的，这里不过多讲解，销毁函数实现如下
```cpp
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
        if (c->handler)    							// handle就是释放外部资源要执行的回调函数，由用户给出
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
```
# 不足
个人认为nginx内存池有以下几点不足

1. 没有解决小内存池的内存碎片问题（或者说内存碎片仍需要优化）
2. 重置时，没有考虑其他小内存节点头部大小和第一个节点是不一样的，回收其他内存结点时，没有充分回收，本项目已经解决
3. 没有考虑多线程下的线程安全问题
4. 重置的时候没有考虑大内存块占用的外部资源释放，可能会导致内存泄漏
5. 重置内存池前，小内存块没有回收机制，如果内存一直申请会导致无内存可用（nginx提供了一个间接的解决方案，由于是http链接，nginx检查http连接，如果60秒都每发送消息，那就直接关闭连接，此时可以重置内存池，避免了一直开辟小内存块）
6. 重置并没有释放任何小内存块，如果之前小内存块开辟的太多（小内存块链表的节点太多），下次也用不完，那不是浪费了吗
# 疑问

1. 为什么在重置内存池时，只考虑了大内存块占用的外部资源的释放，而没有考虑小内存节点占用外部资源的释放情况
2. 大内存块每次都是使用时新开辟一个空间给用户（见ngx_palloc_large函数），而非像小内存池一样直接从已有的空间中拿一部分给用户，那这和用户直接调用malloc开辟出一个大内存块有什么分别呢，甚至还要处理大内存块头部字段，这不是更多次一举吗
3. ngx_palloc_small调用了ngx_palloc_block函数，其中判断小内存块是否剩余足够空间进行了两次，ngx_palloc_small的do while一次，ngx_palloc_block的for循环一次，这两次都是循环遍历小内存块链表，依次判断每个节点是否有足够的可分配空间，为什么要进行两次呢，一次不可以吗？

我觉得是可以合并的，只需要在ngx_palloc_small函数的`p = p->d.next;`后添加ngx_palloc_block的for循环中的if语句
不合并也行，ngx_palloc_small就专门负责找有没有可用的小内存块，至于这些内存块分配失败后的failed标识的自增就交给ngx_palloc_block

