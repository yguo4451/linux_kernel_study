/*
 * pagemap_demo.c — 验证用户态 malloc 地址能否查到物理地址
 *
 * 原理：/proc/self/pagemap 每个虚拟页对应 8 字节：
 *   bit 63      = present（该页是否已映射）
 *   bits 0~54   = 物理页帧号 PFN（需要 root/CAP_SYS_ADMIN 才能读到）
 *   物理地址    = PFN << 12 + 页内偏移
 *
 * 编译：容器内  gcc -static -o build/pagemap_demo experiments/memory/pagemap_demo.c
 * 运行：QEMU 内 ./pagemap_demo（需 root）
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/mman.h>

/* 查虚拟地址对应的物理地址；返回 0 表示该页尚未映射 */
static uint64_t virt_to_phys(void *vaddr)
{
    uint64_t entry = 0;
    uint64_t idx = ((uint64_t)vaddr / 4096) * 8;   /* 页号 × 8 字节 */

    int fd = open("/proc/self/pagemap", O_RDONLY);
    if (fd < 0) {
        perror("open /proc/self/pagemap");
        return 0;
    }
    if (pread(fd, &entry, 8, (off_t)idx) != 8) {
        perror("pread");
        close(fd);
        return 0;
    }
    close(fd);

    if (!(entry & (1ULL << 63)))          /* bit63 = present */
        return 0;                         /* 未映射，还没有物理页 */

    uint64_t pfn = entry & ((1ULL << 55) - 1);   /* 低 55 位 = PFN */
    return (pfn << 12) | ((uint64_t)vaddr & 0xFFF);   /* PFN<<12 + 页内偏移 */
}

int main(void)
{
    /* === 实验 1：mmap 一页，展示"访问前没有物理页" === */
    char *m = mmap(NULL, 4096, PROT_READ | PROT_WRITE,
                   MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (m == MAP_FAILED) { perror("mmap"); return 1; }

    printf("== 实验1：缺页机制 ==\n");
    printf("vaddr(mmap)   = %p\n", m);
    uint64_t phys = virt_to_phys(m);
    printf("访问前: %s\n", phys ? "已映射" : "NOT present（尚无物理页）");

    strcpy(m, "hello");
    phys = virt_to_phys(m);
    printf("写入后: phys = 0x%lx\n", phys);

    /* === 实验 2：malloc 两块，查物理地址是否连续 === */
    char *p1 = malloc(4096);
    char *p2 = malloc(4096);
    if (!p1 || !p2) { perror("malloc"); return 1; }

    strcpy(p1, "AAA");
    strcpy(p2, "BBB");

    uint64_t phys1 = virt_to_phys(p1);
    uint64_t phys2 = virt_to_phys(p2);

    printf("\n== 实验2：malloc 的物理地址 ==\n");
    printf("p1 vaddr = %p, phys = 0x%lx\n", p1, phys1);
    printf("p2 vaddr = %p, phys = 0x%lx\n", p2, phys2);

    /* 相邻虚拟页的物理页是否连续？ */
    if (phys1 && phys2) {
        uint64_t diff = phys1 > phys2 ? phys1 - phys2 : phys2 - phys1;
        printf("|phys1 - phys2| = 0x%lx (%s)\n", diff,
               diff == 4096 ? "恰好连续" : "不连续（物理页分散）");
    }

    /* === 实验 3：页内偏移一致性 === */
    printf("\n== 实验3：页内偏移 ==\n");
    printf("p1 vaddr 页内偏移 = 0x%lx\n", (uint64_t)p1 & 0xFFF);
    printf("p1 phys  页内偏移 = 0x%lx\n", phys1 & 0xFFF);
    printf("（两者相等说明物理地址 = PFN<<12 + 虚拟页内偏移）\n");

    free(p1);
    free(p2);
    munmap(m, 4096);
    return 0;
}
