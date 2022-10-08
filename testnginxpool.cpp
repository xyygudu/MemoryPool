#include "ngx_mem_pool.h"

#include <stdio.h>
#include <string.h>

typedef struct Data stData;

struct Data
{
    char *ptr;
    FILE *pfile;
};

void func1(void *p1)
{
    char *p = (char *)p1;
    printf("释放ptr内存\n");
    free(p);
}

void func2(void *p2)
{
    FILE *pf = (FILE*)p2;
    printf("关闭文件\n");
    fclose(pf);
}

int main()
{
    ngx_mem_pool mempool;  
    
    if (!mempool.ngx_create_pool(512)) // 可在构造函数中实现
    {
        printf("ngx_create_pool失败\n");
        return 0;
    }
    void *p1 = mempool.ngx_palloc(128);  // 从小内存池分配128字节
    if (p1 == nullptr)
    {
        printf("ngx_palloc分配128字节失败\n");
        return 0;
    }
    stData *p2 = (stData *)mempool.ngx_palloc(512);
    p2->ptr = (char *)malloc(12);   // 让p2的ptr指向外部资源
    strcpy(p2->ptr, "hello world");
    p2->pfile = fopen("data.txt", "w");

    ngx_pool_cleanup_s *c1 = mempool.ngx_pool_cleanup_add(sizeof(char *));
    c1->handler = func1;
    c1->data = p2->ptr;

    ngx_pool_cleanup_s *c2 = mempool.ngx_pool_cleanup_add(sizeof(FILE*));
    c2->handler = func2;
    c2->data = p2->pfile;
    // mempool.ngx_reset_pool();
    mempool.ngx_destroy_pool();   // 可在析构函数中实现

    return 0;

}