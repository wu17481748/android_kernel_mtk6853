# mm_osvelte 0.1.1 (2022-02-24)

---

Bug fixes:

* 修复在注册 vendor_hook 失败，打印导致 dump 的问题

  ```osvelte:
  pr_err("%s regist extra meminfo proc failed.\n");
  ```

Feature enhancements:

* **lowmem-dbg** 打印增加版本号

  ```osvelte:
  [  174.763430] osvelte: lowmem_dbg start osvelte v0.1.1 <<<<<<<
  ```
* **lowmem-dbg** 增加 zone 和 node 的使用打印，因为有遇到过 `isolated_file` 过大，导致内存分配过程中 `msleep` 的问题

  ```osvelte:
  [  174.763491] osvelte: active_anon:37064 inactive_anon:2092183 isolated_anon:0 active_file:120745 inactive_file:93988 isolated_file:0 unevictable:33154 dirty:150 writeback:0 slab_reclaimable:35687 slab_unreclaimable:106681 mapped:174055 shmem:5104 pagetables:28048 bounce:0 free:65242 free_pcp:3930 free_cma:3410
  [  174.763528] osvelte: DMA32 free:127856kB min:2896kB low:23148kB high:29356kB reserved_highatomic:0KB active_anon:26500kB inactive_anon:2077496kB active_file:79400kB inactive_file:11400kB unevictable:20kB writepending:32kB present:3143676kB managed:2484184kB mlocked:20kB pagetables:10880kB bounce:0kB free_pcp:8984kB local_pcp:1496kB free_cma:13680kB
  [  174.763559] osvelte: Normal free:133112kB min:10712kB low:85616kB high:108560kB reserved_highatomic:4096KB active_anon:121756kB inactive_anon:6291256kB active_file:403580kB inactive_file:364552kB unevictable:132596kB writepending:552kB
  ```
* **lowmem-dbg** 支持增加 zram 使用率打印

  ```osvelte:
  [  174.763609] osvelte: hybridswap same_pages: 118604 compr_data_size: 140730 pages_stored: 575892
  ```
* **sys-memstat** 支持打印 osvelte 版本号

  ```osvelte:
  OP520F:/ # cat /proc/osvelte/version
  osvelte v0.1.1 based on kernel-5.10
  ```
* **sys-memstat** 支持打印 zram 使用情况

  ```osvelte:
  OP520F:/ # cat /proc/osvelte/hybridswap_info
  SamePages:       5765648 kB
  ComprDataSize:    204724 kB
  PagesStored:     6551528 kB
  ```
