/*
 * huge_mmap.c - 用 MAP_HUGETLB 分配巨页，观察 HugePages_Free 下降
 *
 * 用法:
 *   huge_mmap [MB]
 *     MB : 分配大小（默认 4，= 2 个 2MB 巨页）
 *
 * 教学点:
 *   MAP_HUGETLB 让 mmap 直接拿 2MB 巨页（而不是 4KB 普通页）。
 *   分配后 /proc/meminfo 的 HugePages_Free 会减少（每 2MB 减 1）。
 *   对比 malloc（普通 4KB 页，HugePages_Free 不变），就是巨页的直观证据。
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/mman.h>

int main(int argc, char **argv)
{
	long mb = (argc > 1) ? atol(argv[1]) : 4;
	size_t size = (size_t)mb * 1024 * 1024;
	void *p;

	/* MAP_HUGETLB | MAP_ANONYMOUS：直接分配 2MB 巨页（不用文件） */
	p = mmap(NULL, size, PROT_READ | PROT_WRITE,
		 MAP_PRIVATE | MAP_ANONYMOUS | MAP_HUGETLB, -1, 0);
	if (p == MAP_FAILED) {
		perror("mmap MAP_HUGETLB");
		fprintf(stderr, "提示: 先 echo 8 > /proc/sys/vm/nr_hugepages\n");
		return 1;
	}

	memset(p, 0x5a, size);   /* touch：真正分配巨页 */
	printf("pid=%d mmap %ldMB hugepages (MAP_HUGETLB)\n", getpid(), mb);
	fflush(stdout);
	pause();                 /* 驻留，观察 HugePages_Free 变化 */
	return 0;
}
